// Live side of the passive Wi-Fi security monitor (package A step 4): a
// bounded, non-blocking input queue, one consumer thread that owns the
// SecurityState, an optional size-capped NDJSON recorder, and immutable
// published snapshots.
//
// The acquisition thread only ever calls submit(). submit() takes one short
// lock, never waits on the consumer or the disk, and drops (counting the
// loss) when the queue is full - so a slow consumer or a failing disk can
// degrade security coverage but can never stall reception. Every drop
// resurfaces downstream as a LossNotice, both in the snapshot and in the
// recording, rather than as silently thinner data.
//
// Capture records have a small reserved headroom above the item limit: the
// coverage denominator is worth more than one more frame.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include "security/wifi_security_state.hpp"

namespace rfmon::wifi_security {

struct MonitorConfig {
    size_t queue_max_items = 20000;
    size_t queue_max_bytes = size_t(16) << 20;  // approximate, MPDU bytes + fixed overhead
    size_t capture_reserve = 1024;              // extra items allowed for CaptureRecords only
    StateLimits limits;
    size_t published_recent = 256;              // frames copied into each published snapshot
    double publish_interval_s = 0.25;
    std::string record_path;                    // empty: no recording
    uint64_t record_max_bytes = uint64_t(64) << 20;  // current file + one rotated ".1" file
    std::string writer = "rf_monitor";
    std::string run_id;                         // written into each recording header
    // Baselines and incidents persisted across restarts (empty: in memory
    // only). Loaded before the consumer starts; saved atomically every
    // persist_interval_s and on stop.
    std::string state_path;
    double persist_interval_s = 60.0;
};

struct QueueStats {
    size_t depth = 0, bytes = 0, high_water = 0;
    uint64_t events_dropped = 0, captures_dropped = 0;
};

// The queue admission rule, exposed so its limits are testable without
// racing the consumer thread. Capture records get the reserved headroom.
bool queue_admits(const MonitorConfig& cfg, size_t depth, size_t queued_bytes, size_t item_bytes, bool is_capture);

class WifiSecurityMonitor {
public:
    explicit WifiSecurityMonitor(MonitorConfig config = {});
    ~WifiSecurityMonitor();
    WifiSecurityMonitor(const WifiSecurityMonitor&) = delete;
    WifiSecurityMonitor& operator=(const WifiSecurityMonitor&) = delete;

    void start();
    // Drains everything already queued, publishes a final snapshot, joins.
    void stop();

    bool submit(FrameEvent e);      // false: dropped (queue full or stopped)
    bool submit(CaptureRecord c);
    bool submit(ControlCommand c);  // operator action; uses the capture-record headroom

    std::shared_ptr<const SecuritySnapshot> snapshot() const;
    QueueStats queue_stats() const;

private:
    using Item = std::variant<FrameEvent, CaptureRecord, ControlCommand>;
    static size_t approx_bytes(const Item& i);
    bool push(Item item, bool is_capture);
    void run();
    void record(const nlohmann::json& j);
    void publish();
    void save_state();

    MonitorConfig cfg_;
    mutable std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<Item> queue_;
    size_t queue_bytes_ = 0, high_water_ = 0;
    uint64_t events_dropped_ = 0, captures_dropped_ = 0;
    uint64_t pending_events_dropped_ = 0, pending_captures_dropped_ = 0;
    int64_t pending_first_ns_ = 0, pending_last_ns_ = 0;
    bool running_ = false, stopping_ = false;
    std::thread worker_;

    // Consumer-thread only.
    SecurityState state_;
    std::ofstream out_;
    uint64_t out_bytes_ = 0, total_recorded_ = 0;
    bool recording_failed_ = false;
    std::string storage_error_, persist_error_;

    mutable std::mutex pubmu_;
    std::shared_ptr<const SecuritySnapshot> published_;
};

}  // namespace rfmon::wifi_security
