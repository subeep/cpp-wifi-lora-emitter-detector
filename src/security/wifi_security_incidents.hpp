// Incident storage for the passive Wi-Fi security monitor
// (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package B). Detector rules
// (package C) report observations here; the store does not decide whether a rule fires.
//
// Record contents follow the plan: rule/config version, severity and a
// qualitative confidence (independent of each other, never a probability),
// first/last observation, claimed source/target, channel, the measured
// numerator and its analysed-time denominator, coverage limitations, benign
// alternatives and bounded evidence references.
//
// Repeated observations of the same thing coalesce into one open incident.
// An incident closes only after a quiet period measured in ANALYSED air time
// on its channel - a scan gap or a restart is not evidence that activity
// stopped. Incidents restored after a restart are flagged as interrupted.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "security/wifi_frame_event.hpp"

namespace rfmon::wifi_security {

enum class IncidentSeverity { Info, Low, Medium, High };
enum class IncidentConfidence { Low, Medium, High };
const char* severity_name(IncidentSeverity s);
const char* confidence_name(IncidentConfidence c);

struct IncidentEvidence {
    IngestKey frame;  // (run_id, capture_seq, sample_start, phy)
    int64_t host_ns = 0;
    std::string note;
    std::string role;              // e.g. "first_unit", "segment_latest"
    std::string summary;           // describe() of the frame at the time
    std::vector<uint8_t> mpdu;     // first bytes of the MPDU (bounded), for inspection
};

// One point of an incident's timeline, added by the rule at chosen moments
// (first emission, segment close), so the GUI can show how the measured rate
// evolved against the threshold.
struct IncidentPoint {
    int64_t host_ns = 0;
    std::optional<int64_t> device_ns;
    std::string run_id;
    uint64_t capture_seq_first = 0, capture_seq_last = 0;
    double a_start = 0, analysed_s = 0;  // position/length in the channel's analysed timeline
    double units = 0, raw = 0, rate = 0, threshold = 0;
    std::string governing_term, confidence, path, tags;
};

// What a rule reports each time its condition is observed.
struct IncidentObservation {
    std::string rule, rule_version, config_version;
    IncidentSeverity severity = IncidentSeverity::Info;
    IncidentConfidence confidence = IncidentConfidence::Low;
    std::string band;
    int channel = 0;
    std::string claimed_source, target;  // claimed addresses, never authenticated identities
    int64_t host_ns = 0;
    std::optional<int64_t> device_ns;
    double numerator = 0, denominator_analysed_s = 0;
    std::string coverage_note;
    std::vector<std::string> benign_alternatives;
    std::optional<IncidentEvidence> evidence;
    std::vector<IncidentEvidence> extra_evidence;
    // Optional structured fields (detector rules). claimed_bssid, when set,
    // replaces claimed_source in the coalescing key, so a changing claimed
    // transmitter does not split one pattern into many incidents.
    std::string claimed_bssid, mode, tier, path, baseline_key;
    uint32_t baseline_version = 0;
    std::vector<std::string> confidence_basis;
    std::map<std::string, double> measures;
    std::map<std::string, std::string> context;
};

struct Incident {
    uint64_t id = 0;
    std::string rule, rule_version, config_version;
    IncidentSeverity severity = IncidentSeverity::Info;
    IncidentConfidence confidence = IncidentConfidence::Low;
    bool open = true, restored = false;
    std::string band;
    int channel = 0;
    std::string claimed_source, target;
    int64_t first_host_ns = 0, last_host_ns = 0;
    std::optional<int64_t> first_device_ns, last_device_ns;
    uint64_t observations = 0;
    double numerator = 0, denominator_analysed_s = 0;  // latest reported values
    std::string coverage_note;
    std::vector<std::string> benign_alternatives;
    std::deque<IncidentEvidence> evidence;  // newest kept, bounded
    std::vector<IncidentEvidence> first_evidence;  // the earliest evidence, kept once
    uint64_t evidence_dropped = 0;
    double quiet_analysed_s = 0;            // analysed seconds on its channel since last observation
    std::string claimed_bssid, mode, tier, path, baseline_key;
    uint32_t baseline_version = 0;
    std::vector<std::string> confidence_basis;
    std::map<std::string, double> measures;
    std::map<std::string, std::string> context;
    IncidentConfidence peak_confidence = IncidentConfidence::Low;
    double peak_rate = 0;
    std::deque<IncidentPoint> timeline;     // bounded
};

struct IncidentLimits {
    size_t max_incidents = 1000;
    size_t max_evidence_bytes = 512; // per item, live and restored
    size_t max_text_bytes = 512;
    size_t max_context_items = 32;
    size_t max_evidence = 64;
    size_t max_first_evidence = 8;
    size_t max_timeline = 240;
    double close_after_quiet_analysed_s = 60.0;
};

class IncidentStore {
public:
    explicit IncidentStore(IncidentLimits limits = {}) : limits_(limits) {}
    // Coalesces into the open incident with the same rule, channel, source
    // and target, or opens a new one. Returns its id.
    uint64_t observe(const IncidentObservation& o);
    // Analysed air on (band, channel) with no new observation: advances the
    // quiet clock of that channel's open incidents and closes expired ones.
    // Incidents in `skip` (observed during this same capture) are exempt, so an
    // incident cannot close and reopen within one capture.
    void note_analysed(const std::string& band, int channel, double analysed_s,
                       const std::set<uint64_t>* skip = nullptr, const std::string& baseline_key = "");
    // Adds a timeline point; ignores ids that were evicted meanwhile.
    void append_point(uint64_t id, const IncidentPoint& p);
    void reset_quiet(uint64_t id);
    uint64_t revision() const { return revision_; }  // bumps on every change
    const std::deque<Incident>& incidents() const { return incidents_; }  // oldest first
    size_t open_count() const;
    uint64_t evicted() const { return evicted_; }

    nlohmann::json export_json() const;
    size_t import_json(const nlohmann::json& j);  // restored incidents are flagged `restored`

private:
    void bound(Incident& i);
    IncidentLimits limits_;
    std::deque<Incident> incidents_;
    uint64_t next_id_ = 1, evicted_ = 0, revision_ = 0;
};

nlohmann::json incident_json(const Incident& i);

}  // namespace rfmon::wifi_security
