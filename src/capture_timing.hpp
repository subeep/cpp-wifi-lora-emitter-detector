#pragma once
// Radio-free capture provenance, shared by live receive and offline tools.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace rfmon {
// Receive timing and continuity for one finite capture. Recorded for the
// Wi-Fi security monitor's coverage accounting
// (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package A step 4); purely
// passive - reading it costs no extra device transactions.
struct CaptureTiming {
    // USRP time of the first received sample, from the first packet's
    // rx_metadata time_spec. Device time counts from the radio's own
    // reset, NOT from any wall clock, but it is sample-exact and monotonic
    // for as long as one UsrpCapture instance lives - which makes it the
    // right domain for comparing receive times across captures.
    bool device_time_valid = false;
    int64_t device_time_ns = 0;
    // Host system_clock (Unix ns) just before the stream command and just
    // after the first samples arrived. The first sample was received
    // somewhere in [host_before_ns, host_after_ns]; the width is the
    // host-time uncertainty. Never a tight clock.
    int64_t host_before_ns = 0, host_after_ns = 0;
    // Sample index at which each overflow was reported (samples before it
    // are contiguous). If a time_spec followed the overflow, the device
    // time of the first sample after the gap is recorded; otherwise the
    // gap length is unknown and precise timing after it is invalid.
    struct Overflow { size_t at_sample = 0; bool resume_time_valid = false; int64_t resume_device_ns = 0; };
    std::vector<Overflow> overflows;
    size_t requested_samples = 0;
    bool timed_out = false;       // recv timed out before all samples arrived
    bool exception = false;       // UHD exception: capture dropped
    double actual_rf_hz = 0.0;    // tune result: RF LO actually set
    double actual_dsp_hz = 0.0;   // tune result: residual DSP shift
    bool retuned = true;          // false: already on this frequency, tune and settle skipped
    std::optional<double> gain_db;  // gain last requested through this object (nullopt: AGC)
};


inline void validate_capture_timing(const CaptureTiming& t, size_t received, double rate = 0) {
    if (t.requested_samples < received || t.overflows.size() > 4096 ||
        t.host_before_ns < 0 || t.host_after_ns < 0 ||
        (t.host_after_ns && t.host_after_ns < t.host_before_ns) ||
        !std::isfinite(t.actual_rf_hz) || !std::isfinite(t.actual_dsp_hz) ||
        (t.gain_db && !std::isfinite(*t.gain_db)))
        throw std::runtime_error("Invalid capture timing metadata");
    size_t previous = 0;
    for (const auto& o : t.overflows) {
        if (o.at_sample > received || o.at_sample < previous)
            throw std::runtime_error("Invalid overflow sample ordering/range");
        previous = o.at_sample;
    }
    if (rate > 0 && std::isfinite(rate)) {
        auto fits = [&](int64_t anchor, size_t span) {
            const long double end = anchor + (long double)span / rate * 1e9L;
            return end >= std::numeric_limits<int64_t>::min() && end <= std::numeric_limits<int64_t>::max();
        };
        if ((t.device_time_valid && !fits(t.device_time_ns, received)) ||
            (t.host_after_ns && !fits(t.host_after_ns, received)))
            throw std::runtime_error("Capture clock range overflow");
        for (const auto& o : t.overflows)
            if (o.resume_time_valid && !fits(o.resume_device_ns, received-o.at_sample))
                throw std::runtime_error("Resume clock range overflow");
    }
}
// The time of the supplied SAMPLE INDEX is exact when the anchor is known.
// A decoder's estimate of a packet's start can still be coarse.
inline std::optional<int64_t> capture_sample_time(const CaptureTiming& t, double rate,
                                                 size_t received, size_t sample) {
    if (!t.device_time_valid || !std::isfinite(rate) || rate <= 0 || sample >= received) return {};
    size_t anchor_sample = 0;
    int64_t anchor_ns = t.device_time_ns;
    bool valid = true;
    for (const auto& o : t.overflows) if (o.at_sample <= sample) {
        anchor_sample = o.at_sample; anchor_ns = o.resume_device_ns; valid = o.resume_time_valid;
    }
    if (!valid) return {};
    const long double time = anchor_ns + std::round((long double)(sample-anchor_sample) / rate * 1e9L);
    if (time < std::numeric_limits<int64_t>::min() || time > std::numeric_limits<int64_t>::max()) return {};
    return int64_t(time);
}
} // namespace rfmon
