#include "security/wifi_security_beacon_replay.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::wifi_security {
BeaconReplayRule::BeaconReplayRule(BeaconReplayConfig c) : cfg_(c) {
    for (double x : {c.min_delay_s,c.retention_s,c.max_gap_s})
        if (!std::isfinite(x) || x <= 0) throw std::invalid_argument("Invalid beacon replay time limit");
    if (c.retention_s < c.min_delay_s || !c.max_pending || !c.max_frames_per_capture ||
        !c.max_profiles || !c.max_identities || !c.history_per_identity || c.max_frame_bytes < 40 ||
        c.max_frame_bytes > kMaxStoredMpduBytes)
        throw std::invalid_argument("Invalid beacon replay capacity");
    config_version_ = nlohmann::json{{"v",1},{"enabled",c.enabled},{"delay_s",c.min_delay_s},
        {"retention_s",c.retention_s},{"gap_s",c.max_gap_s},{"pending",c.max_pending},
        {"frames",c.max_frames_per_capture},{"profiles",c.max_profiles},
        {"identities",c.max_identities},{"history",c.history_per_identity},{"bytes",c.max_frame_bytes}}.dump();
}
void BeaconReplayRule::interrupt() { pending_.clear(); profiles_.clear(); lost_ = true; }
void BeaconReplayRule::note_frame(const FrameEvent& e, const MacFrame& f) {
    if (!cfg_.enabled) return;
    auto key = std::make_pair(e.run_id,e.capture_seq);
    if (!pending_.count(key) && pending_.size() >= cfg_.max_pending) interrupt();
    auto& p = pending_[key]; ++p.received;
    if (!f.is_management(mgmt::Beacon)) return;
    if (!f.fcs_valid() || f.header_state != ParseState::Ok ||
        (f.body_state != BodyState::Readable && f.body_state != BodyState::IntegrityProtected) ||
        !f.management || !f.management->timestamp_tsf || !f.transmitter || !f.bssid || !f.receiver ||
        f.fc.more_fragments || f.fragment_number.value_or(0) != 0) return;
    if (e.clock != ClockDomain::UsrpDevice || !e.device_time_ns || e.after_overflow ||
        e.mpdu.size() > cfg_.max_frame_bytes || e.mpdu.size() < 40 ||
        p.frames.size() >= cfg_.max_frames_per_capture) { p.degraded = true; return; }
    Beacon b;
    b.bssid = format_mac(*f.bssid); b.transmitter = format_mac(*f.transmitter);
    b.receiver = format_mac(*f.receiver);
    b.identity = b.bssid + "/" + b.transmitter + "/" + e.phy + "/" + std::to_string(e.rate_mbps);
    b.tsf = *f.management->timestamp_tsf; b.time = *e.device_time_ns;
    b.bytes.assign(e.mpdu.begin(),e.mpdu.end()-4); b.bytes[1] &= ~uint8_t(8);
    b.evidence.frame = ingest_key(e); b.evidence.host_ns = e.host_time_ns.value_or(0);
    b.evidence.summary = describe(f);
    b.evidence.note = "device_ns=" + std::to_string(b.time) + " TSF_us=" + std::to_string(b.tsf);
    b.evidence.mpdu.assign(e.mpdu.begin(),e.mpdu.begin()+std::min(e.mpdu.size(),size_t(512)));
    p.frames.push_back(std::move(b));
}
BeaconReplayResult BeaconReplayRule::finish_capture(const CaptureRecord& c, IncidentStore& incidents) {
    BeaconReplayResult result; result.usable = c.security_usable();
    if (!cfg_.enabled) return result;
    Pending p;
    auto it = pending_.find({c.run_id,c.capture_seq});
    if (it != pending_.end()) { p = std::move(it->second); pending_.erase(it); }
    const auto key = BaselineStore::key_for(c);
    result.usable = result.usable && !lost_ && !p.degraded && c.clock == ClockDomain::UsrpDevice &&
        p.received == c.events_submitted;
    lost_ = false;
    // Validate the entire capture before modifying history or emitting incidents.
    const long double end = (long double)c.device_time_ns + c.observed_seconds()*1e9L;
    int64_t previous = c.device_time_ns;
    for (const auto& b : p.frames) {
        if (b.time < previous || (long double)b.time >= end) result.usable = false;
        previous = b.time;
    }
    if (!result.usable) { ++excluded_; profiles_.erase(key); return result; }
    if (!profiles_.count(key) && profiles_.size() >= cfg_.max_profiles) {
        profiles_.erase(profiles_.begin()); ++forgotten_;
    }
    auto& profile = profiles_[key];
    bool changed = profile.run != c.run_id || profile.session != c.radio_session;
    if (profile.initialized && !changed && c.device_time_ns < profile.end) {
        ++excluded_; result.usable = false; profile = Profile{}; return result;
    }
    if (changed || (profile.initialized && ((long double)c.device_time_ns-profile.end)/1e9L > cfg_.max_gap_s))
        profile = Profile{};
    profile.initialized = true; profile.run = c.run_id; profile.session = c.radio_session;
    // Valid hardware captures are far inside int64 range; guard malformed offline input.
    if (end > INT64_MAX || end < INT64_MIN) {
        ++excluded_; result.usable = false; profile = Profile{}; return result;
    }
    profile.end = int64_t(end);
    for (auto& b : p.frames) {
        ++evaluated_;
        if (!profile.identities.count(b.identity) && profile.identities.size() >= cfg_.max_identities) {
            profile.identities.erase(profile.identities.begin()); ++forgotten_;
        }
        auto& h = profile.identities[b.identity];
        while (!h.frames.empty() && ((long double)b.time-h.frames.front().time)/1e9L > cfg_.retention_s) {
            h.frames.pop_front(); ++forgotten_;
        }
        if (h.initialized && ((long double)b.time-h.newest.time)/1e9L > cfg_.retention_s) h = History{};
        auto old = std::find_if(h.frames.begin(),h.frames.end(),[&](const Beacon& a) { return a.bytes == b.bytes; });
        const bool match = old != h.frames.end();
        const double age = match ? double(((long double)b.time-old->time)/1e9L) : 0;
        if (match && h.initialized && h.newest.tsf > b.tsf && h.newest.time > old->time &&
            b.time > h.newest.time && age >= cfg_.min_delay_s) {
            IncidentObservation o;
            o.rule = "historical_beacon_replay"; o.rule_version = "1"; o.config_version = config_version_;
            o.severity = IncidentSeverity::Medium; o.confidence = IncidentConfidence::Medium;
            o.band = c.band; o.channel = c.channel; o.claimed_bssid = b.bssid;
            o.claimed_source = b.transmitter; o.target = b.receiver; o.baseline_key = key;
            o.host_ns = b.evidence.host_ns; o.device_ns = b.time;
            o.mode = "observation"; o.tier = "suspected"; o.path = "historical_beacon_after_newer";
            o.context = {{"original_tsf_us",std::to_string(old->tsf)},
                {"newer_tsf_us",std::to_string(h.newest.tsf)},{"repeated_tsf_us",std::to_string(b.tsf)},
                {"run_id",c.run_id},{"radio_session",std::to_string(c.radio_session)}};
            o.measures = {{"replay_age_s",age},{"minimum_age_s",cfg_.min_delay_s}};
            o.confidence_basis = {"Full MPDU equality excluding FCS and Retry bit", "Newer TSF observed between matching beacons", "Ordered device timestamps in one receiver context"};
            o.coverage_note = "Historical beacon repetition, not authenticated replay or proof of client impact. No inference across long gaps; bounded history.";
            o.benign_alternatives = {"AP firmware retransmitting stale beacons", "Unobserved AP restart with repeated state", "External capture duplication not identified by ingest keys"};
            o.evidence = old->evidence; o.evidence->role = "original_beacon";
            auto newer = h.newest.evidence; newer.role = "intervening_newer_beacon";
            auto repeated = b.evidence; repeated.role = "repeated_beacon";
            o.extra_evidence = {newer,repeated};
            result.observed_incidents.insert(incidents.observe(o));
        }
        // Unknown TSF regression: restart/anomaly is unresolved. Reacquire;
        // never compare a new AP epoch against the old timeline.
        if (h.initialized && b.tsf < h.newest.tsf && !match) h = History{};
        if (!h.initialized || b.tsf > h.newest.tsf) { h.newest = b; h.initialized = true; }
        if (!match) {
            h.frames.push_back(std::move(b));
            while (h.frames.size() > cfg_.history_per_identity) { h.frames.pop_front(); ++forgotten_; }
        }
    }
    return result;
}
}
