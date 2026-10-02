#pragma once
#include <deque>
#include "security/wifi_security_baseline.hpp"
#include "security/wifi_security_incidents.hpp"

namespace rfmon::wifi_security {
// Provisional observation-mode thresholds, not calibrated attack probabilities.
struct FloodConfig {
    bool enabled = true;
    double window_s = 2, min_baseline_s = 30, floor_rate = 10, baseline_multiplier = 4;
    size_t min_baseline_windows = 3, min_units = 20, min_active_captures = 2;
    double release_quiet_s = 5, max_gap_s = 1;
    // Repeated-content units require device timing, Retry clear, and separation
    // from the previous accepted copy. Short retry trains never inflate them.
    double repeat_guard_s = .01;
    size_t min_ap_targets = 2, max_window_captures = 128;
    size_t max_pending = 64, max_frames_per_capture = 1024, max_profiles = 16;
    size_t max_groups = 128, max_units = 4096;
};
struct FloodResult {
    bool usable = false;
    std::set<uint64_t> observed_incidents;
};
class ManagementFloodRule {
public:
    explicit ManagementFloodRule(FloodConfig config = {});
    void note_frame(const FrameEvent& e, const MacFrame& f);
    FloodResult finish_capture(const CaptureRecord& c, BaselineStore& baselines, IncidentStore& incidents);
    void interrupt(); // loss/restart: discard partial rate windows, never infer quiet
    uint64_t excluded() const { return excluded_; }
    uint64_t evaluations() const { return evaluations_; }
    const std::string& config_version() const { return config_version_; }
private:
    struct Unit {
        std::string bssid, target, source;
        uint64_t hash = 0;
        bool retry = false;
        std::optional<int64_t> device_ns;
        IncidentEvidence evidence;
    };
    struct Pending { uint64_t received = 0; bool degraded = false; std::vector<Unit> units; };
    struct Group {
        std::set<uint64_t> unique;
        uint64_t raw = 0, active_captures = 0;
        uint64_t independent = 0, latest_units = 0;
        std::set<std::string> targets;
        std::map<uint64_t, std::optional<int64_t>> last_copy;
        Unit first, last;
    };
    struct Segment { uint64_t capture = 0; double seconds = 0; std::vector<Unit> units; };
    struct Window {
        std::string run;
        uint64_t session = 0, first_capture = 0;
        uint32_t baseline_version = 0;
        int64_t end_ns = 0;
        bool device_clock = false;
        double seconds = 0, quiet = 0;
        size_t units = 0;
        std::deque<Segment> segments;
        std::map<std::pair<std::string,std::string>, Group> groups;
    };
    FloodConfig cfg_;
    std::string config_version_;
    std::map<std::pair<std::string,uint64_t>, Pending> pending_;
    std::map<std::string, Window> windows_;
    uint64_t excluded_ = 0, evaluations_ = 0;
    bool lost_ = false;
};
}
