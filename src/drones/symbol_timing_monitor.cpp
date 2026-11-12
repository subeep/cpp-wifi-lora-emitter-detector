#include "symbol_timing_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace rfmon::symbol_timing {
using Clock = std::chrono::steady_clock;

struct Monitor::State {
    static constexpr std::size_t pending_limit = 4;
    struct Job {
        std::vector<std::complex<float>> samples =
            std::vector<std::complex<float>>(max_blocks * block_size);
        Result result;
        std::atomic<bool> copied{false};
        Job() { result.run_id.reserve(256); }
    };
    std::array<Job, pending_limit + 1> jobs;
    std::deque<Result> history;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::thread worker;
    std::size_t head{}, count{};
    bool stopping{}, active{};
    std::atomic<bool> running{false};
    std::atomic<uint64_t> full{}, contention{}, invalid{}, unavailable{}, completed{}, max_ns{};

    void run() {
        try {
            Probe probe;
            for (;;) {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&]{ return count || stopping; });
                if (stopping && !count) return;
                auto& copied = jobs[head].copied;
                if (!copied.load()) {
                    // The head is reserved and its producer is copying outside the lock.
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                active = true;
                auto& job = jobs[head];
                lock.unlock();
                const auto start = Clock::now();
                auto& result = job.result;
                bool valid = true;
                for (std::size_t block = 0; block < result.blocks; ++block) {
                    Ratios ratios;
                    if (!probe.measure(job.samples.data() + block * block_size, result.rate, ratios)) {
                        valid = false;
                        break;
                    }
                    for (std::size_t i = 0; i < ratios.size(); ++i)
                        result.fractions[i] += ratios[i] > flag_ratio;
                }
                for (auto& fraction : result.fractions) fraction /= result.blocks;
                result.timing_group = group(result.fractions);
                result.processing_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

                lock.lock();
                if (valid) {
                    history.push_front(result);
                    if (history.size() > 200) history.pop_back();
                    ++completed;
                } else {
                    ++invalid;
                }
                job.copied = false;
                head = (head + 1) % jobs.size();
                --count;
                active = false;
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            unavailable += count;
            count = 0;
            active = false;
            running = false;
        }
    }
};

Monitor::Monitor() : state_(std::make_unique<State>()) {}
Monitor::~Monitor() { stop(); }

void Monitor::start() {
    auto& s = *state_;
    if (s.worker.joinable()) return;
    s.stopping = false;
    s.running = true;
    try {
        s.worker = std::thread([&s]{ s.run(); });
    } catch (...) {
        s.running = false;
        throw;
    }
}

void Monitor::stop() {
    auto& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        s.running = false;
        s.stopping = true;
    }
    s.ready.notify_one();
    if (s.worker.joinable()) s.worker.join();
}

void Monitor::observe(const std::vector<std::complex<float>>& iq,
                      const wifi_security::CaptureRecord& c) noexcept {
    auto& s = *state_;
    const auto start = Clock::now();
    const double center = c.stream_center_hz();
    if (!((center >= 2400e6 && center <= 2500e6) ||
          (center >= 4900e6 && center <= 5925e6))) return;
    if (!supported_rate(c.sample_rate_hz) || iq.size() < block_size ||
        c.samples_received != iq.size() || !c.contiguous() || c.run_id.size() > 256) {
        ++s.invalid;
        return;
    }
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock) { ++s.contention; return; }
    if (!s.running) { ++s.unavailable; return; }
    // The occupied head slot includes work in progress. Reserve one slot for it.
    if (s.count - s.active >= State::pending_limit) { ++s.full; return; }
    auto& job = s.jobs[(s.head + s.count) % s.jobs.size()];
    ++s.count;
    lock.unlock();
    auto& r = job.result;
    r.run_id = c.run_id;
    r.capture_seq = c.capture_seq;
    r.radio_session = c.radio_session;
    r.center = center;
    r.rate = c.sample_rate_hz;
    r.clock = c.clock;
    r.device_ns = c.device_time_ns;
    r.host_before_ns = c.host_before_ns;
    r.host_after_ns = c.host_after_ns;
    r.observed = start;
    r.blocks = std::min(max_blocks, iq.size() / block_size);
    r.fractions.fill(0);
    for (std::size_t i = 0; i < r.blocks; ++i)
        std::copy_n(iq.data() + block_start(iq.size(), i, r.blocks), block_size,
                    job.samples.data() + i * block_size);
    job.copied = true;
    s.ready.notify_one();
    const auto ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    auto previous = s.max_ns.load();
    while (ns > previous && !s.max_ns.compare_exchange_weak(previous, ns)) {}
}

Snapshot Monitor::snapshot() const {
    auto& s = *state_;
    std::lock_guard lock(s.mutex);
    return {s.full, s.contention, s.invalid, s.unavailable, s.completed,
            s.max_ns / 1e6, {s.history.begin(), s.history.end()}};
}
}
