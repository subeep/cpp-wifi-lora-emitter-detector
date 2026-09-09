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
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"

namespace rfmon {

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

    // Change gain live (nullopt = switch to AGC). Safe to call between captures.
    void set_gain(std::optional<double> gain_db);

private:
    void ensure_streamer(double sample_rate_hz);

    uhd::usrp::multi_usrp::sptr usrp_;
    uhd::rx_streamer::sptr streamer_;
    size_t channel_;
    double current_rate_ = -1.0;
};

}  // namespace rfmon
