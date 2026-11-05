// RF identity rules for the passive Wi-Fi security monitor (RF fingerprint
// plan, phase B: docs/WIFI_RF_IDENTITY_2026-11-04.md).
//
// Each verified frame carries its whole-frame early CFO (wifi_rf_features.hpp,
// phase A). The reference for a claimed sender is that sender's OWN frames
// on the same channel within the last few seconds, so receiver and device
// drift cancel and no enrolment is needed. Two rules:
//
//  - rf_forged_disconnect: deauthentication / disassociation frames whose
//    CFO is inconsistent with the claimed sender's other frames of the same
//    length class. Several inconsistent frames, at least half of those
//    evaluated, are required before reporting.
//  - rf_identity_two_transmitters: one AP address whose recent frames form
//    two CFO clusters that interleave in time - two radios using one BSSID
//    (evil twin, AP clone).
//
// Only management and data frames are used; control frames are not reliable
// enough (see note_frame).
//
// Roles matter (live, 5 November 2026): clients pre-correct their carrier to
// their AP and re-adjust it in steps of up to about 1 ppm while associated,
// so a client's own frames are not one stable cluster. APs do not. A
// disconnect claimed from a client (transmitter != BSSID) is therefore judged
// with a wider tolerance, and the two-transmitter rule only runs for
// addresses seen as an AP and never as a client.
//
// Calibrated on 12,745 live frames (phase A): genuine per-frame scores reach
// 5.6 spreads and 0.18 ppm at the 99.9th percentile; the thresholds below sit
// above both. Known limit: a client that pre-corrects its carrier to its AP
// shares the AP's CFO, so a CFO match is "this sender or one of its
// pre-correcting clients", never proof of the sender.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "security/wifi_frame_event.hpp"
#include "security/wifi_mac_frame.hpp"
#include "security/wifi_security_incidents.hpp"

namespace rfmon::wifi_security {

struct RfIdentityConfig {
    bool enabled = true;
    double window_s = 10;              // contemporaneous reference, either side
    double fallback_window_s = 60;     // when the window has too few frames (sweeps revisit a channel slowly)
    size_t min_reference = 5;          // reference frames needed (same length class)
    double score_threshold = 8;        // |delta| / reference spread
    double min_delta_ppm = 0.25;       // and at least this far, in ppm
    double spread_floor_ppm = 0.02;    // reference spread never assumed smaller
    double mixed_class_allowance_ppm = 0.15;  // extra tolerance when only other-length frames exist
    size_t min_inconsistent = 3;       // inconsistent disconnect frames before reporting
    double min_inconsistent_share = 0.5;
    size_t min_cluster = 5;            // two transmitters: frames in each cluster
    double min_cluster_gap_ppm = 0.3;
    double cluster_gap_spreads = 8;
    int min_alternations = 2;          // cluster changes in time order (interleaving)
    double client_min_delta_ppm = 2.0; // disconnects claimed from a client: pre-correction steps
    size_t max_identities = 2048, history_per_identity = 256, max_pending_frames = 4096;
};

// One verified frame with RF features, as the tracker sees it.
struct RfObservation {
    std::string band;
    int channel = 0;
    std::string transmitter, receiver, bssid;
    bool disconnect = false;           // deauthentication / disassociation
    bool client_frame = false;         // data frame to the DS (station to AP)
    std::string length_class;          // "dsss", "ofdm-short" (< 20 symbols), "ofdm-long"
    double cfo_ppm = 0;                // early CFO
    int64_t t_ns = 0;
    IncidentEvidence evidence;
};

struct RfFinding {
    enum class Kind { ForgedDisconnect, TwoTransmitters } kind = Kind::ForgedDisconnect;
    std::string band, transmitter, target, bssid, length_class;
    bool claimed_client = false;       // forged disconnect: claimed sender is a client
    int channel = 0;
    int64_t t_ns = 0;
    // Forged disconnect: the suspect frames against the sender's reference.
    size_t inconsistent = 0, evaluated = 0, reference_frames = 0;
    double reference_ppm = 0, suspect_ppm = 0, reference_spread_ppm = 0;
    // Two transmitters: the two clusters.
    double a_ppm = 0, b_ppm = 0, a_spread_ppm = 0, b_spread_ppm = 0;
    size_t a_frames = 0, b_frames = 0;
    int alternations = 0;
    std::vector<IncidentEvidence> evidence;
};

// Pure: observations in, findings out. Shared by the monitor rule and the
// offline evaluation (tools/wifi_rf_auth_eval.cpp).
class RfIdentityTracker {
public:
    explicit RfIdentityTracker(RfIdentityConfig cfg = {}) : cfg_(cfg) {}
    void add(const RfObservation& o);
    // Judges disconnect frames added since the last call and re-checks the
    // touched identities for two clusters.
    std::vector<RfFinding> evaluate();
    void clear();
    uint64_t judged() const { return judged_; }
    uint64_t unverifiable() const { return unverifiable_; }
    uint64_t inconsistent() const { return inconsistent_; }
    size_t identities() const { return ids_.size(); }
private:
    struct Entry { RfObservation o; bool judged = false, evaluable = false, inconsistent = false; };
    using Key = std::tuple<std::string, int, std::string>;
    struct Role { bool ap = false, client = false; };
    RfIdentityConfig cfg_;
    std::map<Key, std::deque<Entry>> ids_;
    std::map<Key, Role> roles_;        // kept as long as the identity
    static bool claimed_client(const RfObservation& o, const Role& r);
    std::set<Key> touched_;
    uint64_t judged_ = 0, unverifiable_ = 0, inconsistent_ = 0;
};

struct RfIdentityResult { bool usable = false; std::set<uint64_t> observed_incidents; };

// Security-state adapter: frames wait for their capture record, as in the
// other rules; degraded captures are excluded, never judged.
class RfIdentityRule {
public:
    explicit RfIdentityRule(RfIdentityConfig cfg = {});
    void note_frame(const FrameEvent& e, const MacFrame& f);
    RfIdentityResult finish_capture(const CaptureRecord& c, IncidentStore& incidents);
    void interrupt();  // loss / restart: forget references, never compare across a gap
    const RfIdentityTracker& tracker() const { return tracker_; }
    uint64_t excluded() const { return excluded_; }
    uint64_t frames_with_features() const { return with_features_; }
    const std::string& config_version() const { return config_version_; }
private:
    RfIdentityConfig cfg_;
    std::string config_version_;
    RfIdentityTracker tracker_;
    std::map<std::pair<std::string, uint64_t>, std::vector<RfObservation>> pending_;
    size_t pending_frames_ = 0;
    uint64_t excluded_ = 0, with_features_ = 0;
};

}  // namespace rfmon::wifi_security
