#include "sdr_capture.hpp"

#include <algorithm>
#include <chrono>
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
    : channel_(channel) {
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

std::tuple<std::vector<std::complex<float>>, double, bool> UsrpCapture::capture(
    double center_hz, double sample_rate_hz, double duration_s, double settle_s) {
    // Ethernet-connected devices (X310) occasionally hit a transient
    // control-channel timeout (uhd::io_error, "Timed out getting recv
    // buff for management transaction") under rapid retuning - this is
    // a known class of flakiness on that transport, not a logic bug.
    // An uncaught exception here would escape Scanner::run()'s thread
    // function and abort the whole process, so treat any UHD-level
    // hiccup as a dropped capture instead - the next scan cycle simply
    // tries again.
    try {
        ensure_streamer(sample_rate_hz);
        double actual_rate = current_rate_;

        usrp_->set_rx_freq(uhd::tune_request_t(center_hz), channel_);
        std::this_thread::sleep_for(std::chrono::duration<double>(settle_s));

        size_t num_samps = static_cast<size_t>(actual_rate * duration_s);
        std::vector<std::complex<float>> buf(num_samps);
        uhd::rx_metadata_t metadata;

        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
        stream_cmd.num_samps = num_samps;
        stream_cmd.stream_now = true;
        streamer_->issue_stream_cmd(stream_cmd);

        size_t recvd = 0;
        size_t max_recv = streamer_->get_max_num_samps();
        bool overflow = false;
        while (recvd < num_samps) {
            size_t remaining = num_samps - recvd;
            size_t request = (max_recv > 0) ? std::min(remaining, max_recv) : remaining;
            size_t n = streamer_->recv(buf.data() + recvd, request, metadata, 1.0);

            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                overflow = true;
            } else if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT && n == 0) {
                break;
            }
            recvd += n;
            if (n == 0 && metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE) {
                break;  // defensive: avoid spinning forever on an unexpected zero-length, no-error recv
            }
        }
        buf.resize(recvd);

        streamer_->issue_stream_cmd(
            uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
        return {std::move(buf), actual_rate, overflow};
    } catch (const uhd::exception& e) {
        std::fprintf(stderr, "UsrpCapture::capture: %s (treating as a dropped capture)\n",
                     e.what());
        // Don't trust whatever state the streamer was left in - force a
        // full rebuild via ensure_streamer() on the next call rather
        // than risk reusing a half-broken one.
        streamer_.reset();
        current_rate_ = -1.0;
        return {std::vector<std::complex<float>>{}, sample_rate_hz, false};
    }
}

}  // namespace rfmon
