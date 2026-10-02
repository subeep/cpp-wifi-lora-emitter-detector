#pragma once
#include "capture_timing.hpp"
#include <nlohmann/json.hpp>
namespace rfmon {
inline nlohmann::json capture_timing_json(const CaptureTiming& t) {
    nlohmann::json gaps = nlohmann::json::array();
    for (const auto& o : t.overflows) gaps.push_back({{"at_sample",o.at_sample},
        {"resume_device_ns",o.resume_time_valid ? nlohmann::json(o.resume_device_ns) : nlohmann::json(nullptr)}});
    return {{"device_time_valid",t.device_time_valid},{"device_time_ns",t.device_time_ns},
        {"host_before_ns",t.host_before_ns},{"host_after_ns",t.host_after_ns},
        {"requested_samples",t.requested_samples},{"overflows",gaps},
        {"timed_out",t.timed_out},{"exception",t.exception},{"retuned",t.retuned},
        {"actual_rf_hz",t.actual_rf_hz},{"actual_dsp_hz",t.actual_dsp_hz},
        {"gain_db",t.gain_db ? nlohmann::json(*t.gain_db) : nlohmann::json(nullptr)}};
}
inline size_t capture_sample_count(const nlohmann::json& j) {
    if (!j.is_number_unsigned() && !(j.is_number_integer() && j.get<int64_t>() >= 0))
        throw std::runtime_error("Invalid unsigned sample count");
    const uint64_t n = j.get<uint64_t>();
    if (n > std::numeric_limits<size_t>::max()) throw std::runtime_error("Sample count overflow");
    return size_t(n);
}
inline CaptureTiming capture_timing_from_json(const nlohmann::json& j, size_t received, double rate = 0) {
    CaptureTiming t;
    t.device_time_valid=j.at("device_time_valid");t.device_time_ns=j.at("device_time_ns");
    t.host_before_ns=j.at("host_before_ns");t.host_after_ns=j.at("host_after_ns");
    t.requested_samples=capture_sample_count(j.at("requested_samples"));
    t.timed_out=j.at("timed_out");t.exception=j.at("exception");t.retuned=j.at("retuned");
    t.actual_rf_hz=j.at("actual_rf_hz");t.actual_dsp_hz=j.at("actual_dsp_hz");
    if (!j.at("gain_db").is_null()) t.gain_db=j.at("gain_db").get<double>();
    const auto& gaps=j.at("overflows");
    if (!gaps.is_array() || gaps.size()>4096) throw std::runtime_error("Invalid overflow list");
    for (const auto& g:gaps) {
        CaptureTiming::Overflow o; o.at_sample=capture_sample_count(g.at("at_sample"));
        if (!g.at("resume_device_ns").is_null()) {o.resume_time_valid=true;o.resume_device_ns=g.at("resume_device_ns");}
        t.overflows.push_back(o);
    }
    validate_capture_timing(t,received,rate);return t;
}
} // namespace rfmon
