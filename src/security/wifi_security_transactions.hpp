#pragma once
#include <array>
#include <deque>
#include <map>
#include "security/wifi_security_baseline.hpp"
#include "security/wifi_security_incidents.hpp"

namespace rfmon::wifi_security {
// Observation policy: historical requests after a newer answered exchange.
// Neither an observed response nor FCS verifies authentication or acceptance.
struct TransactionConfig {
    bool enabled = true;
    double min_delay_s = .5, retention_s = 30, max_gap_s = 1;
    size_t max_pending = 32, max_frames = 256, max_profiles = 8, max_links = 128, history = 8;
};
struct TransactionResult { bool usable = true; std::set<uint64_t> observed_incidents; };
class TransactionReplayRule {
public:
    explicit TransactionReplayRule(TransactionConfig config = {});
    void note_frame(const FrameEvent&, const MacFrame&);
    TransactionResult finish_capture(const CaptureRecord&, IncidentStore&);
    void interrupt();
    nlohmann::json diagnostics() const;
private:
    struct Frame {
        std::string ap, client, link, kind;
        bool request = false, retry = false, reset = false, eapol = false;
        uint64_t counter = 0;
        int stage = 0;
        std::array<uint8_t,32> nonce{};
        std::vector<uint8_t> bytes;
        FrameEvent event;
        IncidentEvidence evidence;
    };
    struct Exchange { Frame request; std::optional<Frame> response; };
    struct Link { std::deque<Exchange> history; std::array<uint8_t,32> nonce{}; bool nonce_known=false; };
    struct Profile {
        std::string run; uint64_t session=0; int64_t end=0; bool initialized=false;
        std::map<std::string,Link> links;
    };
    struct Pending { size_t received=0; bool degraded=false; std::vector<Frame> frames; };
    TransactionConfig cfg_;
    std::map<std::pair<std::string,uint64_t>,Pending> pending_;
    std::map<std::string,Profile> profiles_;
    bool lost_=false;
    uint64_t evaluated_=0, excluded_=0, forgotten_=0, unsupported_=0;
};
}
