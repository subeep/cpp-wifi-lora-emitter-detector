#include "security/wifi_security_io.hpp"

namespace rfmon::wifi_security {

using nlohmann::json;

std::string to_hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string h;
    h.reserve(b.size() * 2);
    for (uint8_t c : b) { h += d[c >> 4]; h += d[c & 15]; }
    return h;
}

bool from_hex(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return false; }
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

namespace {
template <class T> void put(json& j, const char* k, const std::optional<T>& v) {
    if (v) j[k] = *v;
}
template <class T> void get(const json& j, const char* k, std::optional<T>& v) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) v = it->get<T>();
}
template <class T> void get(const json& j, const char* k, T& v) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) v = it->get<T>();
}
}  // namespace

json header_json(const std::string& run_id, const std::string& writer) {
    return {{"kind", "header"}, {"schema", kEventSchema}, {"run_id", run_id}, {"writer", writer}};
}

json to_json(const CaptureRecord& c) {
    json o = {{"kind", "capture"}, {"schema", c.schema}, {"run_id", c.run_id}, {"capture_seq", c.capture_seq},
              {"radio_session", c.radio_session}, {"source", c.source}, {"band", c.band}, {"channel", c.channel},
              {"channel_hz", c.channel_hz}, {"capture_center_hz", c.capture_center_hz},
              {"actual_rf_hz", c.actual_rf_hz}, {"actual_dsp_hz", c.actual_dsp_hz},
              {"requested_rate_hz", c.requested_rate_hz}, {"sample_rate_hz", c.sample_rate_hz},
              {"requested_duration_s", c.requested_duration_s}, {"antenna", c.antenna}, {"device", c.device},
              {"samples_requested", c.samples_requested}, {"samples_received", c.samples_received},
              {"clock", clock_domain_name(c.clock)}, {"device_time_ns", c.device_time_ns},
              {"host_before_ns", c.host_before_ns}, {"host_after_ns", c.host_after_ns},
              {"timed_out", c.timed_out}, {"exception", c.exception}, {"retuned", c.retuned},
              {"processed", c.processed}, {"bursts_beyond_identity_limit", c.bursts_beyond_identity_limit},
              {"burst_cap", c.burst_cap}, {"bursts_detected", c.bursts_detected},
              {"burst_cap_reached", c.burst_cap_reached}, {"analysed_samples", c.analysed_samples},
              {"bursts_unknown", c.bursts_unknown},
              {"bursts_narrowband", c.bursts_narrowband}, {"bursts_dsss", c.bursts_dsss},
              {"bursts_ofdm", c.bursts_ofdm}, {"dsss_decode_attempts", c.dsss_decode_attempts},
              {"dsss_security_only_attempts", c.dsss_security_only_attempts},
              {"dsss_not_attempted", c.dsss_not_attempted}, {"dsss_fcs_valid", c.dsss_fcs_valid},
              {"ofdm_decode_attempts", c.ofdm_decode_attempts}, {"ofdm_fcs_valid", c.ofdm_fcs_valid},
              {"events_submitted", c.events_submitted}, {"events_rejected_by_queue", c.events_rejected_by_queue},
              {"processing_s", c.processing_s}};
    put(o, "gain_db", c.gain_db);
    json ov = json::array();
    for (const auto& m : c.overflows) {
        json x = {{"at_sample", m.at_sample}};
        put(x, "resume_device_ns", m.resume_device_ns);
        ov.push_back(x);
    }
    o["overflows"] = ov;
    return o;
}

json to_json(const FrameEvent& e) {
    json o = {{"kind", "frame"}, {"schema", e.schema}, {"run_id", e.run_id}, {"capture_seq", e.capture_seq},
              {"radio_session", e.radio_session}, {"sample_start", e.sample_start},
              {"sample_length", e.sample_length}, {"band", e.band}, {"channel", e.channel},
              {"channel_hz", e.channel_hz}, {"capture_center_hz", e.capture_center_hz},
              {"sample_rate_hz", e.sample_rate_hz}, {"phy", e.phy}, {"rate_mbps", e.rate_mbps},
              {"security_decode_only", e.security_decode_only}, {"fcs_valid", e.fcs_valid},
              {"mpdu", to_hex(e.mpdu)}, {"mpdu_truncated", e.mpdu_truncated},
              {"clock", clock_domain_name(e.clock)}, {"host_uncertainty_ns", e.host_uncertainty_ns},
              {"after_overflow", e.after_overflow}, {"power_db", e.power_db}, {"bandwidth_hz", e.bandwidth_hz},
              {"duration_us", e.duration_us}, {"confidence", e.confidence}};
    put(o, "device_time_ns", e.device_time_ns);
    put(o, "host_time_ns", e.host_time_ns);
    put(o, "fp_cfo_ppm", e.fp_cfo_ppm); put(o, "fp_irr_db", e.fp_irr_db); put(o, "fp_iq_eps", e.fp_iq_eps);
    put(o, "fp_iq_phi_deg", e.fp_iq_phi_deg); put(o, "fp_dc_dbc", e.fp_dc_dbc); put(o, "fp_snr_db", e.fp_snr_db);
    put(o, "fp_evm_pct", e.fp_evm_pct); put(o, "fp_sync_corr", e.fp_sync_corr);
    put(o, "fp_gate_reason", e.fp_gate_reason);
    return o;
}

json to_json(const LossNotice& l) {
    return {{"kind", "loss"}, {"events_dropped", l.events_dropped}, {"captures_dropped", l.captures_dropped},
            {"first_host_ns", l.first_host_ns}, {"last_host_ns", l.last_host_ns}};
}

CaptureRecord capture_from_json(const json& j) {
    CaptureRecord c;
    c.run_id = j.at("run_id").get<std::string>();
    c.capture_seq = j.at("capture_seq").get<uint64_t>();
    get(j, "schema", c.schema); get(j, "radio_session", c.radio_session); get(j, "source", c.source);
    get(j, "band", c.band); get(j, "channel", c.channel); get(j, "channel_hz", c.channel_hz);
    get(j, "capture_center_hz", c.capture_center_hz); get(j, "actual_rf_hz", c.actual_rf_hz);
    get(j, "actual_dsp_hz", c.actual_dsp_hz); get(j, "requested_rate_hz", c.requested_rate_hz);
    get(j, "sample_rate_hz", c.sample_rate_hz); get(j, "requested_duration_s", c.requested_duration_s);
    get(j, "gain_db", c.gain_db); get(j, "antenna", c.antenna); get(j, "device", c.device);
    get(j, "samples_requested", c.samples_requested); get(j, "samples_received", c.samples_received);
    std::string clock; get(j, "clock", clock); c.clock = clock_domain_from_name(clock);
    get(j, "device_time_ns", c.device_time_ns); get(j, "host_before_ns", c.host_before_ns);
    get(j, "host_after_ns", c.host_after_ns); get(j, "timed_out", c.timed_out); get(j, "exception", c.exception);
    get(j, "retuned", c.retuned); get(j, "bursts_beyond_identity_limit", c.bursts_beyond_identity_limit);
    get(j, "processed", c.processed); get(j, "burst_cap", c.burst_cap); get(j, "bursts_detected", c.bursts_detected);
    get(j, "burst_cap_reached", c.burst_cap_reached); get(j, "analysed_samples", c.analysed_samples);
    get(j, "bursts_unknown", c.bursts_unknown);
    get(j, "bursts_narrowband", c.bursts_narrowband); get(j, "bursts_dsss", c.bursts_dsss);
    get(j, "bursts_ofdm", c.bursts_ofdm); get(j, "dsss_decode_attempts", c.dsss_decode_attempts);
    get(j, "dsss_security_only_attempts", c.dsss_security_only_attempts);
    get(j, "dsss_not_attempted", c.dsss_not_attempted); get(j, "dsss_fcs_valid", c.dsss_fcs_valid);
    get(j, "ofdm_decode_attempts", c.ofdm_decode_attempts); get(j, "ofdm_fcs_valid", c.ofdm_fcs_valid);
    get(j, "events_submitted", c.events_submitted); get(j, "events_rejected_by_queue", c.events_rejected_by_queue);
    get(j, "processing_s", c.processing_s);
    if (auto it = j.find("overflows"); it != j.end() && it->is_array()) {
        for (const auto& x : *it) {
            OverflowMark m;
            m.at_sample = x.at("at_sample").get<size_t>();
            get(x, "resume_device_ns", m.resume_device_ns);
            c.overflows.push_back(m);
        }
    }
    return c;
}

FrameEvent frame_from_json(const json& j) {
    FrameEvent e;
    e.run_id = j.at("run_id").get<std::string>();
    e.capture_seq = j.at("capture_seq").get<uint64_t>();
    e.sample_start = j.at("sample_start").get<size_t>();
    e.phy = j.at("phy").get<std::string>();
    if (!from_hex(j.at("mpdu").get<std::string>(), e.mpdu)) throw std::runtime_error("frame: MPDU is not hex");
    get(j, "schema", e.schema); get(j, "radio_session", e.radio_session); get(j, "sample_length", e.sample_length);
    get(j, "band", e.band); get(j, "channel", e.channel); get(j, "channel_hz", e.channel_hz);
    get(j, "capture_center_hz", e.capture_center_hz); get(j, "sample_rate_hz", e.sample_rate_hz);
    get(j, "rate_mbps", e.rate_mbps); get(j, "security_decode_only", e.security_decode_only);
    get(j, "fcs_valid", e.fcs_valid); get(j, "mpdu_truncated", e.mpdu_truncated);
    std::string clock; get(j, "clock", clock); e.clock = clock_domain_from_name(clock);
    get(j, "device_time_ns", e.device_time_ns); get(j, "host_time_ns", e.host_time_ns);
    get(j, "host_uncertainty_ns", e.host_uncertainty_ns); get(j, "after_overflow", e.after_overflow);
    get(j, "power_db", e.power_db); get(j, "bandwidth_hz", e.bandwidth_hz); get(j, "duration_us", e.duration_us);
    get(j, "confidence", e.confidence);
    get(j, "fp_cfo_ppm", e.fp_cfo_ppm); get(j, "fp_irr_db", e.fp_irr_db); get(j, "fp_iq_eps", e.fp_iq_eps);
    get(j, "fp_iq_phi_deg", e.fp_iq_phi_deg); get(j, "fp_dc_dbc", e.fp_dc_dbc); get(j, "fp_snr_db", e.fp_snr_db);
    get(j, "fp_evm_pct", e.fp_evm_pct); get(j, "fp_sync_corr", e.fp_sync_corr);
    get(j, "fp_gate_reason", e.fp_gate_reason);
    return e;
}

json to_json(const ControlCommand& c) {
    return {{"kind", "command"}, {"action", c.action}, {"key", c.key}, {"host_ns", c.host_ns}};
}

ControlCommand command_from_json(const json& j) {
    ControlCommand c;
    c.action = j.at("action").get<std::string>();
    get(j, "key", c.key);
    get(j, "host_ns", c.host_ns);
    return c;
}

LossNotice loss_from_json(const json& j) {
    LossNotice l;
    get(j, "events_dropped", l.events_dropped); get(j, "captures_dropped", l.captures_dropped);
    get(j, "first_host_ns", l.first_host_ns); get(j, "last_host_ns", l.last_host_ns);
    return l;
}

}  // namespace rfmon::wifi_security
