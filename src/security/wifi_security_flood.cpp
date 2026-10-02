#include "security/wifi_security_flood.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::wifi_security {
namespace {
constexpr const char* rule = "management_disconnect_flood";
// Keep the persisted holder name so a v1 interrupted incident can recover.
constexpr const char* holder = "management_disconnect_flood/v1";
}
ManagementFloodRule::ManagementFloodRule(FloodConfig c) : cfg_(c) {
    for (double x : {c.window_s,c.min_baseline_s,c.floor_rate,c.baseline_multiplier,c.release_quiet_s,c.max_gap_s,c.repeat_guard_s})
        if (!std::isfinite(x) || x <= 0) throw std::invalid_argument("Invalid flood time/rate configuration");
    if (!c.max_pending || !c.max_frames_per_capture || !c.max_profiles || !c.max_groups || !c.max_units ||
        !c.min_baseline_windows || !c.min_units || !c.min_active_captures || c.min_ap_targets < 2 || !c.max_window_captures)
        throw std::invalid_argument("Invalid flood capacity configuration");
    config_version_ = nlohmann::json{{"version",2},{"enabled",c.enabled},{"window_s",c.window_s},
        {"min_baseline_s",c.min_baseline_s},{"min_windows",c.min_baseline_windows},{"floor",c.floor_rate},
        {"multiplier",c.baseline_multiplier},{"min_units",c.min_units},{"active_captures",c.min_active_captures},
        {"release_s",c.release_quiet_s},{"gap_s",c.max_gap_s},{"repeat_guard_s",c.repeat_guard_s},
        {"min_ap_targets",c.min_ap_targets},{"window_mode","rolling_whole_captures"},
        {"max_window_captures",c.max_window_captures},{"max_units",c.max_units},
        {"max_groups",c.max_groups},{"max_profiles",c.max_profiles},
        {"max_pending",c.max_pending},{"max_frames_per_capture",c.max_frames_per_capture}}.dump();
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
    // Client-originated disconnects target the AP. Aggregate affected clients,
    // rather than mistaking the AP receiver address for a single client.
    if (u.target == u.bssid) u.target = u.source;
    u.hash = f.content_hash_retry_invariant; u.retry = f.fc.retry;
    if (e.clock == ClockDomain::UsrpDevice && !e.after_overflow) u.device_ns = e.device_time_ns;
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
    for (auto& u : p.units) {
        const size_t sample = std::get<2>(u.evidence.frame);
        const size_t analysed = c.analysed_samples ? std::min(c.analysed_samples,c.samples_received) : c.samples_received;
        if (sample >= analysed) p.degraded = true;
        if (u.device_ns != device_time_of_sample(c,sample)) u.device_ns.reset();
    }
    if (lost_ || p.degraded || p.received != c.events_submitted) result.usable = false;
    lost_ = false;
    if (!result.usable) {
        ++excluded_; windows_.erase(key);
        return result; // gaps cannot release persisted holds or establish quiet
    }
    if (!windows_.count(key) && windows_.size() >= cfg_.max_profiles) windows_.erase(windows_.begin());
    auto& w = windows_[key];
    const auto* baseline = baselines.summary(key);
    const uint32_t version = baseline ? baseline->version : 0;
    const bool device = c.clock == ClockDomain::UsrpDevice;
    const int64_t start = device ? c.device_time_ns : c.host_before_ns;
    const bool new_context = w.run != c.run_id || w.session != c.radio_session ||
        (w.baseline_version && w.baseline_version != version) || w.device_clock != device;
    if (!new_context && start && w.end_ns && start < w.end_ns) {
        // Keep the high-water clock but invalidate the partial comparison.
        w.segments.clear(); w.groups.clear(); w.seconds = 0; w.units = 0; w.quiet = 0;
        ++excluded_; result.usable = false; return result;
    }
    const bool gap = !start || !w.end_ns || double(start-w.end_ns)/1e9 > cfg_.max_gap_s;
    if (new_context || gap) w = Window{};
    w.run = c.run_id; w.session = c.radio_session; w.baseline_version = version; w.device_clock = device;
    w.end_ns = start ? start + int64_t(c.observed_seconds()*1e9) : 0;
    // Sample order is authoritative; queue arrival order need not be.
    std::stable_sort(p.units.begin(),p.units.end(),[](const Unit& a,const Unit& b) {
        return std::get<2>(a.evidence.frame) < std::get<2>(b.evidence.frame);
    });
    w.segments.push_back({c.capture_seq,c.analysed_seconds(),std::move(p.units)});
    w.seconds += c.analysed_seconds();
    // Retain the shortest whole-capture suffix covering window_s and the
    // required capture count (long captures still need sustained evidence).
    // Evaluate at
    // every capture end: activity straddling an old fixed boundary stays visible.
    while (w.segments.size() > cfg_.min_active_captures && w.seconds-w.segments.front().seconds >= cfg_.window_s) {
        w.seconds -= w.segments.front().seconds; w.segments.pop_front();
    }
    w.groups.clear(); w.units = 0;
    auto overload = [&] {
        ++excluded_; result.usable = false; w = Window{};
        baselines.set_hold(key,holder,true);
    };
    if (w.segments.size() > cfg_.max_window_captures) { overload(); return result; }
    w.first_capture = w.segments.front().capture;
    for (const auto& segment : w.segments) {
        std::set<std::pair<std::string,std::string>> active;
        for (const auto& u : segment.units) {
            if (++w.units > cfg_.max_units) { overload(); return result; }
            // Empty target is a separate AP-wide scope, never a MAC address.
            for (const std::string& target : {u.target,std::string{}}) {
                auto gkey = std::make_pair(u.bssid,target);
                if (!w.groups.count(gkey) && w.groups.size() >= cfg_.max_groups) { overload(); return result; }
                auto& g = w.groups[gkey];
                if (!g.raw) g.first = u;
                g.last = u; ++g.raw; g.targets.insert(u.target);
                const bool distinct = g.unique.insert(u.hash).second;
                auto& last = g.last_copy[u.hash];
                const bool separated = !u.retry && device && u.device_ns && last &&
                    *u.device_ns > *last && double(*u.device_ns-*last)/1e9 >= cfg_.repeat_guard_s;
                if (distinct || separated) {
                    ++g.independent;
                    if (u.device_ns) last = u.device_ns;
                    active.insert(gkey);
                    if (segment.capture == c.capture_seq) ++g.latest_units;
                }
            }
        }
        for (const auto& gkey : active) ++w.groups[gkey].active_captures;
    }
    bool candidate = false;
    for (const auto& [gkey,g] : w.groups)
        if (double(g.latest_units)/c.analysed_seconds() >= cfg_.floor_rate) candidate = true;
    if (candidate) {
        // The current samples must not train the reference, even before maturity.
        if (!baselines.set_hold(key,holder,true)) result.usable = false;
        w.quiet = 0;
        for (const auto& i : incidents.incidents()) {
            if (!i.open || i.rule != rule || i.baseline_key != key) continue;
            auto g = w.groups.find({i.claimed_bssid,i.target});
            if (g != w.groups.end() && double(g->second.latest_units)/c.analysed_seconds() >= cfg_.floor_rate) {
                result.observed_incidents.insert(i.id); incidents.reset_quiet(i.id);
            }
        }
    } else {
        w.quiet += c.analysed_seconds();
        if (w.quiet >= cfg_.release_quiet_s) baselines.set_hold(key,holder,false);
    }
    if (w.seconds < cfg_.window_s) return result;
    ++evaluations_;
    const bool mature = baseline && baseline->windows_included >= cfg_.min_baseline_windows &&
        baseline->learned_analysed_s >= cfg_.min_baseline_s;
    const auto rates = baselines.combined_rate(key,{BaselineMetric::Deauth,BaselineMetric::Disassoc});
    const double threshold = std::max(cfg_.floor_rate,cfg_.baseline_multiplier*rates.p95);
    auto qualifies = [&](const Group& g,size_t units) {
        return mature && g.latest_units && units >= cfg_.min_units &&
            g.active_captures >= cfg_.min_active_captures && double(units)/w.seconds >= threshold;
    };
    std::set<std::string> target_qualified;
    for (const auto& [gkey,g] : w.groups)
        if (!gkey.second.empty() && qualifies(g,g.independent)) target_qualified.insert(gkey.first);
    for (const auto& [gkey,g] : w.groups) {
        const bool ap = gkey.second.empty();
        if (ap && (g.targets.size() < cfg_.min_ap_targets || target_qualified.count(gkey.first))) continue;
        const bool distinct = qualifies(g,g.unique.size());
        const size_t units = distinct ? g.unique.size() : g.independent;
        if (!qualifies(g,units)) continue;
        if (!baselines.set_hold(key,holder,true)) result.usable = false;
        w.quiet = 0;
        IncidentObservation o;
        o.rule = rule; o.rule_version = "2"; o.config_version = config_version_;
        o.severity = IncidentSeverity::Medium; o.confidence = IncidentConfidence::Medium;
        o.band = c.band; o.channel = c.channel; o.claimed_bssid = gkey.first; o.target = gkey.second;
        o.claimed_source = g.last.source; o.host_ns = c.host_before_ns;
        if (device) o.device_ns = c.device_time_ns;
        o.numerator = units; o.denominator_analysed_s = w.seconds;
        o.mode = "observation"; o.tier = "suspected";
        o.path = ap ? (distinct ? "ap_wide_distinct_management_frames" : "ap_wide_repeated_management_frames") :
                      (distinct ? "distinct_management_frames" : "repeated_management_frames");
        o.baseline_key = key; o.baseline_version = version;
        o.measures = {{"rate",double(units)/w.seconds},{"threshold",threshold},{"raw_frames",double(g.raw)},
            {"distinct_units",double(g.unique.size())},{"independent_units",double(g.independent)},
            {"targets",double(g.targets.size())},{"active_captures",double(g.active_captures)},
            {"baseline_p95",rates.p95},{"baseline_seconds",baseline->learned_analysed_s}};
        o.context = {{"scope",ap ? "claimed_bssid" : "target"},{"timing","rolling whole-capture exposure"}};
        o.confidence_basis = {"FCS-valid management headers", "multiple active captures", "mature receiver-matched baseline"};
        o.coverage_note = "Decoded units per analysed second; rolling whole captures can exceed the configured window. Repeated-content units require device timing, Retry clear and guard separation. No proof of service impact.";
        o.benign_alternatives = {"AP/client restart or reconfiguration", "reconnect storm", "sender spoofing; PMF acceptance unknown"};
        o.evidence = g.first.evidence; o.extra_evidence.push_back(g.last.evidence);
        auto id = incidents.observe(o); result.observed_incidents.insert(id);
        IncidentPoint point;
        point.host_ns = o.host_ns; point.device_ns = o.device_ns; point.run_id = c.run_id;
        point.capture_seq_first = w.first_capture; point.capture_seq_last = c.capture_seq;
        point.analysed_s = w.seconds; point.units = units; point.raw = g.raw;
        point.rate = o.measures.at("rate"); point.threshold = threshold; point.confidence = "medium";
        point.governing_term = threshold == cfg_.floor_rate ? "provisional floor" : "channel baseline p95 multiplier";
        point.path = o.path; incidents.append_point(id,point);
    }
    for (const auto& i : incidents.incidents()) {
        if (!i.open || i.rule != rule || i.baseline_key != key || result.observed_incidents.count(i.id)) continue;
        auto g = w.groups.find({i.claimed_bssid,i.target});
        IncidentPoint point;
        point.run_id = c.run_id; point.host_ns = c.host_before_ns;
        point.capture_seq_first = w.first_capture; point.capture_seq_last = c.capture_seq;
        point.analysed_s = w.seconds; point.threshold = threshold; point.path = i.path;
        if (g != w.groups.end()) {
            point.units = i.path.find("repeated") != std::string::npos ? g->second.independent : g->second.unique.size();
            point.raw = g->second.raw;
        }
        point.rate = point.units/w.seconds; point.tags = "observed recovery; rolling window";
        point.governing_term = "baseline/floor"; incidents.append_point(i.id,point);
    }
    return result;
}
}
