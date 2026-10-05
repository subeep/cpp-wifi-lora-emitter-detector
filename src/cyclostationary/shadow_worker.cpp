#include "shadow_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace rfmon::cyclo {
namespace {
template<class T> void update_max(std::atomic<T>& target, T value) noexcept {
    auto old = target.load();
    while (old < value && !target.compare_exchange_weak(old, value)) {}
}
}
ShadowWorker::~ShadowWorker() { set_enabled(false); }

std::size_t continuous_capture_prefix(const CaptureTiming& t, std::size_t received, bool overflow) noexcept {
    if (t.exception || t.overflows.size() > 4096 || (overflow && t.overflows.empty())) return 0;
    std::size_t prefix = received, previous = 0;
    for (const auto& gap : t.overflows) {
        if (gap.at_sample > received || gap.at_sample < previous) return 0;
        previous = gap.at_sample; prefix = std::min(prefix, gap.at_sample);
    }
    return prefix;
}

bool ShadowWorker::set_enabled(bool enable) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (enable && enabled_.load()) return true;
        // A faulted worker can be disabled but still have a joinable thread.
        // Join it before destruction or restart too.
        if (enable && thread_.joinable()) {
            { std::lock_guard<std::mutex> lock(queue_mutex_); stopping_ = true; }
            wake_.notify_all(); thread_.join();
            for (auto& s : slots_) s.state = State::free;
        }
        if (!enable) {
            enabled_ = false;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                stopping_ = true; ++epoch_;
                for (auto& s : slots_) if (s.state == State::queued) { s.state = State::free; ++discarded_; }
                std::atomic_store(&latest_, std::shared_ptr<const ShadowResult>{});
            }
            wake_.notify_all();
            if (thread_.joinable()) thread_.join();
            for (auto& s : slots_) { std::vector<std::complex<float>>().swap(s.iq); s.state = State::free; }
            return true;
        }
        // All allocation precedes enabled=true; the producer stays a no-op.
        for (auto& s : slots_) s.iq.resize(samples_per_slot);
        stopping_ = false; ++epoch_;
        thread_ = std::thread(&ShadowWorker::run, this);
        enabled_ = true;
        wake_.notify_one();
        return true;
    } catch (...) {
        ++failures_; enabled_ = false;
        if (thread_.joinable()) {
            { std::lock_guard<std::mutex> lock(queue_mutex_); stopping_ = true; }
            wake_.notify_all(); thread_.join();
        }
        for (auto& s : slots_) std::vector<std::complex<float>>().swap(s.iq);
        return false;
    }
}

bool ShadowWorker::try_submit(const std::vector<std::complex<float>>& iq, std::size_t prefix,
                              const CaptureContext& capture, const BurstHints& hints) noexcept {
    if (!enabled_.load()) return false;
    const auto start = std::chrono::steady_clock::now();
    try {
        if (prefix > iq.size() || prefix < 2048 || capture.source_samples != iq.size() ||
            !std::isfinite(capture.sample_rate_hz) || capture.sample_rate_hz <= 0 || capture.sample_rate_hz > 1e9 ||
            !std::isfinite(capture.capture_center_hz) || capture.capture_center_hz <= 0 ||
            (capture.band_ghz != 2 && capture.band_ghz != 5)) { ++invalid_; return false; }
        std::unique_lock<std::mutex> lock(queue_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) { ++busy_; return false; }
        if (!enabled_.load() || stopping_) return false;
        if (capture.submission_epoch != epoch_.load()) { ++discarded_; return false; }
        auto slot = std::find_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.state == State::free; });
        if (slot == slots_.end()) { ++busy_; return false; }
        slot->selection = select_sample_tiles(prefix, hints);
        slot->plan = slot->selection.plan;
        slot->iq.resize(slot->plan.samples); // preallocated capacity
        std::size_t packed = 0;
        for (std::size_t t = 0; t < slot->plan.count; ++t) {
            const auto& tile = slot->plan.tiles[t];
            std::copy_n(iq.begin() + tile.source_offset, tile.samples, slot->iq.begin() + packed);
            packed += tile.samples;
        }
        slot->capture = capture;
        if (slot->selection.burst_windows) ++burst_guided_jobs_;
        hint_descriptors_examined_ += slot->selection.hints_examined;
        slot->sequence = ++submitted_; slot->epoch = epoch_.load(); slot->state = State::queued;
        const auto occupied = std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.state != State::free; });
        update_max(high_water_, std::size_t(occupied));
        lock.unlock(); wake_.notify_one();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        update_max(max_submit_ns_, std::uint64_t(elapsed));
        return true;
    } catch (...) { ++failures_; return false; }
}

void ShadowWorker::invalidate() noexcept {
    try {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ++epoch_;
        for (auto& s : slots_) if (s.state == State::queued) { s.state = State::free; ++discarded_; }
        std::atomic_store(&latest_, std::shared_ptr<const ShadowResult>{});
    } catch (...) { ++failures_; }
}

void ShadowWorker::run() noexcept {
    try {
        // Publish the parent's enabled state before constructing anything that
        // could fail in this thread; a fast failure must not be overwritten
        // by set_enabled(true)'s final store.
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            wake_.wait(lock, [&] { return stopping_ || enabled_.load(); });
            if (stopping_) return;
        }
        ProcessClient client(options_);
        unsigned fault_count = 0, requests = 0;
        bool retry_after_fault = false;
        for (;;) {
            Slot* next = nullptr;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                wake_.wait(lock, [&] {
                    return stopping_ || std::any_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.state == State::queued; });
                });
                if (stopping_) break;
                for (auto& s : slots_) if (s.state == State::queued && (!next || s.sequence < next->sequence)) next = &s;
                next->state = State::working;
            }
            const auto start = std::chrono::steady_clock::now();
            std::shared_ptr<ShadowResult> r;
            bool exhausted = false;
            const auto cancelled = [&] { return !enabled_.load() || epoch_.load() != next->epoch; };
            try {
                if (!client.running()) {
                    if (retry_after_fault) { ++restarts_; retry_after_fault = false; }
                    client.start(cancelled); ++launches_; requests = 0; worker_pid_ = client.pid();
                }
                r = std::make_shared<ShadowResult>(); r->capture = next->capture;
                r->epoch = next->epoch; r->copied_samples = next->iq.size();
                r->selection = next->selection;
                r->tiles = client.analyze(next->sequence, next->capture.sample_rate_hz, next->plan, next->iq, cancelled);
                ++requests;
                r->processing_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                r->completed_at = std::chrono::steady_clock::now();
            } catch (const WorkerError& e) {
                r.reset();
                if (e.fault != WorkerFault::cancelled) ++failures_;
                if (e.fault == WorkerFault::analysis) ++requests;
                else {
                    client.terminate(); worker_pid_ = -1;
                    if (e.fault != WorkerFault::cancelled) {
                        if (e.fault == WorkerFault::timeout) ++timeouts_;
                        if (e.fault == WorkerFault::protocol) ++protocol_errors_;
                        retry_after_fault = true;
                        exhausted = ++fault_count > options_.restart_limit;
                    }
                }
            } catch (...) { ++failures_; r.reset(); client.terminate(); worker_pid_ = -1; exhausted = true; }
            // Recycle well before the child's cumulative 30-second CPU cap.
            // Normal recycling and selection cancellation consume no fault budget.
            if (requests >= 8) { client.terminate(); worker_pid_ = -1; requests = 0; }
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (r && !stopping_ && enabled_.load() && r->epoch == epoch_.load()) {
                    std::atomic_store(&latest_, std::shared_ptr<const ShadowResult>(std::move(r))); ++measured_;
                } else ++discarded_;
                next->state = State::free;
                if (exhausted) {
                    enabled_ = false;
                    for (auto& s : slots_) if (s.state == State::queued) { s.state = State::free; ++discarded_; }
                    std::atomic_store(&latest_, std::shared_ptr<const ShadowResult>{});
                }
            }
            if (exhausted) break;
        }
    } catch (...) {
        ++failures_; enabled_ = false;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto& s : slots_) if (s.state == State::queued) { s.state = State::free; ++discarded_; }
        std::atomic_store(&latest_, std::shared_ptr<const ShadowResult>{});
    }
    worker_pid_ = -1;
}

std::shared_ptr<const ShadowResult> ShadowWorker::snapshot() const noexcept { return std::atomic_load(&latest_); }
ShadowStats ShadowWorker::stats() const noexcept {
    return {enabled_.load(), submitted_.load(), measured_.load(), busy_.load(), invalid_.load(),
            discarded_.load(), failures_.load(), epoch_.load(), high_water_.load(), max_submit_ns_.load() / 1e6,
            launches_.load(), restarts_.load(), timeouts_.load(), protocol_errors_.load(), worker_pid_.load(),
            burst_guided_jobs_.load(), hint_descriptors_examined_.load()};
}
} // namespace rfmon::cyclo
