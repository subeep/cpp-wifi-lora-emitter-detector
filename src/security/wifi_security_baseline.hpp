// Clean-traffic baselines for the passive Wi-Fi security monitor
// (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package B).
//
// A baseline is a history of fixed-length windows of ANALYSED air time (not
// wall-clock) on one channel under one receiver configuration, each holding
// observed frame counts. Detector thresholds (package C) will compare live
// per-analysed-second rates against these windows' distribution.
//
// Rules the plan requires, and how they are met:
//  - Conditioned on channel AND receiver settings: the key includes band,
//    channel, sample rate, gain, antenna and device. Changing any of them
//    starts a different baseline rather than silently mixing conditions.
//  - Only clean input: a capture contributes only if it was contiguous (no
//    overflow/timeout) and processed. Degraded captures are counted, not
//    learned from.
//  - Versioned and reviewable: reset bumps the version and clears history;
//    each window records the version it was learned under.
//  - Freeze during candidate incidents: while frozen, windows still close
//    but are marked excluded, so persistent suspicious activity cannot become
//    "normal". There are two independent sources: the operator's freeze
//    (set_frozen) and detector-rule holds (set_hold, one set of holders per
//    key). Neither can release the other.
//  - Restart does not pretend continuity: restored baselines keep their
//    closed windows, but any partial window is discarded and the restore is
//    flagged.
// Counts are observed decoded frames, never transmitted totals.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "security/wifi_frame_event.hpp"
#include "security/wifi_mac_frame.hpp"

namespace rfmon::wifi_security {

// Per-window counters. Kept as a fixed array so windows are cheap to copy.
enum class BaselineMetric : size_t {
    Frames, Bursts, Deauth, DeauthBroadcast, Disassoc, Auth, AssocRequest, ReassocRequest, ProbeRequest,
    Beacon, Action, Data, Control, Transmitters, kCount
};
constexpr size_t kBaselineMetrics = size_t(BaselineMetric::kCount);
const char* baseline_metric_name(BaselineMetric m);

struct BaselineLimits {
    double window_analysed_s = 10.0;  // analysed seconds per window
    size_t max_windows = 720;         // per baseline: 2 h of analysed air at 10 s
    size_t max_baselines = 64;
    size_t max_pending_captures = 4096;
    size_t max_window_transmitters = 4096;
};

struct BaselineWindow {
    uint32_t version = 1;
    bool excluded = false;             // closed while frozen or held: not used for statistics
    bool held = false;                 // a rule hold was active at any capture in this window
    std::string exclusion_reason;      // "", "user_freeze", "rule_hold" or "user_freeze+rule_hold"
    int64_t first_host_ns = 0, last_host_ns = 0;
    double analysed_s = 0;
    uint64_t captures = 0;
    std::array<uint64_t, kBaselineMetrics> counts{};
};

// Distribution of a metric's per-analysed-second rate across included windows.
struct RateStats {
    double mean = 0, p50 = 0, p95 = 0, max = 0;
};

struct BaselineSummary {
    std::string key, band, antenna, device;
    int channel = 0;
    double sample_rate_hz = 0;
    std::optional<double> gain_db;
    uint32_t version = 1;
    bool frozen = false, restored = false;
    uint64_t windows_closed = 0, windows_excluded = 0, captures_excluded = 0, windows_held = 0;
    size_t windows_retained = 0, windows_included = 0;
    std::vector<std::string> holders;  // active rule holds on this key
    double learned_analysed_s = 0, current_analysed_s = 0;
    std::array<RateStats, kBaselineMetrics> rates{};
};

class BaselineStore {
public:
    explicit BaselineStore(BaselineLimits limits = {}) : limits_(limits) {}
    // Accepted frame from capture (run, seq); counted when its capture record arrives.
    void note_frame(const FrameEvent& e, const MacFrame& f);
    void note_capture(const CaptureRecord& c, bool usable = true);
    void invalidate_pending();
    void set_frozen(const std::string& key, bool frozen);  // key "" = all
    void reset(const std::string& key);                    // key "" = all; holds are kept
    // Rule-driven learning hold on one key. Works for keys whose baseline does
    // not exist yet, survives eviction and reset(), and is independent of the
    // operator freeze: set_frozen never touches holds, set_hold never touches
    // the freeze.
    bool set_hold(const std::string& key, const std::string& holder, bool on);
    const std::set<std::string>* holders(const std::string& key) const;
    const BaselineSummary* summary(const std::string& key) const;
    // Distribution over included windows (same filter as the per-metric
    // rates) of the SUMMED per-window rate of several metrics, e.g. deauth +
    // disassoc. Per-metric percentiles cannot be added; this sums first.
    RateStats combined_rate(const std::string& key, std::initializer_list<BaselineMetric> metrics) const;
    std::vector<BaselineSummary> summaries() const;
    const std::deque<BaselineWindow>* windows(const std::string& key) const;
    uint64_t pending_dropped() const { return pending_dropped_; }
    static std::string key_for(const CaptureRecord& c);

    nlohmann::json export_json() const;              // closed windows + metadata, no partial windows
    size_t import_json(const nlohmann::json& j);     // returns baselines restored; marks them restored

private:
    struct Pending {
        std::array<uint64_t, kBaselineMetrics> counts{};
        std::set<MacAddress> transmitters;
    };
    struct Baseline {
        BaselineSummary meta;
        std::deque<BaselineWindow> windows;
        BaselineWindow current;
        std::set<MacAddress> current_transmitters;
    };
    Baseline& baseline_for(const CaptureRecord& c);
    void close_window(Baseline& b);
    static void recompute(Baseline& b);

    BaselineLimits limits_;
    std::map<std::string, Baseline> baselines_;
    std::map<std::pair<std::string, uint64_t>, Pending> pending_;
    std::deque<std::pair<std::string, uint64_t>> pending_order_;
    uint64_t pending_dropped_ = 0;
    bool freeze_new_ = false;  // a global freeze also applies to baselines created later
    std::map<std::string, std::set<std::string>> holds_;  // key -> holders (outside Baseline on purpose)
};

}  // namespace rfmon::wifi_security
