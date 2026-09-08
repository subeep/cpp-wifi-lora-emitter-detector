#include "sdr_capture.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <uhd/types/metadata.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/tune_request.hpp>

namespace rfmon {

B210Capture::B210Capture(const std::string& antenna, std::optional<double> gain_db,
                          size_t channel, const std::string& device_args)
    : channel_(channel) {
    usrp_ = uhd::usrp::multi_usrp::make(device_args);
    usrp_->set_rx_antenna(antenna, channel_);

    // Best-effort hardware correction. NOTE: this does not fully remove
    // the LO-leakage spike at the tuned center frequency (confirmed
    // empirically in the Python prototype) - detection still masks a
    // guard band around it (see config.hpp).
    usrp_->set_rx_dc_offset(true, channel_);
    usrp_->set_rx_iq_balance(true, channel_);

    if (gain_db.has_value()) {
        usrp_->set_rx_agc(false, channel_);
        usrp_->set_rx_gain(*gain_db, channel_);
    } else {
        usrp_->set_rx_agc(true, channel_);
    }
}

void B210Capture::set_gain(std::optional<double> gain_db) {
    if (gain_db.has_value()) {
        usrp_->set_rx_agc(false, channel_);
        usrp_->set_rx_gain(*gain_db, channel_);
    } else {
        usrp_->set_rx_agc(true, channel_);
    }
}

void B210Capture::ensure_streamer(double sample_rate_hz) {
    if (!streamer_ || current_rate_ != sample_rate_hz) {
        usrp_->set_rx_rate(sample_rate_hz, channel_);
        double actual_rate = usrp_->get_rx_rate(channel_);
        usrp_->set_rx_bandwidth(actual_rate, channel_);

        uhd::stream_args_t stream_args("fc32", "sc16");
        stream_args.channels = {channel_};
        streamer_ = usrp_->get_rx_stream(stream_args);
        current_rate_ = actual_rate;
    }
}

std::tuple<std::vector<std::complex<float>>, double, bool> B210Capture::capture(
    double center_hz, double sample_rate_hz, double duration_s, double settle_s) {
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
}

}  // namespace rfmon
