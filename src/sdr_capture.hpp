// Thin wrapper around the UHD multi_usrp API for finite-duration IQ
// capture. Device-agnostic by construction (just device_args/antenna/
// gain/channel passed in) - used for both the USRP B210 (USB3) and the
// USRP X310 (Ethernet, RFNoC), see config.hpp's DeviceProfile/
// device_profile() for the per-device args each one is constructed
// with.
//
// Uses host format fc32 (complex64) over wire format sc16 - the wire
// format halves the bytes/sample vs raw fc32, which is what keeps
// 56 Msps comfortably inside USB3 bandwidth on the B210 (confirmed on
// this hardware in the Python prototype: zero overflow at 56 Msps over
// a 1s sustained capture). The X310's 1GigE link is the newer, lower-
// bandwidth transport of the two - see PROJECT_STATUS.md for whether
// 56 Msps has been validated not to overflow there too.

#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"

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

struct CaptureResult {
    std::vector<std::complex<float>> samples;
    double sample_rate_hz = 0.0;
    bool overflow = false;
    CaptureTiming timing;
};

class UsrpCapture {
public:
    UsrpCapture(const std::string& antenna = ANTENNA,
                std::optional<double> gain_db = DEFAULT_GAIN_DB, size_t channel = 0,
                const std::string& device_args = DEVICE_ARGS);

    // Tune to center_hz and return duration_s worth of IQ samples.
    // Returns (samples, actual_sample_rate_hz, overflow_flag).
    std::tuple<std::vector<std::complex<float>>, double, bool> capture(
        double center_hz, double sample_rate_hz, double duration_s,
        double settle_s = RETUNE_SETTLE_S);

    // capture() plus receive timing/continuity metadata. capture() is a
    // thin wrapper over this, so both run exactly the same stream logic.
    // With `retune_always` false, a capture on the frequency this object last
    // tuned skips set_rx_freq() and the settle delay - the LO has not moved,
    // so there is nothing to settle. Default true keeps the historical
    // always-retune behaviour (LoRa and capture() rely on it).
    CaptureResult capture_detailed(double center_hz, double sample_rate_hz, double duration_s,
                                   double settle_s = RETUNE_SETTLE_S, bool retune_always = true);

    // Change gain live (nullopt = switch to AGC). Safe to call between captures.
    void set_gain(std::optional<double> gain_db);

private:
    void ensure_streamer(double sample_rate_hz);

    uhd::usrp::multi_usrp::sptr usrp_;
    uhd::rx_streamer::sptr streamer_;
    size_t channel_;
    double current_rate_ = -1.0;
    std::optional<double> gain_db_;
    std::optional<double> tuned_center_hz_;  // last successful tune; cleared on streamer rebuild/error
    double last_rf_hz_ = 0.0, last_dsp_hz_ = 0.0;
};

}  // namespace rfmon
