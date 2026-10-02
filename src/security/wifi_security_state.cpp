#include "security/wifi_security_state.hpp"

#include <algorithm>

namespace rfmon::wifi_security {

using nlohmann::json;

SecurityState::SecurityState(StateLimits limits)
    : limits_(limits), baselines_(limits.baseline), incidents_(limits.incidents), flood_(limits.flood), beacon_replay_(limits.beacon_replay) {
    snap_.beacon_replay_enabled = limits.beacon_replay.enabled;
    snap_.beacon_replay_config = beacon_replay_.config_version();
    snap_.flood_enabled = limits.flood.enabled;
    snap_.flood_config = flood_.config_version();
}

void SecurityState::refresh_baselines() {
    snap_.baselines = baselines_.summaries();
    snap_.baseline_pending_dropped = baselines_.pending_dropped();
}

void SecurityState::refresh_incidents() {
    const auto& all = incidents_.incidents();
    const size_t n = std::min(limits_.published_incidents, all.size());
    snap_.incidents.assign(all.end() - long(n), all.end());
    snap_.incidents_open = incidents_.open_count();
    snap_.incidents_total = all.size();
    snap_.incidents_evicted = incidents_.evicted();
}

void SecurityState::note_timeline(const CaptureRecord& c, ChannelCoverage& cov) {
    if (!c.retuned) ++snap_.timeline.retune_skipped;
    if (c.clock != ClockDomain::UsrpDevice || c.samples_received == 0 || c.sample_rate_hz <= 0) return;
    snap_.timeline.sampled_s += c.observed_seconds();
    auto [it, fresh] = sessions_.try_emplace({c.run_id, c.radio_session});
    SessionClock& clk = it->second;
    if (fresh) {
        ++snap_.timeline.radio_sessions;  // first capture of a session: no gap measurable before it
    } else if (!clk.end_known) {
        ++snap_.timeline.unknown_gaps;
    } else if (c.device_time_ns < clk.end_ns) {
        ++snap_.timeline.out_of_order;
    } else {
        const double gap = double(c.device_time_ns - clk.end_ns) / 1e9;
        ++snap_.timeline.gaps;
        snap_.timeline.dead_s += gap;
        snap_.timeline.max_gap_s = std::max(snap_.timeline.max_gap_s, gap);
        cov.dead_s += gap;
    }
    // The end is only known if no overflow of unknown length intervened.
    const bool unknown_gap_inside = std::any_of(c.overflows.begin(), c.overflows.end(),
                                                [](const OverflowMark& o) { return !o.resume_device_ns; });
    if (unknown_gap_inside) {
        clk.end_known = false;
    } else {
        const OverflowMark* last = c.overflows.empty() ? nullptr : &c.overflows.back();
        const int64_t anchor = last ? *last->resume_device_ns : c.device_time_ns;
        const size_t from = last ? last->at_sample : 0;
        clk.end_ns = anchor + int64_t(double(c.samples_received - from) / c.sample_rate_hz * 1e9);
        clk.end_known = true;
    }
}

ChannelCoverage& SecurityState::coverage_for(const std::string& band, int channel) {
    auto [it, inserted] = snap_.coverage.try_emplace({band, channel});
    if (inserted) { it->second.band = band; it->second.channel = channel; }
    return it->second;
}

void SecurityState::ingest(const CaptureRecord& c) {
    const auto key = std::make_pair(c.run_id, c.capture_seq);
    if (captures_seen_.count(key)) { ++snap_.captures_duplicate; return; }
    captures_seen_.insert(key);
    captures_order_.push_back(key);
    while (captures_order_.size() > limits_.dedupe_captures) {
        captures_seen_.erase(captures_order_.front());
        captures_order_.pop_front();
    }
    ++snap_.captures_ingested;
    ChannelCoverage& cov = coverage_for(c.band, c.channel);
    ++cov.captures;
    if (c.samples_received == 0 || c.exception) ++cov.failed_captures;
    if (!c.overflows.empty()) ++cov.overflow_captures;
    if (c.burst_cap_reached) ++cov.burst_cap_captures;
    if (c.clock == ClockDomain::UsrpDevice) ++cov.device_time_captures;
    else if (c.clock == ClockDomain::HostOnly) ++cov.host_only_captures;
    cov.sampled_s += c.observed_seconds();
    if (c.contiguous()) cov.contiguous_s += c.observed_seconds();
    if (c.processed) cov.analysed_s += c.analysed_seconds();
    cov.processing_s += c.processing_s;
    cov.bursts += c.bursts_detected;
    cov.dsss_attempts += c.dsss_decode_attempts;
    cov.dsss_fcs_valid += c.dsss_fcs_valid;
    cov.dsss_not_attempted += c.dsss_not_attempted;
    cov.ofdm_attempts += c.ofdm_decode_attempts;
    cov.ofdm_fcs_valid += c.ofdm_fcs_valid;
    cov.events_rejected_by_queue += c.events_rejected_by_queue;
    cov.bursts_beyond_identity_limit += c.bursts_beyond_identity_limit;
    note_timeline(c, cov);
    auto outcome = flood_.finish_capture(c, baselines_, incidents_);
    snap_.flood_evaluations = flood_.evaluations();
    snap_.flood_excluded_captures = flood_.excluded();
    auto replay = beacon_replay_.finish_capture(c, incidents_);
    snap_.beacon_replay_evaluated = beacon_replay_.evaluated();
    snap_.beacon_replay_excluded = beacon_replay_.excluded();
    snap_.beacon_replay_forgotten = beacon_replay_.forgotten();
    outcome.observed_incidents.insert(replay.observed_incidents.begin(), replay.observed_incidents.end());
    baselines_.note_capture(c, outcome.usable && replay.observed_incidents.empty());
    refresh_incidents();
    refresh_baselines();
    if (outcome.usable && replay.usable && incidents_.open_count()) {
        incidents_.note_analysed(c.band, c.channel, c.analysed_seconds(), &outcome.observed_incidents, BaselineStore::key_for(c));
        refresh_incidents();
    }
}

void SecurityState::ingest(const ControlCommand& c) {
    if (c.action == "baseline_freeze") baselines_.set_frozen(c.key, true);
    else if (c.action == "baseline_unfreeze") baselines_.set_frozen(c.key, false);
    else if (c.action == "baseline_reset") baselines_.reset(c.key);
    else { ++snap_.input_lines_rejected; return; }
    if (c.key.empty() && (c.action == "baseline_freeze" || c.action == "baseline_unfreeze"))
        snap_.baselines_frozen = c.action == "baseline_freeze";
    ++snap_.commands_applied;
    refresh_baselines();
}

uint64_t SecurityState::observe(const IncidentObservation& o) {
    const uint64_t id = incidents_.observe(o);
    refresh_incidents();
    return id;
}

json SecurityState::export_persistent() const {
    return {{"schema", 1}, {"baselines", baselines_.export_json()}, {"incidents", incidents_.export_json()},
            {"baselines_frozen", snap_.baselines_frozen}};
}

void SecurityState::import_persistent(const json& j) {
    flood_.interrupt();
    beacon_replay_.interrupt();
    if (j.contains("baselines")) snap_.baselines_restored = baselines_.import_json(j.at("baselines"));
    if (j.contains("incidents")) snap_.incidents_restored = incidents_.import_json(j.at("incidents"));
    if (j.value("baselines_frozen", false)) {
        baselines_.set_frozen("", true);
        snap_.baselines_frozen = true;
    }
    refresh_baselines();
    refresh_incidents();
}

bool SecurityState::ingest(const FrameEvent& e) {
    ++snap_.frames_ingested;
    // Idempotence first: the same burst processed twice is one ingestion,
    // whatever its bytes say.
    const auto cap = std::make_pair(e.run_id, e.capture_seq);
    auto [it, inserted] = seen_frames_.try_emplace(cap);
    if (inserted) {
        seen_order_.push_back(cap);
        while (seen_order_.size() > limits_.dedupe_captures) {
            seen_frames_.erase(seen_order_.front());
            seen_order_.pop_front();
            ++snap_.dedupe_forgotten_captures;
        }
        it = seen_frames_.find(cap);
    }
    if (!it->second.insert({e.sample_start, e.phy}).second) {
        ++snap_.frames_duplicate;
        return false;
    }

    // Acceptance: the bytes must carry a valid FCS, re-checked here. A
    // truncated store cannot be checked, so it cannot be accepted either.
    ProcessedFrame pf;
    pf.frame = parse_mac_frame(e.mpdu.data(), e.mpdu.size(), true);
    if (e.mpdu_truncated) ++snap_.frames_truncated_storage;
    if (!e.fcs_valid || e.mpdu_truncated || !pf.frame.fcs_valid()) {
        ++snap_.frames_rejected_fcs;
        return false;
    }
    if (pf.frame.header_state == ParseState::Truncated || pf.frame.header_state == ParseState::Malformed) {
        ++snap_.frames_rejected_malformed;
        return false;
    }

    ++snap_.frames_accepted;
    ++snap_.frames_by_phy[e.phy];
    ++snap_.frames_by_type[frame_type_label(pf.frame)];
    ++snap_.body_states[body_state_name(pf.frame.body_state)];
    if (!e.device_time_ns) ++snap_.frames_without_device_time;
    if (e.after_overflow) ++snap_.frames_after_overflow;
    if (e.security_decode_only) ++snap_.frames_security_only;
    ++coverage_for(e.band, e.channel).frames_accepted;
    baselines_.note_frame(e, pf.frame);
    flood_.note_frame(e, pf.frame);
    beacon_replay_.note_frame(e, pf.frame);

    // Same bytes (Retry bit aside) at a different ingest key: a separate
    // transmission. Kept, and linked to where the bytes were first seen.
    const uint64_t h = pf.frame.content_hash_retry_invariant;
    if (auto r = repeat_first_.find(h); r != repeat_first_.end()) {
        pf.repeated_content = true;
        pf.first_seen_as = r->second;
        ++snap_.frames_repeated_content;
    } else {
        repeat_first_.emplace(h, ingest_key(e));
        repeat_order_.push_back(h);
        while (repeat_order_.size() > limits_.repeat_memory) {
            repeat_first_.erase(repeat_order_.front());
            repeat_order_.pop_front();
        }
    }

    pf.event = e;
    snap_.recent.push_back(std::move(pf));
    while (snap_.recent.size() > limits_.recent_frames) snap_.recent.pop_front();
    return true;
}

void SecurityState::note_input_rejected() {
    ++snap_.input_lines_rejected;
    flood_.interrupt();
    beacon_replay_.interrupt();
    baselines_.invalidate_pending();
}

void SecurityState::ingest(const LossNotice& l) {
    if (l.events_dropped || l.captures_dropped) { flood_.interrupt(); beacon_replay_.interrupt(); baselines_.invalidate_pending(); }
    snap_.queue_events_dropped += l.events_dropped;
    snap_.queue_captures_dropped += l.captures_dropped;
    snap_.losses.push_back(l);
    while (snap_.losses.size() > limits_.loss_notices) snap_.losses.pop_front();
}

json snapshot_json(const SecuritySnapshot& s, bool include_recent) {
    json j = {{"schema", s.schema},
              {"captures_ingested", s.captures_ingested}, {"captures_duplicate", s.captures_duplicate},
              {"frames_ingested", s.frames_ingested}, {"frames_accepted", s.frames_accepted},
              {"frames_duplicate", s.frames_duplicate}, {"frames_rejected_fcs", s.frames_rejected_fcs},
              {"frames_rejected_malformed", s.frames_rejected_malformed},
              {"frames_repeated_content", s.frames_repeated_content},
              {"frames_truncated_storage", s.frames_truncated_storage},
              {"frames_without_device_time", s.frames_without_device_time},
              {"frames_after_overflow", s.frames_after_overflow}, {"frames_security_only", s.frames_security_only},
              {"dedupe_forgotten_captures", s.dedupe_forgotten_captures},
              {"queue_events_dropped", s.queue_events_dropped}, {"queue_captures_dropped", s.queue_captures_dropped},
              {"input_lines_rejected", s.input_lines_rejected}, {"frames_by_type", s.frames_by_type},
              {"frames_by_phy", s.frames_by_phy}, {"body_states", s.body_states},
              {"storage_error", s.storage_error}, {"recorded_bytes", s.recorded_bytes}};
    json cov = json::array();
    for (const auto& [k, c] : s.coverage) {
        cov.push_back({{"band", c.band}, {"channel", c.channel}, {"captures", c.captures},
                       {"failed_captures", c.failed_captures}, {"overflow_captures", c.overflow_captures},
                       {"burst_cap_captures", c.burst_cap_captures}, {"device_time_captures", c.device_time_captures},
                       {"host_only_captures", c.host_only_captures}, {"sampled_s", c.sampled_s},
                       {"contiguous_s", c.contiguous_s}, {"analysed_s", c.analysed_s},
                       {"processing_s", c.processing_s}, {"dead_s", c.dead_s}, {"bursts", c.bursts},
                       {"dsss_attempts", c.dsss_attempts}, {"dsss_fcs_valid", c.dsss_fcs_valid},
                       {"dsss_not_attempted", c.dsss_not_attempted}, {"ofdm_attempts", c.ofdm_attempts},
                       {"ofdm_fcs_valid", c.ofdm_fcs_valid}, {"frames_accepted", c.frames_accepted},
                       {"events_rejected_by_queue", c.events_rejected_by_queue},
                       {"bursts_beyond_identity_limit", c.bursts_beyond_identity_limit}});
    }
    j["coverage"] = cov;
    const auto& t = s.timeline;
    j["timeline"] = {{"radio_sessions", t.radio_sessions}, {"gaps", t.gaps}, {"unknown_gaps", t.unknown_gaps},
                     {"out_of_order", t.out_of_order}, {"retune_skipped", t.retune_skipped},
                     {"sampled_s", t.sampled_s}, {"dead_s", t.dead_s}, {"max_gap_s", t.max_gap_s},
                     {"duty", t.duty()}};
    json bl = json::array();
    for (const auto& b : s.baselines) {
        json rates = json::object();
        for (size_t i = 0; i < kBaselineMetrics; ++i) {
            const auto& r = b.rates[i];
            rates[baseline_metric_name(BaselineMetric(i))] = {{"mean", r.mean}, {"p50", r.p50}, {"p95", r.p95}, {"max", r.max}};
        }
        bl.push_back({{"key", b.key}, {"band", b.band}, {"channel", b.channel}, {"version", b.version},
                      {"frozen", b.frozen}, {"holders", b.holders}, {"windows_held", b.windows_held}, {"restored", b.restored}, {"windows_closed", b.windows_closed},
                      {"windows_excluded", b.windows_excluded}, {"windows_included", b.windows_included},
                      {"captures_excluded", b.captures_excluded}, {"learned_analysed_s", b.learned_analysed_s},
                      {"current_analysed_s", b.current_analysed_s}, {"rates", rates}});
    }
    j["baselines"] = bl;
    j["flood"] = {{"enabled",s.flood_enabled},{"evaluations",s.flood_evaluations},{"excluded_captures",s.flood_excluded_captures},{"config",s.flood_config}};
    j["beacon_replay"] = {{"enabled",s.beacon_replay_enabled},{"evaluated",s.beacon_replay_evaluated},{"excluded_captures",s.beacon_replay_excluded},{"forgotten",s.beacon_replay_forgotten},{"config",s.beacon_replay_config}};
    json incidents = json::array();
    for (const auto& incident : s.incidents) incidents.push_back(incident_json(incident));
    j["incidents"] = incidents;
    j["baselines_frozen"] = s.baselines_frozen;
    j["baseline_pending_dropped"] = s.baseline_pending_dropped;
    j["commands_applied"] = s.commands_applied;
    j["incidents_open"] = s.incidents_open;
    j["incidents_total"] = s.incidents_total;
    j["incidents_evicted"] = s.incidents_evicted;
    j["baselines_restored"] = s.baselines_restored;
    j["incidents_restored"] = s.incidents_restored;
    j["persist_error"] = s.persist_error;
    json losses = json::array();
    for (const auto& l : s.losses) losses.push_back(to_json(l));
    j["losses"] = losses;
    if (include_recent) {
        json rec = json::array();
        for (const auto& p : s.recent) {
            json f = to_json(p.event);
            f["summary"] = describe(p.frame);
            f["repeated_content"] = p.repeated_content;
            if (p.first_seen_as) {
                const auto& [run, seq, sample, phy] = *p.first_seen_as;
                f["first_seen_as"] = {{"run_id", run}, {"capture_seq", seq}, {"sample_start", sample}, {"phy", phy}};
            }
            rec.push_back(f);
        }
        j["recent"] = rec;
    }
    return j;
}

}  // namespace rfmon::wifi_security
