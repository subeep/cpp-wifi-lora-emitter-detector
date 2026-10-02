#pragma once
#include "wifi_iq_capture.hpp"
#include "security/wifi_frame_event.hpp"
#include "config.hpp"
namespace rfmon::wifi_security {
inline CaptureRecord wifi_iq_record(const WifiIqCapture& input, const std::string& filename, uint64_t legacy_seq) {
    const auto& j=input.metadata;CaptureRecord r;
    const double hz=j.at("channel_hz");
    r.run_id=j.value("run_id","offline:"+j.value("fnv1a64",filename));
    r.capture_seq=input.timing ? j.at("capture_seq").get<uint64_t>() : legacy_seq;
    r.radio_session=j.value("radio_session",uint64_t(0));r.source="offline:"+filename;
    r.band=hz<3e9 ? BAND_WIFI_2G4:BAND_WIFI_5G;
    r.channel=int(std::lround(hz==2484e6 ? 14 : hz<3e9 ? (hz/1e6-2407)/5 : (hz/1e6-5000)/5));
    r.channel_hz=hz;r.capture_center_hz=j.at("capture_center_hz");r.sample_rate_hz=j.at("sample_rate_hz");
    r.requested_rate_hz=j.value("requested_rate_hz",r.sample_rate_hz);r.requested_duration_s=j.value("requested_duration_s",0.0);
    if(j.contains("requested_gain_db")&&!j.at("requested_gain_db").is_null())r.gain_db=j.at("requested_gain_db").get<double>();
    r.antenna=j.value("antenna","");r.device=j.value("device_args","");r.samples_received=input.iq.size();r.samples_requested=r.samples_received;
    if(input.timing) {
        const auto& t=*input.timing;r.samples_requested=t.requested_samples;
        r.clock=t.device_time_valid ? ClockDomain::UsrpDevice : t.host_before_ns>0 ? ClockDomain::HostOnly:ClockDomain::Unknown;
        r.device_time_ns=t.device_time_ns;r.host_before_ns=t.host_before_ns;r.host_after_ns=t.host_after_ns;
        r.actual_rf_hz=t.actual_rf_hz;r.actual_dsp_hz=t.actual_dsp_hz;r.gain_db=t.gain_db;
        r.timed_out=t.timed_out;r.exception=t.exception;r.retuned=t.retuned;
        for(const auto& o:t.overflows)r.overflows.push_back({o.at_sample,o.resume_time_valid ? std::optional<int64_t>(o.resume_device_ns):std::nullopt});
    } else {
        // Legacy host timestamp precedes tuning; it is not a first-sample bracket.
        r.host_before_ns=j.value("host_start_ns",int64_t(0));r.clock=ClockDomain::Unknown;
        if(j.value("overflow",false))r.overflows.push_back({0,std::nullopt});
    }
    return r;
}
} // namespace rfmon::wifi_security
