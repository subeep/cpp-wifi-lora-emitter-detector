// Incident storage for the passive Wi-Fi security monitor
// (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package B). Detector rules
// (package C) report observations here; package B only provides the store,
// so nothing produces incidents yet.
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
#include <optional>
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
    uint64_t evidence_dropped = 0;
    double quiet_analysed_s = 0;            // analysed seconds on its channel since last observation
};

struct IncidentLimits {
    size_t max_incidents = 1000;
    size_t max_evidence = 64;
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
    void note_analysed(const std::string& band, int channel, double analysed_s);
    const std::deque<Incident>& incidents() const { return incidents_; }  // oldest first
    size_t open_count() const;
    uint64_t evicted() const { return evicted_; }

    nlohmann::json export_json() const;
    size_t import_json(const nlohmann::json& j);  // restored incidents are flagged `restored`

private:
    IncidentLimits limits_;
    std::deque<Incident> incidents_;
    uint64_t next_id_ = 1, evicted_ = 0;
};

}  // namespace rfmon::wifi_security
