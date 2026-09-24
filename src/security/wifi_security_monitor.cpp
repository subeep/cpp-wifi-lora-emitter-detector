#include "security/wifi_security_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace rfmon::wifi_security {

namespace {
int64_t host_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

WifiSecurityMonitor::WifiSecurityMonitor(MonitorConfig config)
    : cfg_(std::move(config)), state_(cfg_.limits), published_(std::make_shared<SecuritySnapshot>()) {}

WifiSecurityMonitor::~WifiSecurityMonitor() { stop(); }

void WifiSecurityMonitor::start() {
    std::lock_guard<std::mutex> lock(qmu_);
    if (running_) return;
    // Load persisted baselines/incidents before the consumer owns the state.
    if (!cfg_.state_path.empty()) {
        std::ifstream in(cfg_.state_path);
        if (in) {
            try {
                nlohmann::json j;
                in >> j;
                state_.import_persistent(j);
            } catch (const std::exception& e) {
                persist_error_ = std::string("could not load saved state (ignored): ") + e.what();
            }
        }
    }
    running_ = true;
    stopping_ = false;
    worker_ = std::thread([this] { run(); });
}

void WifiSecurityMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(qmu_);
        if (!running_) return;
        stopping_ = true;
    }
    qcv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(qmu_);
    running_ = false;
}

bool queue_admits(const MonitorConfig& cfg, size_t depth, size_t queued_bytes, size_t item_bytes, bool is_capture) {
    const size_t item_limit = cfg.queue_max_items + (is_capture ? cfg.capture_reserve : 0);
    const size_t byte_limit = cfg.queue_max_bytes + (is_capture ? cfg.capture_reserve * 512 : 0);
    return depth < item_limit && queued_bytes + item_bytes <= byte_limit;
}

size_t WifiSecurityMonitor::approx_bytes(const Item& i) {
    if (const auto* e = std::get_if<FrameEvent>(&i)) return e->mpdu.size() + 512;
    if (const auto* c = std::get_if<CaptureRecord>(&i)) return 512 + c->overflows.size() * 24;
    return 256;
}

bool WifiSecurityMonitor::push(Item item, bool is_capture) {
    const size_t bytes = approx_bytes(item);
    {
        std::lock_guard<std::mutex> lock(qmu_);
        if (!running_ || stopping_ || !queue_admits(cfg_, queue_.size(), queue_bytes_, bytes, is_capture)) {
            const int64_t now = host_now_ns();
            (is_capture ? captures_dropped_ : events_dropped_)++;
            (is_capture ? pending_captures_dropped_ : pending_events_dropped_)++;
            if (!pending_first_ns_) pending_first_ns_ = now;
            pending_last_ns_ = now;
            return false;
        }
        queue_.push_back(std::move(item));
        queue_bytes_ += bytes;
        if (queue_.size() > high_water_) high_water_ = queue_.size();
    }
    qcv_.notify_one();
    return true;
}

bool WifiSecurityMonitor::submit(FrameEvent e) { return push(Item(std::move(e)), false); }
bool WifiSecurityMonitor::submit(CaptureRecord c) { return push(Item(std::move(c)), true); }
bool WifiSecurityMonitor::submit(ControlCommand c) { return push(Item(std::move(c)), true); }

void WifiSecurityMonitor::save_state() {
    if (cfg_.state_path.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path(cfg_.state_path);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const std::string tmp = cfg_.state_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        out << state_.export_persistent().dump() << "\n";
        out.flush();
        if (!out) { persist_error_ = "could not write saved state: " + tmp; return; }
    }
    if (std::rename(tmp.c_str(), cfg_.state_path.c_str()) != 0) {
        persist_error_ = "could not replace saved state: " + cfg_.state_path;
        return;
    }
    if (persist_error_.rfind("could not write", 0) == 0 || persist_error_.rfind("could not replace", 0) == 0)
        persist_error_.clear();
}

QueueStats WifiSecurityMonitor::queue_stats() const {
    std::lock_guard<std::mutex> lock(qmu_);
    return {queue_.size(), queue_bytes_, high_water_, events_dropped_, captures_dropped_};
}

std::shared_ptr<const SecuritySnapshot> WifiSecurityMonitor::snapshot() const {
    std::lock_guard<std::mutex> lock(pubmu_);
    return published_;
}

void WifiSecurityMonitor::record(const nlohmann::json& j) {
    if (cfg_.record_path.empty() || recording_failed_) return;
    const uint64_t half = std::max<uint64_t>(cfg_.record_max_bytes / 2, 4096);
    auto fail = [&](const std::string& why) {
        recording_failed_ = true;
        storage_error_ = why + ": " + cfg_.record_path;
        out_.close();
    };
    if (out_.is_open() && out_bytes_ >= half) {
        // Rotate: the previous file becomes ".1" (replacing any older one),
        // bounding disk use to about record_max_bytes.
        out_.close();
        const std::string old = cfg_.record_path + ".1";
        std::remove(old.c_str());
        if (std::rename(cfg_.record_path.c_str(), old.c_str()) != 0) return fail("rotation failed");
    }
    if (!out_.is_open()) {
        out_.open(cfg_.record_path, std::ios::out | std::ios::trunc);
        if (!out_) return fail("cannot open recording");
        out_bytes_ = 0;
        const std::string h = header_json(cfg_.run_id, cfg_.writer).dump() + "\n";
        out_ << h;
        out_bytes_ += h.size();
        total_recorded_ += h.size();
    }
    const std::string line = j.dump() + "\n";
    out_ << line;  // flushed once per batch in run()
    if (!out_) return fail("write failed");
    out_bytes_ += line.size();
    total_recorded_ += line.size();
}

void WifiSecurityMonitor::publish() {
    auto s = std::make_shared<SecuritySnapshot>();
    const SecuritySnapshot& cur = state_.snapshot();
    // Copy everything except the bulk of the recent-frame buffer.
    s->schema = cur.schema;
    s->captures_ingested = cur.captures_ingested; s->captures_duplicate = cur.captures_duplicate;
    s->frames_ingested = cur.frames_ingested; s->frames_accepted = cur.frames_accepted;
    s->frames_duplicate = cur.frames_duplicate; s->frames_rejected_fcs = cur.frames_rejected_fcs;
    s->frames_rejected_malformed = cur.frames_rejected_malformed;
    s->frames_repeated_content = cur.frames_repeated_content;
    s->frames_truncated_storage = cur.frames_truncated_storage;
    s->frames_without_device_time = cur.frames_without_device_time;
    s->frames_after_overflow = cur.frames_after_overflow; s->frames_security_only = cur.frames_security_only;
    s->dedupe_forgotten_captures = cur.dedupe_forgotten_captures;
    s->queue_events_dropped = cur.queue_events_dropped; s->queue_captures_dropped = cur.queue_captures_dropped;
    s->input_lines_rejected = cur.input_lines_rejected;
    s->frames_by_type = cur.frames_by_type; s->frames_by_phy = cur.frames_by_phy; s->body_states = cur.body_states;
    s->coverage = cur.coverage;
    s->losses = cur.losses;
    const size_t n = std::min(cfg_.published_recent, cur.recent.size());
    s->recent.assign(cur.recent.end() - long(n), cur.recent.end());
    s->storage_error = storage_error_;
    s->recorded_bytes = total_recorded_;
    s->timeline = cur.timeline;
    s->baselines = cur.baselines;
    s->baselines_frozen = cur.baselines_frozen;
    s->baseline_pending_dropped = cur.baseline_pending_dropped;
    s->incidents = cur.incidents;
    s->incidents_open = cur.incidents_open;
    s->incidents_total = cur.incidents_total;
    s->incidents_evicted = cur.incidents_evicted;
    s->commands_applied = cur.commands_applied;
    s->baselines_restored = cur.baselines_restored;
    s->incidents_restored = cur.incidents_restored;
    s->persist_error = persist_error_;
    std::lock_guard<std::mutex> lock(pubmu_);
    published_ = std::move(s);
}

void WifiSecurityMonitor::run() {
    auto last_publish = std::chrono::steady_clock::now();
    auto last_persist = last_publish;
    bool dirty = false;
    for (;;) {
        std::deque<Item> batch;
        LossNotice loss;
        bool finish = false;
        {
            std::unique_lock<std::mutex> lock(qmu_);
            qcv_.wait_for(lock, std::chrono::duration<double>(cfg_.publish_interval_s),
                          [&] { return !queue_.empty() || stopping_ || pending_events_dropped_ || pending_captures_dropped_; });
            batch.swap(queue_);
            queue_bytes_ = 0;
            loss.events_dropped = pending_events_dropped_;
            loss.captures_dropped = pending_captures_dropped_;
            loss.first_host_ns = pending_first_ns_;
            loss.last_host_ns = pending_last_ns_;
            pending_events_dropped_ = pending_captures_dropped_ = 0;
            pending_first_ns_ = pending_last_ns_ = 0;
            finish = stopping_;
        }
        for (auto& item : batch) {
            if (auto* e = std::get_if<FrameEvent>(&item)) {
                record(to_json(*e));
                state_.ingest(*e);
            } else if (auto* c = std::get_if<CaptureRecord>(&item)) {
                record(to_json(*c));
                state_.ingest(*c);
            } else {
                auto& cmd = std::get<ControlCommand>(item);
                record(to_json(cmd));
                state_.ingest(cmd);
            }
            dirty = true;
        }
        if (loss.events_dropped || loss.captures_dropped) {
            record(to_json(loss));
            state_.ingest(loss);
            dirty = true;
        }
        if (out_.is_open() && !recording_failed_) {
            out_.flush();
            if (!out_) {
                recording_failed_ = true;
                storage_error_ = "write failed: " + cfg_.record_path;
                out_.close();
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (finish || (dirty && now - last_publish >= std::chrono::duration<double>(cfg_.publish_interval_s))) {
            publish();
            last_publish = now;
            dirty = false;
        }
        if (!finish && now - last_persist >= std::chrono::duration<double>(cfg_.persist_interval_s)) {
            save_state();
            last_persist = now;
        }
        if (finish) {
            // Anything submitted after stopping_ was refused; the queue is empty.
            out_.close();
            save_state();
            publish();
            return;
        }
    }
}

}  // namespace rfmon::wifi_security
