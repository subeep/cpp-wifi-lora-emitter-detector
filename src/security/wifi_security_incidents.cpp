#include "security/wifi_security_incidents.hpp"

#include <algorithm>

namespace rfmon::wifi_security {

using nlohmann::json;

const char* severity_name(IncidentSeverity s) {
    switch (s) {
        case IncidentSeverity::Info: return "info";
        case IncidentSeverity::Low: return "low";
        case IncidentSeverity::Medium: return "medium";
        case IncidentSeverity::High: return "high";
    }
    return "info";
}

const char* confidence_name(IncidentConfidence c) {
    switch (c) {
        case IncidentConfidence::Low: return "low";
        case IncidentConfidence::Medium: return "medium";
        case IncidentConfidence::High: return "high";
    }
    return "low";
}

namespace {
IncidentSeverity severity_from(const std::string& s) {
    if (s == "high") return IncidentSeverity::High;
    if (s == "medium") return IncidentSeverity::Medium;
    if (s == "low") return IncidentSeverity::Low;
    return IncidentSeverity::Info;
}
IncidentConfidence confidence_from(const std::string& s) {
    if (s == "high") return IncidentConfidence::High;
    if (s == "medium") return IncidentConfidence::Medium;
    return IncidentConfidence::Low;
}
}  // namespace

uint64_t IncidentStore::observe(const IncidentObservation& o) {
    auto it = std::find_if(incidents_.begin(), incidents_.end(), [&](const Incident& i) {
        return i.open && i.rule == o.rule && i.band == o.band && i.channel == o.channel &&
               i.claimed_source == o.claimed_source && i.target == o.target;
    });
    if (it == incidents_.end()) {
        if (incidents_.size() >= limits_.max_incidents) {
            // Evict the oldest closed incident; if all are open, the oldest.
            auto victim = std::find_if(incidents_.begin(), incidents_.end(), [](const Incident& i) { return !i.open; });
            if (victim == incidents_.end()) victim = incidents_.begin();
            incidents_.erase(victim);
            ++evicted_;
        }
        Incident n;
        n.id = next_id_++;
        n.rule = o.rule;
        n.band = o.band;
        n.channel = o.channel;
        n.claimed_source = o.claimed_source;
        n.target = o.target;
        n.first_host_ns = o.host_ns;
        n.first_device_ns = o.device_ns;
        incidents_.push_back(std::move(n));
        it = incidents_.end() - 1;
    }
    Incident& i = *it;
    i.rule_version = o.rule_version;
    i.config_version = o.config_version;
    i.severity = std::max(i.severity, o.severity);
    i.confidence = o.confidence;  // latest assessment, as reported by the rule
    i.last_host_ns = o.host_ns;
    if (o.device_ns) i.last_device_ns = o.device_ns;
    ++i.observations;
    i.numerator = o.numerator;
    i.denominator_analysed_s = o.denominator_analysed_s;
    i.coverage_note = o.coverage_note;
    i.benign_alternatives = o.benign_alternatives;
    i.quiet_analysed_s = 0;
    if (o.evidence) {
        i.evidence.push_back(*o.evidence);
        while (i.evidence.size() > limits_.max_evidence) { i.evidence.pop_front(); ++i.evidence_dropped; }
    }
    return i.id;
}

void IncidentStore::note_analysed(const std::string& band, int channel, double analysed_s) {
    for (auto& i : incidents_) {
        if (!i.open || i.band != band || i.channel != channel) continue;
        i.quiet_analysed_s += analysed_s;
        if (i.quiet_analysed_s >= limits_.close_after_quiet_analysed_s) i.open = false;
    }
}

size_t IncidentStore::open_count() const {
    return size_t(std::count_if(incidents_.begin(), incidents_.end(), [](const Incident& i) { return i.open; }));
}

json IncidentStore::export_json() const {
    json arr = json::array();
    for (const auto& i : incidents_) {
        json ev = json::array();
        for (const auto& e : i.evidence) {
            const auto& [run, seq, sample, phy] = e.frame;
            ev.push_back({{"run_id", run}, {"capture_seq", seq}, {"sample_start", sample}, {"phy", phy},
                          {"host_ns", e.host_ns}, {"note", e.note}});
        }
        json o = {{"id", i.id}, {"rule", i.rule}, {"rule_version", i.rule_version},
                  {"config_version", i.config_version}, {"severity", severity_name(i.severity)},
                  {"confidence", confidence_name(i.confidence)}, {"open", i.open}, {"band", i.band},
                  {"channel", i.channel}, {"claimed_source", i.claimed_source}, {"target", i.target},
                  {"first_host_ns", i.first_host_ns}, {"last_host_ns", i.last_host_ns},
                  {"observations", i.observations}, {"numerator", i.numerator},
                  {"denominator_analysed_s", i.denominator_analysed_s}, {"coverage_note", i.coverage_note},
                  {"benign_alternatives", i.benign_alternatives}, {"evidence", ev},
                  {"evidence_dropped", i.evidence_dropped}, {"quiet_analysed_s", i.quiet_analysed_s}};
        if (i.first_device_ns) o["first_device_ns"] = *i.first_device_ns;
        if (i.last_device_ns) o["last_device_ns"] = *i.last_device_ns;
        arr.push_back(o);
    }
    return {{"schema", 1}, {"next_id", next_id_}, {"incidents", arr}};
}

size_t IncidentStore::import_json(const json& j) {
    size_t n = 0;
    next_id_ = std::max<uint64_t>(next_id_, j.value("next_id", uint64_t(1)));
    for (const auto& o : j.value("incidents", json::array())) {
        Incident i;
        i.id = o.at("id").get<uint64_t>();
        i.rule = o.value("rule", "");
        i.rule_version = o.value("rule_version", "");
        i.config_version = o.value("config_version", "");
        i.severity = severity_from(o.value("severity", "info"));
        i.confidence = confidence_from(o.value("confidence", "low"));
        i.open = o.value("open", false);
        i.restored = true;  // observation was interrupted by the restart
        i.band = o.value("band", "");
        i.channel = o.value("channel", 0);
        i.claimed_source = o.value("claimed_source", "");
        i.target = o.value("target", "");
        i.first_host_ns = o.value("first_host_ns", int64_t(0));
        i.last_host_ns = o.value("last_host_ns", int64_t(0));
        if (o.contains("first_device_ns")) i.first_device_ns = o.at("first_device_ns").get<int64_t>();
        if (o.contains("last_device_ns")) i.last_device_ns = o.at("last_device_ns").get<int64_t>();
        i.observations = o.value("observations", uint64_t(0));
        i.numerator = o.value("numerator", 0.0);
        i.denominator_analysed_s = o.value("denominator_analysed_s", 0.0);
        i.coverage_note = o.value("coverage_note", "");
        i.benign_alternatives = o.value("benign_alternatives", std::vector<std::string>{});
        for (const auto& e : o.value("evidence", json::array())) {
            IncidentEvidence ev;
            ev.frame = {e.value("run_id", ""), e.value("capture_seq", uint64_t(0)), e.value("sample_start", size_t(0)),
                        e.value("phy", "")};
            ev.host_ns = e.value("host_ns", int64_t(0));
            ev.note = e.value("note", "");
            i.evidence.push_back(ev);
        }
        i.evidence_dropped = o.value("evidence_dropped", uint64_t(0));
        i.quiet_analysed_s = o.value("quiet_analysed_s", 0.0);
        next_id_ = std::max(next_id_, i.id + 1);
        incidents_.push_back(std::move(i));
        while (incidents_.size() > limits_.max_incidents) { incidents_.pop_front(); ++evicted_; }
        ++n;
    }
    return n;
}

}  // namespace rfmon::wifi_security
