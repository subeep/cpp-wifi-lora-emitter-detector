#include "security/wifi_security_incidents.hpp"

#include "security/wifi_security_io.hpp"

#include <algorithm>
#include <cmath>

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

void IncidentStore::bound(Incident& i) {
    auto text = [&](std::string& s) { if (s.size() > limits_.max_text_bytes) s.resize(limits_.max_text_bytes); };
    auto strings = [&](auto& v) { if (v.size() > limits_.max_context_items) v.resize(limits_.max_context_items); for (auto& s : v) text(s); };
    for (auto* s : {&i.rule, &i.rule_version, &i.config_version, &i.band, &i.claimed_source,
                   &i.claimed_bssid, &i.target, &i.coverage_note, &i.mode, &i.tier, &i.path, &i.baseline_key}) text(*s);
    strings(i.benign_alternatives); strings(i.confidence_basis);
    while (i.measures.size() > limits_.max_context_items) i.measures.erase(std::prev(i.measures.end()));
    while (i.context.size() > limits_.max_context_items) i.context.erase(std::prev(i.context.end()));
    for (auto it = i.context.begin(); it != i.context.end();) {
        if (it->first.size() > limits_.max_text_bytes) it = i.context.erase(it);
        else { text(it->second); ++it; }
    }
    for (auto it = i.measures.begin(); it != i.measures.end();) {
        if (it->first.size() > limits_.max_text_bytes || !std::isfinite(it->second)) it = i.measures.erase(it);
        else ++it;
    }
    if (i.first_evidence.size() > limits_.max_first_evidence) {
        i.evidence_dropped += i.first_evidence.size() - limits_.max_first_evidence;
        i.first_evidence.resize(limits_.max_first_evidence);
    }
    while (i.evidence.size() > limits_.max_evidence) { i.evidence.pop_front(); ++i.evidence_dropped; }
    auto evidence = [&](IncidentEvidence& e) {
        if (e.mpdu.size() > limits_.max_evidence_bytes) e.mpdu.resize(limits_.max_evidence_bytes);
        text(e.note); text(e.role); text(e.summary); text(std::get<0>(e.frame)); text(std::get<3>(e.frame));
    };
    for (auto& e : i.evidence) evidence(e);
    for (auto& e : i.first_evidence) evidence(e);
    while (i.timeline.size() > limits_.max_timeline) i.timeline.pop_front();
    for (auto& p : i.timeline) {
        text(p.run_id); text(p.governing_term); text(p.confidence); text(p.path); text(p.tags);
    }
}

uint64_t IncidentStore::observe(const IncidentObservation& o) {
    if (!limits_.max_incidents) return 0;
    ++revision_;
    auto it = std::find_if(incidents_.begin(), incidents_.end(), [&](const Incident& i) {
        if (!i.open || i.rule != o.rule || i.band != o.band || i.channel != o.channel || i.target != o.target)
            return false;
        if (!o.claimed_bssid.empty()) return i.claimed_bssid == o.claimed_bssid;
        return i.claimed_bssid.empty() && i.claimed_source == o.claimed_source;
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
        n.claimed_bssid = o.claimed_bssid;
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
    i.claimed_source = o.claimed_source;  // display only when claimed_bssid keys the incident
    if (!o.mode.empty()) i.mode = o.mode;
    if (!o.tier.empty()) i.tier = o.tier;
    if (!o.path.empty()) i.path = o.path;
    if (!o.baseline_key.empty()) { i.baseline_key = o.baseline_key; i.baseline_version = o.baseline_version; }
    if (!o.confidence_basis.empty()) i.confidence_basis = o.confidence_basis;
    for (const auto& [k, v] : o.measures) i.measures[k] = v;
    for (const auto& [k, v] : o.context) i.context[k] = v;
    i.peak_confidence = std::max(i.peak_confidence, o.confidence);
    if (o.denominator_analysed_s > 0) i.peak_rate = std::max(i.peak_rate, o.numerator / o.denominator_analysed_s);
    auto add_evidence = [&](const IncidentEvidence& e) {
        if (i.first_evidence.size() < limits_.max_first_evidence) { i.first_evidence.push_back(e); return; }
        i.evidence.push_back(e);
        while (i.evidence.size() > limits_.max_evidence) { i.evidence.pop_front(); ++i.evidence_dropped; }
    };
    if (o.evidence) add_evidence(*o.evidence);
    for (const auto& e : o.extra_evidence) add_evidence(e);
    bound(i);
    return i.id;
}

void IncidentStore::note_analysed(const std::string& band, int channel, double analysed_s,
                                  const std::set<uint64_t>* skip, const std::string& baseline_key) {
    if (!std::isfinite(analysed_s) || analysed_s <= 0) return;
    for (auto& i : incidents_) {
        if (!i.open || i.band != band || i.channel != channel) continue;
        if (skip && skip->count(i.id)) continue;
        if (!baseline_key.empty() && !i.baseline_key.empty() && i.baseline_key != baseline_key) continue;
        ++revision_;
        i.quiet_analysed_s += analysed_s;
        if (i.quiet_analysed_s >= limits_.close_after_quiet_analysed_s) i.open = false;
    }
}

void IncidentStore::reset_quiet(uint64_t id) {
    for (auto& i : incidents_) if (i.id == id && i.open) {
        i.quiet_analysed_s = 0; ++revision_; return;
    }
}

void IncidentStore::append_point(uint64_t id, const IncidentPoint& p) {
    for (auto& i : incidents_) {
        if (i.id != id) continue;
        ++revision_;
        i.timeline.push_back(p);
        bound(i);
        return;
    }
}

size_t IncidentStore::open_count() const {
    return size_t(std::count_if(incidents_.begin(), incidents_.end(), [](const Incident& i) { return i.open; }));
}

namespace {
json evidence_json(const IncidentEvidence& e) {
    const auto& [run, seq, sample, phy] = e.frame;
    json o = {{"run_id", run}, {"capture_seq", seq}, {"sample_start", sample}, {"phy", phy},
              {"host_ns", e.host_ns}, {"note", e.note}};
    if (!e.role.empty()) o["role"] = e.role;
    if (!e.summary.empty()) o["summary"] = e.summary;
    if (!e.mpdu.empty()) o["mpdu"] = to_hex(e.mpdu);
    return o;
}
IncidentEvidence evidence_from(const json& e) {
    IncidentEvidence ev;
    ev.frame = {e.value("run_id", ""), e.value("capture_seq", uint64_t(0)), e.value("sample_start", size_t(0)),
                e.value("phy", "")};
    ev.host_ns = e.value("host_ns", int64_t(0));
    ev.note = e.value("note", "");
    ev.role = e.value("role", "");
    ev.summary = e.value("summary", "");
    if (e.contains("mpdu")) from_hex(e.at("mpdu").get<std::string>(), ev.mpdu);
    return ev;
}
json point_json(const IncidentPoint& p) {
    json o = {{"host_ns", p.host_ns}, {"run_id", p.run_id}, {"capture_seq_first", p.capture_seq_first},
              {"capture_seq_last", p.capture_seq_last}, {"a_start", p.a_start}, {"analysed_s", p.analysed_s},
              {"units", p.units}, {"raw", p.raw}, {"rate", p.rate}, {"threshold", p.threshold},
              {"governing_term", p.governing_term}, {"confidence", p.confidence}, {"path", p.path}, {"tags", p.tags}};
    if (p.device_ns) o["device_ns"] = *p.device_ns;
    return o;
}
IncidentPoint point_from(const json& o) {
    IncidentPoint p;
    p.host_ns = o.value("host_ns", int64_t(0));
    if (o.contains("device_ns")) p.device_ns = o.at("device_ns").get<int64_t>();
    p.run_id = o.value("run_id", "");
    p.capture_seq_first = o.value("capture_seq_first", uint64_t(0));
    p.capture_seq_last = o.value("capture_seq_last", uint64_t(0));
    p.a_start = o.value("a_start", 0.0);
    p.analysed_s = o.value("analysed_s", 0.0);
    p.units = o.value("units", 0.0);
    p.raw = o.value("raw", 0.0);
    p.rate = o.value("rate", 0.0);
    p.threshold = o.value("threshold", 0.0);
    p.governing_term = o.value("governing_term", "");
    p.confidence = o.value("confidence", "");
    p.path = o.value("path", "");
    p.tags = o.value("tags", "");
    return p;
}
}  // namespace

json incident_json(const Incident& i) {
    json ev = json::array(), first = json::array(), tl = json::array();
    for (const auto& e : i.evidence) ev.push_back(evidence_json(e));
    for (const auto& e : i.first_evidence) first.push_back(evidence_json(e));
    for (const auto& p : i.timeline) tl.push_back(point_json(p));
    json o = {{"id", i.id}, {"rule", i.rule}, {"rule_version", i.rule_version},
              {"config_version", i.config_version}, {"severity", severity_name(i.severity)},
              {"confidence", confidence_name(i.confidence)}, {"peak_confidence", confidence_name(i.peak_confidence)},
              {"open", i.open}, {"restored", i.restored}, {"band", i.band},
              {"channel", i.channel}, {"claimed_source", i.claimed_source}, {"claimed_bssid", i.claimed_bssid},
              {"target", i.target}, {"first_host_ns", i.first_host_ns}, {"last_host_ns", i.last_host_ns},
              {"observations", i.observations}, {"numerator", i.numerator},
              {"denominator_analysed_s", i.denominator_analysed_s}, {"peak_rate", i.peak_rate},
              {"coverage_note", i.coverage_note}, {"benign_alternatives", i.benign_alternatives},
              {"mode", i.mode}, {"tier", i.tier}, {"path", i.path}, {"baseline_key", i.baseline_key},
              {"baseline_version", i.baseline_version}, {"confidence_basis", i.confidence_basis},
              {"measures", i.measures}, {"context", i.context}, {"evidence", ev}, {"first_evidence", first},
              {"timeline", tl}, {"evidence_dropped", i.evidence_dropped}, {"quiet_analysed_s", i.quiet_analysed_s}};
    if (i.first_device_ns) o["first_device_ns"] = *i.first_device_ns;
    if (i.last_device_ns) o["last_device_ns"] = *i.last_device_ns;
    return o;
}

json IncidentStore::export_json() const {
    json arr = json::array();
    for (const auto& i : incidents_) arr.push_back(incident_json(i));
    return {{"schema", 1}, {"next_id", next_id_}, {"incidents", arr}};
}

size_t IncidentStore::import_json(const json& j) {
    if (!limits_.max_incidents) return 0;
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
        i.peak_confidence = confidence_from(o.value("peak_confidence", confidence_name(i.confidence)));
        i.open = o.value("open", false);
        i.restored = true;  // observation was interrupted by the restart
        i.band = o.value("band", "");
        i.channel = o.value("channel", 0);
        i.claimed_source = o.value("claimed_source", "");
        i.claimed_bssid = o.value("claimed_bssid", "");
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
        for (const auto& e : o.value("evidence", json::array())) i.evidence.push_back(evidence_from(e));
        for (const auto& e : o.value("first_evidence", json::array())) i.first_evidence.push_back(evidence_from(e));
        for (const auto& p : o.value("timeline", json::array())) i.timeline.push_back(point_from(p));
        i.mode = o.value("mode", "");
        i.tier = o.value("tier", "");
        i.path = o.value("path", "");
        i.baseline_key = o.value("baseline_key", "");
        i.baseline_version = o.value("baseline_version", 0u);
        i.confidence_basis = o.value("confidence_basis", std::vector<std::string>{});
        i.measures = o.value("measures", std::map<std::string, double>{});
        i.context = o.value("context", std::map<std::string, std::string>{});
        i.peak_rate = o.value("peak_rate", 0.0);
        i.evidence_dropped = o.value("evidence_dropped", uint64_t(0));
        i.quiet_analysed_s = 0; // a restart interrupts evidence of continuous quiet
        bound(i);
        next_id_ = std::max(next_id_, i.id + 1);
        incidents_.push_back(std::move(i));
        while (incidents_.size() > limits_.max_incidents) { incidents_.pop_front(); ++evicted_; }
        ++n;
    }
    return n;
}

}  // namespace rfmon::wifi_security
