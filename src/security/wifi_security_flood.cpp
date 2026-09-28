#include "security/wifi_security_flood.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::wifi_security {
namespace {
constexpr const char* rule = "management_disconnect_flood";
constexpr const char* holder = "management_disconnect_flood/v1";
}
ManagementFloodRule::ManagementFloodRule(FloodConfig c) : cfg_(c) {
    for (double x : {c.window_s,c.min_baseline_s,c.floor_rate,c.baseline_multiplier,c.release_quiet_s,c.max_gap_s})
        if (!std::isfinite(x) || x <= 0) throw std::invalid_argument("Invalid flood time/rate configuration");
    if (!c.max_pending || !c.max_frames_per_capture || !c.max_profiles || !c.max_groups || !c.max_units ||
        !c.min_baseline_windows || !c.min_units || !c.min_active_captures)
        throw std::invalid_argument("Invalid flood capacity configuration");
    config_version_ = nlohmann::json{{"version",1},{"enabled",c.enabled},{"window_s",c.window_s},
        {"min_baseline_s",c.min_baseline_s},{"min_windows",c.min_baseline_windows},{"floor",c.floor_rate},
        {"multiplier",c.baseline_multiplier},{"min_units",c.min_units},{"active_captures",c.min_active_captures},
        {"release_s",c.release_quiet_s},{"gap_s",c.max_gap_s}}.dump();
}
void ManagementFloodRule::interrupt() {
    pending_.clear(); windows_.clear(); lost_ = true;
}
void ManagementFloodRule::note_frame(const FrameEvent& e, const MacFrame& f) {
    if (!cfg_.enabled) return;
    auto key = std::make_pair(e.run_id,e.capture_seq);
    if (!pending_.count(key) && pending_.size() >= cfg_.max_pending) interrupt();
    auto& p = pending_[key];
    ++p.received;
    if (!(f.is_management(mgmt::Deauthentication) || f.is_management(mgmt::Disassociation))) return;
    if (!f.bssid || !f.receiver || !f.transmitter || f.body_state == BodyState::Truncated ||
        f.body_state == BodyState::Malformed) return;
    if (p.units.size() >= cfg_.max_frames_per_capture) { p.degraded = true; return; }
    Unit u;
    u.bssid = format_mac(*f.bssid); u.target = format_mac(*f.receiver); u.source = format_mac(*f.transmitter);
    u.hash = f.content_hash_retry_invariant;
    u.evidence.frame = ingest_key(e); u.evidence.host_ns = e.host_time_ns.value_or(0);
    u.evidence.summary = describe(f); u.evidence.note = "FCS-valid; sender and service impact unverified";
    u.evidence.mpdu.assign(e.mpdu.begin(),e.mpdu.begin()+std::min(e.mpdu.size(),size_t(512)));
    p.units.push_back(std::move(u));
}
FloodResult ManagementFloodRule::finish_capture(const CaptureRecord& c, BaselineStore& baselines, IncidentStore& incidents) {
    FloodResult result;
    result.usable = c.security_usable();
    if (!cfg_.enabled) return result;
    Pending p;
    auto pit = pending_.find({c.run_id,c.capture_seq});
    if (pit != pending_.end()) { p = std::move(pit->second); pending_.erase(pit); }
    const std::string key = BaselineStore::key_for(c);
    if (lost_ || p.degraded || (c.events_submitted && p.received != c.events_submitted)) result.usable = false;
    lost_ = false;
    if (!result.usable) {
        ++excluded_; windows_.erase(key);
        // Persisted holds remain until actual clean quiet exposure releases them.
        return result;
    }
    if (!windows_.count(key) && windows_.size() >= cfg_.max_profiles) {
        windows_.erase(windows_.begin()); // any hold deliberately survives eviction
    }
    auto& w = windows_[key];
    const auto* baseline = baselines.summary(key);
    const uint32_t version = baseline ? baseline->version : 0;
    const bool device = c.clock == ClockDomain::UsrpDevice;
    const int64_t start = device ? c.device_time_ns : c.host_before_ns;
    const bool new_context = w.run != c.run_id || w.session != c.radio_session ||
        (w.baseline_version && w.baseline_version != version) || w.device_clock != device;
    if (!new_context && start && w.end_ns && start < w.end_ns) {
        ++excluded_; result.usable = false; return result;
    }
    const bool gap = !start || !w.end_ns || start < w.end_ns ||
        double(start-w.end_ns)/1e9 > cfg_.max_gap_s;
    if (new_context || gap) w = Window{};
    w.run = c.run_id; w.session = c.radio_session; w.baseline_version = version; w.device_clock = device;
    w.end_ns = start ? start + int64_t(c.observed_seconds()*1e9) : 0;
    if (!w.seconds) w.first_capture = c.capture_seq;
    w.seconds += c.analysed_seconds();
    std::map<std::pair<std::string,std::string>,size_t> added;
    for (auto& u : p.units) {
        auto gkey = std::make_pair(u.bssid,u.target);
        if (!w.groups.count(gkey) && w.groups.size() >= cfg_.max_groups) { result.usable = false; break; }
        auto& g = w.groups[gkey];
        if (!g.raw) g.first = u;
        g.last = u; ++g.raw;
        if (g.unique.insert(u.hash).second) { ++w.units; ++added[gkey]; }
        if (w.units > cfg_.max_units) { result.usable = false; break; }
    }
    if (!result.usable) { ++excluded_; w = Window{}; baselines.set_hold(key,holder,true); return result; }
    bool candidate = false;
    for (auto& [gkey,n] : added) {
        auto& g = w.groups[gkey]; ++g.active_captures;
        if (double(n)/c.analysed_seconds() >= cfg_.floor_rate) candidate = true;
    }
    if (candidate) {
        // Must precede note_capture(): the triggering samples cannot train the reference.
        if (!baselines.set_hold(key,holder,true)) result.usable = false;
        w.quiet = 0;
        for (const auto& i : incidents.incidents())
            if (i.open && i.rule == rule && i.baseline_key == key) {
                result.observed_incidents.insert(i.id); incidents.reset_quiet(i.id);
            }
    } else {
        w.quiet += c.analysed_seconds();
        if (w.quiet >= cfg_.release_quiet_s) baselines.set_hold(key,holder,false);
    }
    if (w.seconds < cfg_.window_s) return result;
    ++evaluations_;
    const bool mature = baseline && baseline->windows_included >= cfg_.min_baseline_windows &&
        baseline->learned_analysed_s >= cfg_.min_baseline_s;
    // Baseline is channel-wide raw deauth+disassoc; comparing target-specific
    // distinct units to it is deliberately conservative, not a per-client model.
    const auto rates = baselines.combined_rate(key,{BaselineMetric::Deauth,BaselineMetric::Disassoc});
    const double threshold = std::max(cfg_.floor_rate,cfg_.baseline_multiplier*rates.p95);
    for (const auto& [gkey,g] : w.groups) {
        const double rate = double(g.unique.size())/w.seconds;
        if (!mature || g.unique.size() < cfg_.min_units || g.active_captures < cfg_.min_active_captures || rate < threshold) continue;
        if (!baselines.set_hold(key,holder,true)) result.usable = false;
        w.quiet = 0;
        IncidentObservation o;
        o.rule = rule; o.rule_version = "1"; o.config_version = config_version_;
        o.severity = IncidentSeverity::Medium; o.confidence = IncidentConfidence::Medium;
        o.band = c.band; o.channel = c.channel; o.claimed_bssid = gkey.first; o.target = gkey.second;
        o.claimed_source = g.last.source; o.host_ns = c.host_before_ns;
        if (device) o.device_ns = c.device_time_ns;
        o.numerator = g.unique.size(); o.denominator_analysed_s = w.seconds;
        o.mode = "observation"; o.tier = "suspected"; o.path = "distinct_management_frames";
        o.baseline_key = key; o.baseline_version = version;
        o.measures = {{"rate",rate},{"threshold",threshold},{"raw_frames",double(g.raw)},
            {"baseline_p95",rates.p95},{"baseline_seconds",baseline->learned_analysed_s}};
        o.confidence_basis = {"FCS-valid management headers", "multiple active captures", "mature receiver-matched baseline"};
        o.coverage_note = "Observed decoded units only; retry-invariant repeats counted once per window. No proof of service impact.";
        o.benign_alternatives = {"AP/client restart or reconfiguration", "reconnect storm", "sender spoofing; PMF acceptance unknown"};
        o.evidence = g.first.evidence; o.extra_evidence.push_back(g.last.evidence);
        auto id = incidents.observe(o); result.observed_incidents.insert(id);
        IncidentPoint point;
        point.host_ns = o.host_ns; point.device_ns = o.device_ns; point.run_id = c.run_id;
        point.capture_seq_first = w.first_capture; point.capture_seq_last = c.capture_seq;
        point.analysed_s = w.seconds; point.units = g.unique.size(); point.raw = g.raw;
        point.rate = rate; point.threshold = threshold; point.confidence = "medium";
        point.governing_term = threshold == cfg_.floor_rate ? "provisional floor" : "channel baseline p95 multiplier";
        point.path = o.path; incidents.append_point(id,point);
    }
    // Include observed recovery points for open incidents, without fabricating
    // points through gaps or degraded captures.
    for (const auto& i : incidents.incidents()) {
        if (!i.open || i.rule != rule || i.baseline_key != key || result.observed_incidents.count(i.id)) continue;
        const auto g = w.groups.find({i.claimed_bssid,i.target});
        IncidentPoint point;
        point.run_id = c.run_id; point.host_ns = c.host_before_ns;
        point.capture_seq_first = w.first_capture; point.capture_seq_last = c.capture_seq;
        point.analysed_s = w.seconds; point.threshold = threshold;
        if (g != w.groups.end()) { point.units = g->second.unique.size(); point.raw = g->second.raw; }
        point.rate = point.units/w.seconds; point.tags = "observed recovery";
        point.governing_term = "baseline/floor"; incidents.append_point(i.id,point);
    }
    // Non-overlapping usable-exposure windows; retain hold recovery and clock context.
    w.seconds = 0; w.units = 0; w.groups.clear();
    return result;
}
}
