#include "security/wifi_security_rf_identity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rfmon::wifi_security {
namespace {

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}
double spread(const std::vector<double>& v, double floor) {
    const double m = median(v);
    std::vector<double> d;
    for (double x : v) d.push_back(std::abs(x - m));
    return std::max(1.4826 * median(d), floor);
}
std::string length_class(const FrameEvent& e) {
    if (e.rf_method.rfind("DSSS", 0) == 0) return "dsss";
    return e.rf_symbols < 20 ? "ofdm-short" : "ofdm-long";
}
std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

// The frame's own addresses decide when they can: from its BSSID it claims
// the AP. Without a BSSID, only an address seen as an AP and never as a
// client counts as one.
bool RfIdentityTracker::claimed_client(const RfObservation& o, const Role& r) {
    if (!o.bssid.empty()) return o.bssid != o.transmitter;
    return r.client || !r.ap;
}

void RfIdentityTracker::add(const RfObservation& o) {
    if (!cfg_.enabled) return;
    const Key key{o.band, o.channel, o.transmitter};
    if (!ids_.count(key) && ids_.size() >= cfg_.max_identities) {
        // Forget the identity heard least recently.
        auto oldest = ids_.begin();
        for (auto it = ids_.begin(); it != ids_.end(); ++it)
            if (!it->second.empty() && (oldest->second.empty() || it->second.back().o.t_ns < oldest->second.back().o.t_ns)) oldest = it;
        touched_.erase(oldest->first);
        roles_.erase(oldest->first);
        ids_.erase(oldest);
    }
    auto& role = roles_[key];
    if (!o.bssid.empty() && o.bssid == o.transmitter) role.ap = true;
    if (o.client_frame) role.client = true;
    auto& d = ids_[key];
    d.push_back({o});
    const int64_t keep = int64_t(std::max(2 * cfg_.window_s, cfg_.fallback_window_s) * 1e9);
    while (d.size() > cfg_.history_per_identity || (d.size() > 1 && d.back().o.t_ns - d.front().o.t_ns > keep)) d.pop_front();
    touched_.insert(key);
}

std::vector<RfFinding> RfIdentityTracker::evaluate() {
    std::vector<RfFinding> out;
    const int64_t w = int64_t(cfg_.window_s * 1e9);
    for (const auto& key : touched_) {
        auto it = ids_.find(key);
        if (it == ids_.end()) continue;
        auto& d = it->second;
        const Role role = roles_[key];
        // Reference: the sender's own non-disconnect frames near t, same length class if possible.
        struct Ref { double centre = 0, spread = 0, allowance = 0; size_t frames = 0; };
        // Live, an AP's carrier held within 0.26 ppm (p99.9) of its frames up
        // to two minutes older, so a quiet sender falls back to the last minute.
        const int64_t fallback = std::max(w, int64_t(cfg_.fallback_window_s * 1e9));
        auto reference = [&](int64_t t, const std::string& cls) -> std::optional<Ref> {
            std::vector<double> same, all;
            for (int64_t span : {w, fallback}) {
                same.clear(); all.clear();
                for (const auto& r : d)
                    if (!r.o.disconnect && std::llabs(r.o.t_ns - t) <= span) {
                        all.push_back(r.o.cfo_ppm);
                        if (r.o.length_class == cls) same.push_back(r.o.cfo_ppm);
                    }
                if (all.size() >= cfg_.min_reference) break;
            }
            Ref ref;
            const std::vector<double>* v = &same;
            if (same.size() < cfg_.min_reference) {
                if (all.size() < cfg_.min_reference) return std::nullopt;
                v = &all;
                ref.allowance = cfg_.mixed_class_allowance_ppm;  // short/long frames differ slightly per device
            }
            ref.centre = median(*v);
            ref.spread = spread(*v, cfg_.spread_floor_ppm);
            ref.frames = v->size();
            return ref;
        };

        // Rule 1: forged disconnect frames.
        bool new_inconsistent = false;
        for (auto& e : d) {
            if (!e.o.disconnect || e.judged) continue;
            e.judged = true;
            ++judged_;
            const auto ref = reference(e.o.t_ns, e.o.length_class);
            if (!ref) { ++unverifiable_; continue; }
            e.evaluable = true;
            const double delta = std::abs(e.o.cfo_ppm - ref->centre);
            // A disconnect from the BSSID claims the AP; otherwise a client,
            // whose pre-corrected carrier moves in steps.
            const double floor = claimed_client(e.o, role) ? cfg_.client_min_delta_ppm : cfg_.min_delta_ppm;
            if (delta >= cfg_.score_threshold * ref->spread + ref->allowance && delta >= floor + ref->allowance) {
                e.inconsistent = true;
                ++inconsistent_;
                new_inconsistent = true;
            }
        }
        if (new_inconsistent) {
            int64_t newest = 0;
            for (const auto& e : d) if (e.o.disconnect && e.evaluable) newest = std::max(newest, e.o.t_ns);
            std::vector<const Entry*> bad;
            size_t evaluated = 0;
            for (const auto& e : d)
                if (e.o.disconnect && e.evaluable && newest - e.o.t_ns <= w) { ++evaluated; if (e.inconsistent) bad.push_back(&e); }
            if (bad.size() >= cfg_.min_inconsistent && double(bad.size()) >= cfg_.min_inconsistent_share * double(evaluated)) {
                RfFinding f;
                f.kind = RfFinding::Kind::ForgedDisconnect;
                const auto& last = bad.back()->o;
                f.band = last.band; f.channel = last.channel; f.transmitter = last.transmitter; f.bssid = last.bssid;
                f.length_class = last.length_class; f.t_ns = last.t_ns;
                f.claimed_client = claimed_client(last, role);
                f.inconsistent = bad.size(); f.evaluated = evaluated;
                std::map<std::string, size_t> targets;
                std::vector<double> suspect;
                for (const auto* b : bad) { ++targets[b->o.receiver]; suspect.push_back(b->o.cfo_ppm); }
                f.target = std::max_element(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
                f.suspect_ppm = median(suspect);
                if (const auto ref = reference(last.t_ns, last.length_class)) {
                    f.reference_ppm = ref->centre; f.reference_spread_ppm = ref->spread; f.reference_frames = ref->frames;
                }
                for (size_t i = bad.size() > 8 ? bad.size() - 8 : 0; i < bad.size(); ++i) f.evidence.push_back(bad[i]->o.evidence);
                out.push_back(std::move(f));
            }
        }

        // Rule 2: two transmitters behind one address (non-disconnect frames
        // around the newest frame). One length class at a time first; then all
        // classes pooled, with the short/long allowance added to the gap, so a
        // clone sending other frame lengths than the owner is still seen.
        if (d.empty() || !role.ap || role.client) continue;
        const int64_t newest = d.back().o.t_ns;
        std::vector<std::pair<std::string, std::vector<const Entry*>>> classes;
        {
            std::map<std::string, std::vector<const Entry*>> by_class;
            std::vector<const Entry*> pooled;
            for (const auto& e : d)
                if (!e.o.disconnect && newest - e.o.t_ns <= w) { by_class[e.o.length_class].push_back(&e); pooled.push_back(&e); }
            for (auto& c : by_class) classes.emplace_back(c.first, std::move(c.second));
            if (classes.size() > 1) classes.emplace_back("mixed", std::move(pooled));
        }
        bool reported = false;
        for (auto& [cls, pts] : classes) {
            const bool mixed = cls == "mixed";
            if (pts.size() < 2 * cfg_.min_cluster || (mixed && reported)) continue;
            std::vector<const Entry*> by_cfo = pts;
            std::sort(by_cfo.begin(), by_cfo.end(), [](const Entry* a, const Entry* b) { return a->o.cfo_ppm < b->o.cfo_ppm; });
            size_t split = 0;
            double widest = -1;
            for (size_t i = cfg_.min_cluster; i + cfg_.min_cluster <= by_cfo.size(); ++i) {
                const double g = by_cfo[i]->o.cfo_ppm - by_cfo[i - 1]->o.cfo_ppm;
                if (g > widest) { widest = g; split = i; }
            }
            if (!split) continue;
            std::vector<double> a, b;
            for (size_t i = 0; i < by_cfo.size(); ++i) (i < split ? a : b).push_back(by_cfo[i]->o.cfo_ppm);
            const double ma = median(a), mb = median(b), sa = spread(a, cfg_.spread_floor_ppm), sb = spread(b, cfg_.spread_floor_ppm);
            const double allowance = mixed ? cfg_.mixed_class_allowance_ppm : 0.0;
            if (mb - ma < std::max(cfg_.min_cluster_gap_ppm, cfg_.cluster_gap_spreads * std::max(sa, sb)) + allowance) continue;
            // Interleaving: both clusters active over the same time, not one step change.
            const double boundary = 0.5 * (by_cfo[split - 1]->o.cfo_ppm + by_cfo[split]->o.cfo_ppm);
            int alternations = 0;
            for (size_t i = 1; i < pts.size(); ++i)
                if ((pts[i]->o.cfo_ppm >= boundary) != (pts[i - 1]->o.cfo_ppm >= boundary)) ++alternations;
            if (alternations < cfg_.min_alternations) continue;
            RfFinding f;
            f.kind = RfFinding::Kind::TwoTransmitters;
            const auto& last = d.back().o;
            f.band = last.band; f.channel = last.channel; f.transmitter = last.transmitter; f.bssid = last.bssid;
            f.length_class = cls; f.t_ns = newest;
            f.a_ppm = ma; f.b_ppm = mb; f.a_spread_ppm = sa; f.b_spread_ppm = sb;
            f.a_frames = a.size(); f.b_frames = b.size(); f.alternations = alternations;
            // Latest evidence from each cluster.
            for (int side = 0; side < 2; ++side) {
                int n = 0;
                for (auto p = pts.rbegin(); p != pts.rend() && n < 4; ++p)
                    if (((*p)->o.cfo_ppm >= boundary) == bool(side)) { f.evidence.push_back((*p)->o.evidence); ++n; }
            }
            out.push_back(std::move(f));
            reported = true;
        }
    }
    touched_.clear();
    return out;
}

void RfIdentityTracker::clear() {
    ids_.clear();
    roles_.clear();
    touched_.clear();
}

RfIdentityRule::RfIdentityRule(RfIdentityConfig cfg) : cfg_(cfg), tracker_(cfg) {
    char b[256];
    std::snprintf(b, sizeof b, "rf-identity-v2 mgmt+data w%.0f fb%.0f ref%zu z%.0f d%.2f client%.2f floor%.2f mix%.2f n%zu share%.2f cl%zu gap%.2f k%.0f alt%d ap-only",
                  cfg.window_s, cfg.fallback_window_s, cfg.min_reference, cfg.score_threshold, cfg.min_delta_ppm, cfg.client_min_delta_ppm, cfg.spread_floor_ppm,
                  cfg.mixed_class_allowance_ppm, cfg.min_inconsistent, cfg.min_inconsistent_share, cfg.min_cluster,
                  cfg.min_cluster_gap_ppm, cfg.cluster_gap_spreads, cfg.min_alternations);
    config_version_ = b;
}

void RfIdentityRule::note_frame(const FrameEvent& e, const MacFrame& f) {
    if (!cfg_.enabled || !e.rf_cfo_early_ppm || !f.transmitter) return;
    // Control frames (RTS, block acks, NDP announcements) are left out: they
    // are often non-HT duplicates over a wide channel, and an AP's RTS bursts
    // were measured live 0.3-0.8 ppm off its own beacons.
    if (f.fc.type == FrameType::Control) return;
    const auto t = e.host_time_ns ? e.host_time_ns : e.device_time_ns;
    if (!t) return;
    ++with_features_;
    if (pending_frames_ >= cfg_.max_pending_frames) { interrupt(); return; }
    RfObservation o;
    o.band = e.band;
    o.channel = e.channel;
    o.transmitter = format_mac(*f.transmitter);
    if (f.receiver) o.receiver = format_mac(*f.receiver);
    if (f.bssid) o.bssid = format_mac(*f.bssid);
    o.disconnect = f.is_management(mgmt::Deauthentication) || f.is_management(mgmt::Disassociation);
    o.client_frame = f.fc.type == FrameType::Data && f.fc.to_ds && !f.fc.from_ds;
    o.length_class = length_class(e);
    o.cfo_ppm = *e.rf_cfo_early_ppm;
    o.t_ns = *t;
    o.evidence.frame = ingest_key(e);
    o.evidence.host_ns = e.host_time_ns.value_or(0);
    o.evidence.summary = describe(f);
    o.evidence.note = "FCS-valid; early CFO " + fmt("%.3f", o.cfo_ppm) + " ppm (" + o.length_class + ")";
    o.evidence.mpdu.assign(e.mpdu.begin(), e.mpdu.begin() + long(std::min(e.mpdu.size(), size_t(256))));
    pending_[{e.run_id, e.capture_seq}].push_back(std::move(o));
    ++pending_frames_;
}

RfIdentityResult RfIdentityRule::finish_capture(const CaptureRecord& c, IncidentStore& incidents) {
    RfIdentityResult result;
    result.usable = c.security_usable();
    if (!cfg_.enabled) return result;
    std::vector<RfObservation> frames;
    if (auto it = pending_.find({c.run_id, c.capture_seq}); it != pending_.end()) {
        frames = std::move(it->second);
        pending_frames_ -= frames.size();
        pending_.erase(it);
    }
    if (!result.usable) { excluded_ += frames.size(); return result; }
    for (const auto& o : frames) tracker_.add(o);
    for (auto& f : tracker_.evaluate()) {
        IncidentObservation o;
        o.rule_version = "2";
        o.config_version = config_version_;
        o.band = f.band;
        o.channel = f.channel;
        o.claimed_source = f.transmitter;
        o.claimed_bssid = f.bssid;
        o.host_ns = f.t_ns;
        o.denominator_analysed_s = c.analysed_seconds();
        o.coverage_note = "Reference: the claimed sender's own verified management and data frames on this channel within ±" +
                          fmt("%.0f", cfg_.window_s) + " s (±" + fmt("%.0f", cfg_.fallback_window_s) +
                          " s when too few); CFO measured over each whole frame, relative to this receiver.";
        o.context["length_class"] = f.length_class;
        if (f.kind == RfFinding::Kind::ForgedDisconnect) {
            const double delta = std::abs(f.suspect_ppm - f.reference_ppm);
            const double share = double(f.inconsistent) / double(std::max<size_t>(1, f.evaluated));
            o.rule = "rf_forged_disconnect";
            o.context["claimed_role"] = f.claimed_client ? "client" : "access point";
            o.target = f.target;
            o.severity = IncidentSeverity::High;
            o.confidence = f.inconsistent >= 10 && share >= 0.8 && delta >= 1.0 ? IncidentConfidence::High : IncidentConfidence::Medium;
            o.numerator = double(f.inconsistent);
            o.measures = {{"inconsistent_frames", double(f.inconsistent)}, {"evaluated_frames", double(f.evaluated)},
                          {"reference_frames", double(f.reference_frames)}, {"reference_cfo_ppm", f.reference_ppm},
                          {"suspect_cfo_ppm", f.suspect_ppm}, {"delta_ppm", delta}, {"reference_spread_ppm", f.reference_spread_ppm}};
            o.confidence_basis = {std::to_string(f.inconsistent) + " of " + std::to_string(f.evaluated) +
                                  " disconnect frames inconsistent with the claimed sender's carrier offset",
                                  "suspect " + fmt("%.3f", f.suspect_ppm) + " ppm vs sender " + fmt("%.3f", f.reference_ppm) + " ppm"};
            o.benign_alternatives = {"a repeater or extender cloning the sender's MAC",
                                     "a second radio of the same device with its own oscillator transmitting under the same MAC"};
            if (f.claimed_client) o.benign_alternatives.push_back("a client that re-adjusted its carrier pre-correction by more than " +
                                                                  fmt("%.1f", cfg_.client_min_delta_ppm) + " ppm");
        } else {
            o.rule = "rf_identity_two_transmitters";
            o.target = "";
            o.severity = IncidentSeverity::Medium;
            o.confidence = f.a_frames >= 10 && f.b_frames >= 10 && f.alternations >= 4 ? IncidentConfidence::High : IncidentConfidence::Medium;
            o.numerator = double(f.a_frames + f.b_frames);
            o.measures = {{"cluster_a_cfo_ppm", f.a_ppm}, {"cluster_b_cfo_ppm", f.b_ppm}, {"gap_ppm", f.b_ppm - f.a_ppm},
                          {"cluster_a_frames", double(f.a_frames)}, {"cluster_b_frames", double(f.b_frames)},
                          {"cluster_a_spread_ppm", f.a_spread_ppm}, {"cluster_b_spread_ppm", f.b_spread_ppm},
                          {"alternations", double(f.alternations)}};
            o.confidence_basis = {"two carrier offsets " + fmt("%.3f", f.b_ppm - f.a_ppm) + " ppm apart under one transmitter address",
                                  std::to_string(f.alternations) + " alternations: both active over the same time"};
            o.benign_alternatives = {"a repeater or extender in MAC-clone mode",
                                     "two radios of one device sharing a MAC on this channel"};
        }
        if (!f.evidence.empty()) {
            o.evidence = f.evidence.front();
            o.extra_evidence.assign(f.evidence.begin() + 1, f.evidence.end());
        }
        result.observed_incidents.insert(incidents.observe(o));
    }
    return result;
}

void RfIdentityRule::interrupt() {
    pending_.clear();
    pending_frames_ = 0;
    tracker_.clear();
}

}  // namespace rfmon::wifi_security
