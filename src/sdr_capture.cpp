#include "sdr_capture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <uhd/exception.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/tune_request.hpp>

namespace rfmon {

namespace {
// Some daughterboards (e.g. the X310's UBX-160) don't implement AGC at
// all - set_rx_agc() throws uhd::not_implemented_error even to
// explicitly *disable* it before setting a manual gain, not just to
// enable it. Manual gain via set_rx_gain() still works fine on those
// boards, so this is a soft no-op rather than a fatal error.
void try_set_agc(const uhd::usrp::multi_usrp::sptr& usrp, bool enable, size_t channel) {
    try {
        usrp->set_rx_agc(enable, channel);
    } catch (const uhd::not_implemented_error&) {
    }
}
}  // namespace

UsrpCapture::UsrpCapture(const std::string& antenna, std::optional<double> gain_db,
                          size_t channel, const std::string& device_args)
    : channel_(channel), gain_db_(gain_db) {
    usrp_ = uhd::usrp::multi_usrp::make(device_args);
    usrp_->set_rx_antenna(antenna, channel_);

    // Best-effort hardware correction. NOTE: this does not fully remove
    // the LO-leakage spike at the tuned center frequency (confirmed
    // empirically in the Python prototype) - detection still masks a
    // guard band around it (see config.hpp). Not every daughterboard
    // supports IQ balance correction either (UHD logs a warning and
    // continues rather than throwing, unlike AGC, so no try/catch
    // needed here).
    usrp_->set_rx_dc_offset(true, channel_);
    usrp_->set_rx_iq_balance(true, channel_);

    if (gain_db.has_value()) {
        try_set_agc(usrp_, false, channel_);
        usrp_->set_rx_gain(*gain_db, channel_);
    } else {
        try_set_agc(usrp_, true, channel_);
    }
}

void UsrpCapture::set_gain(std::optional<double> gain_db) {
    gain_db_ = gain_db;
    if (gain_db.has_value()) {
        try_set_agc(usrp_, false, channel_);
        usrp_->set_rx_gain(*gain_db, channel_);
    } else {
        try_set_agc(usrp_, true, channel_);
    }
}

void UsrpCapture::ensure_streamer(double sample_rate_hz) {
    if (!streamer_ || current_rate_ != sample_rate_hz) {
        if (streamer_) {
            // On RFNoC devices (X310) the old streamer still holds its
            // block-graph connections (e.g. Radio#0 -> DDC#0) until
            // explicitly released. Just reassigning streamer_ below
            // would call get_rx_stream() for the new rate *while the
            // old one is still alive*, so the graph sees those ports
            // already connected ("Attempting to reconnect output
            // port"). That compounds over repeated rate changes (e.g.
            // switching between Wi-Fi's ~50 Msps and LoRa's 125 kHz)
            // until internal state corrupts badly enough to crash. Not
            // an issue on the B210, which doesn't use the RFNoC graph.
            streamer_->issue_stream_cmd(
                uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
            streamer_.reset();
        }

        usrp_->set_rx_rate(sample_rate_hz, channel_);
        double actual_rate = usrp_->get_rx_rate(channel_);
        usrp_->set_rx_bandwidth(actual_rate, channel_);

        uhd::stream_args_t stream_args("fc32", "sc16");
        stream_args.channels = {channel_};
        streamer_ = usrp_->get_rx_stream(stream_args);
        current_rate_ = actual_rate;
    }
}

namespace {
int64_t host_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t time_spec_ns(const uhd::time_spec_t& t) {
    return int64_t(t.get_full_secs()) * 1000000000LL + int64_t(std::llround(t.get_frac_secs() * 1e9));
}
}  // namespace

CaptureResult UsrpCapture::capture_detailed(double center_hz, double sample_rate_hz, double duration_s,
                                            double settle_s, bool retune_always) {
    CaptureTiming timing;
    timing.gain_db = gain_db_;
    // Ethernet-connected devices (X310) occasionally hit a transient
    // control-channel timeout (uhd::io_error, "Timed out getting recv
    // buff for management transaction") under rapid retuning - this is
    // a known class of flakiness on that transport, not a logic bug.
    // An uncaught exception here would escape Scanner::run()'s thread
    // function and abort the whole process, so treat any UHD-level
    // hiccup as a dropped capture instead - the next scan cycle simply
    // tries again.
    try {
        const double rate_before = current_rate_;
        ensure_streamer(sample_rate_hz);
        if (current_rate_ != rate_before) tuned_center_hz_.reset();  // streamer rebuilt: do not assume the tune
        double actual_rate = current_rate_;

        if (retune_always || !tuned_center_hz_ || *tuned_center_hz_ != center_hz) {
            uhd::tune_result_t tune = usrp_->set_rx_freq(uhd::tune_request_t(center_hz), channel_);
            last_rf_hz_ = tune.actual_rf_freq;
            last_dsp_hz_ = tune.actual_dsp_freq;
            tuned_center_hz_ = center_hz;
            std::this_thread::sleep_for(std::chrono::duration<double>(settle_s));
        } else {
            timing.retuned = false;
        }
        timing.actual_rf_hz = last_rf_hz_;
        timing.actual_dsp_hz = last_dsp_hz_;

        size_t num_samps = static_cast<size_t>(actual_rate * duration_s);
        std::vector<std::complex<float>> buf(num_samps);
        uhd::rx_metadata_t metadata;

        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
        stream_cmd.num_samps = num_samps;
        stream_cmd.stream_now = true;
        timing.requested_samples = num_samps;
        timing.host_before_ns = host_now_ns();
        streamer_->issue_stream_cmd(stream_cmd);

        size_t recvd = 0;
        size_t max_recv = streamer_->get_max_num_samps();
        bool overflow = false;
        bool awaiting_resume_time = false;
        while (recvd < num_samps) {
            size_t remaining = num_samps - recvd;
            size_t request = (max_recv > 0) ? std::min(remaining, max_recv) : remaining;
            size_t n = streamer_->recv(buf.data() + recvd, request, metadata, 1.0);

            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                overflow = true;
                timing.overflows.push_back({recvd, false, 0});
                awaiting_resume_time = true;
            } else if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT && n == 0) {
                timing.timed_out = true;
                break;
            }
            if (n > 0) {
                if (recvd == 0 && !timing.device_time_valid) {
                    timing.host_after_ns = host_now_ns();
                    if (metadata.has_time_spec) {
                        timing.device_time_valid = true;
                        timing.device_time_ns = time_spec_ns(metadata.time_spec);
                    }
                } else if (awaiting_resume_time && metadata.has_time_spec) {
                    timing.overflows.back().resume_time_valid = true;
                    timing.overflows.back().resume_device_ns = time_spec_ns(metadata.time_spec);
                }
                awaiting_resume_time = false;
            }
            recvd += n;
            if (n == 0 && metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE) {
                break;  // defensive: avoid spinning forever on an unexpected zero-length, no-error recv
            }
        }
        buf.resize(recvd);

        streamer_->issue_stream_cmd(
            uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
        if (recvd == 0) timing.host_after_ns = host_now_ns();
        return {std::move(buf), actual_rate, overflow, std::move(timing)};
    } catch (const uhd::exception& e) {
        std::fprintf(stderr, "UsrpCapture::capture: %s (treating as a dropped capture)\n",
                     e.what());
        // Don't trust whatever state the streamer was left in - force a
        // full rebuild via ensure_streamer() on the next call rather
        // than risk reusing a half-broken one.
        streamer_.reset();
        current_rate_ = -1.0;
        tuned_center_hz_.reset();
        timing.exception = true;
        if (!timing.host_before_ns) timing.host_before_ns = host_now_ns();
        timing.host_after_ns = host_now_ns();
        return {std::vector<std::complex<float>>{}, sample_rate_hz, false, std::move(timing)};
    }
}

}  // namespace rfmon
