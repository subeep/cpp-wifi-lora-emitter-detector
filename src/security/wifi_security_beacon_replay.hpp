#pragma once
#include "security/wifi_security_baseline.hpp"
#include "security/wifi_security_incidents.hpp"
#include "security/wifi_mac_frame.hpp"

namespace rfmon::wifi_security {
// Conservative historical-beacon rule. No clock-drift, encrypted-data or
// disconnect replay verdicts. All time limits are provisional until RF validation.
struct BeaconReplayConfig {
    bool enabled = true;
    double min_delay_s = 0.5, retention_s = 60, max_gap_s = 2;
    size_t max_pending = 64, max_frames_per_capture = 512;
    size_t max_profiles = 8, max_identities = 64, history_per_identity = 16;
    size_t max_frame_bytes = 2048;
};
struct BeaconReplayResult { bool usable = false; std::set<uint64_t> observed_incidents; };
class BeaconReplayRule {
public:
    explicit BeaconReplayRule(BeaconReplayConfig config = {});
    void note_frame(const FrameEvent&, const MacFrame&);
    BeaconReplayResult finish_capture(const CaptureRecord&, IncidentStore&);
    void interrupt();
    uint64_t evaluated() const { return evaluated_; }
    uint64_t excluded() const { return excluded_; }
    uint64_t forgotten() const { return forgotten_; }
    const std::string& config_version() const { return config_version_; }
private:
    struct Beacon {
        std::string identity, bssid, transmitter, receiver;
        uint64_t tsf = 0;
        int64_t time = 0;
        std::vector<uint8_t> bytes; // Full comparison without FCS, Retry masked.
        IncidentEvidence evidence;
    };
    struct Pending { size_t received = 0; bool degraded = false; std::vector<Beacon> frames; };
    struct History { std::deque<Beacon> frames; Beacon newest; bool initialized = false; };
    struct Profile {
        std::string run; uint64_t session = 0; int64_t end = 0;
        bool initialized = false;
        std::map<std::string, History> identities;
    };
    BeaconReplayConfig cfg_;
    std::string config_version_;
    std::map<std::pair<std::string,uint64_t>, Pending> pending_;
    std::map<std::string, Profile> profiles_;
    bool lost_ = false;
    uint64_t evaluated_ = 0, excluded_ = 0, forgotten_ = 0;
};
}
