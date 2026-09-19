#include "bench_capture.hpp"

#include <chrono>
#include <ctime>

#include "config.hpp"
#include "wifi_dsss_rx.hpp"
#include "wifi_phy.hpp"

namespace rfmon::bench {

namespace {
std::string current_time_hhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return std::string(buf);
}
}  // namespace

BenchCapture::BenchCapture() { gain_db_ = device_profile(device_type_).default_gain_db; }

BenchCapture::~BenchCapture() { stop(); }

void BenchCapture::set_device_type(SdrDeviceType type) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    device_type_ = type;
}
SdrDeviceType BenchCapture::device_type() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return device_type_;
}

void BenchCapture::start(size_t usrp_channel) {
    if (running_.load()) return;
    usrp_channel_ = usrp_channel;
    stop_flag_.store(false);
    running_.store(true);
    thread_ = std::thread(&BenchCapture::run, this);
}

void BenchCapture::stop() {
    stop_flag_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

bool BenchCapture::running() const { return running_.load(); }

void BenchCapture::set_target_freq_hz(double hz) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    target_freq_hz_ = hz;
}
double BenchCapture::target_freq_hz() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return target_freq_hz_;
}
void BenchCapture::set_gain(std::optional<double> gain_db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    gain_db_ = gain_db;
    gain_dirty_ = true;
}
void BenchCapture::set_threshold_db(double db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    threshold_db_ = db;
}
double BenchCapture::threshold_db() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return threshold_db_;
}
void BenchCapture::set_chunk_duration_s(double s) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    chunk_duration_s_ = s;
}
double BenchCapture::chunk_duration_s() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return chunk_duration_s_;
}

BenchStatus BenchCapture::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

std::vector<BenchPacketPtr> BenchCapture::packets_snapshot() const {
    std::lock_guard<std::mutex> lock(packet_mutex_);
    return packets_;  // vector<shared_ptr> copy - cheap, IQ buffers are not duplicated
}

void BenchCapture::clear_packets() {
    std::lock_guard<std::mutex> lock(packet_mutex_);
    packets_.clear();
}

LiveChunkPtr BenchCapture::latest_chunk_snapshot() const {
    std::lock_guard<std::mutex> lock(chunk_mutex_);
    return latest_chunk_;
}

// Same retry discipline as Scanner::connect_sdr() (see that function's
// own comment) - the X310's Ethernet control channel in particular
// fails to come up cleanly on a real, measured fraction of attempts,
// independent of this app; harmless extra patience for the B210's USB3
// link, which doesn't share that failure mode.
bool BenchCapture::connect_sdr(SdrDeviceType type, size_t usrp_channel, std::optional<double> gain) {
    const DeviceProfile profile = device_profile(type);
    std::exception_ptr last_error;
    for (int attempt = 0; attempt < 6 && !stop_flag_.load(); ++attempt) {
        try {
            sdr_.reset();
        } catch (...) {
        }
        try {
            sdr_ = std::make_unique<UsrpCapture>(profile.antenna, gain, usrp_channel,
                                                  profile.device_args);
            last_error = nullptr;
            break;
        } catch (const std::exception&) {
            last_error = std::current_exception();
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        }
    }
    if (last_error) {
        std::string message;
        try {
            std::rethrow_exception(last_error);
        } catch (const std::exception& e) {
            message = e.what();
        }
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = false;
        status_.error = message;
        return false;
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.connected = true;
    status_.error.clear();
    return true;
}

void BenchCapture::run() {
    SdrDeviceType type;
    std::optional<double> initial_gain;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        type = device_type_;
        initial_gain = gain_db_;
        gain_dirty_ = false;
    }
    const DeviceProfile profile = device_profile(type);
    if (!connect_sdr(type, usrp_channel_, initial_gain)) {
        running_.store(false);
        return;
    }

    while (!stop_flag_.load()) {
        double target_freq_hz, threshold_db, chunk_duration_s;
        std::optional<double> pending_gain;
        bool apply_gain;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            target_freq_hz = target_freq_hz_;
            threshold_db = threshold_db_;
            chunk_duration_s = chunk_duration_s_;
            apply_gain = gain_dirty_;
            pending_gain = gain_db_;
            gain_dirty_ = false;
        }
        if (apply_gain) sdr_->set_gain(pending_gain);

        // Same DC-guard-avoidance offset scanner.cpp applies to every
        // Wi-Fi scan step (see config.hpp's WIFI_CHANNEL_CAPTURE_OFFSET_HZ) -
        // the USRP is tuned 1.5MHz off the frequency you actually asked
        // to receive, and every downstream formula (classify_modulation,
        // the fingerprint extractors) is handed both centers so it can
        // mix the offset back out coherently.
        double capture_center_hz = target_freq_hz + WIFI_CHANNEL_CAPTURE_OFFSET_HZ;
        auto [iq, actual_rate, overflow] =
            sdr_->capture(capture_center_hz, profile.max_sample_rate_hz, chunk_duration_s);

        rx_fail_streak_ = iq.empty() ? (rx_fail_streak_ + 1) : 0;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.connected = true;
            status_.last_overflow = overflow;
            status_.actual_sample_rate_hz = actual_rate;
            status_.rx_stalled = rx_fail_streak_ >= kRxStallThreshold;
            status_.chunk_count += 1;
        }
        if (iq.empty()) continue;

        {
            auto chunk = std::make_shared<LiveChunk>();
            chunk->iq = iq;  // top-half live view needs its own copy - `iq` is reused below
            chunk->sample_rate_hz = actual_rate;
            chunk->capture_center_hz = capture_center_hz;
            chunk->segment_center_hz = target_freq_hz;
            std::lock_guard<std::mutex> lock(chunk_mutex_);
            chunk->chunk_id = next_chunk_id_++;
            latest_chunk_ = std::move(chunk);
        }

        auto bursts = wifi::detect_bursts(iq.data(), iq.size(), actual_rate, threshold_db,
                                           WIFI_MAX_BURSTS_PER_CAPTURE);
        std::string ts = current_time_hhmmss();

        for (const auto& b : bursts) {
            std::vector<std::complex<float>> window(iq.begin() + long(b.start),
                                                      iq.begin() + long(b.start + b.length));
            auto result = wifi::classify_modulation(window, actual_rate, capture_center_hz,
                                                      target_freq_hz, /*try_dsss=*/true);
            if (result.mod == wifi::ModClass::Unknown) continue;

            double bw_hz = wifi::estimate_occupied_bandwidth_hz(window.data(), window.size(),
                                                                  actual_rate);
            if (bw_hz < wifi::MIN_WIFI_BANDWIDTH_HZ) continue;

            double power_db = wifi::estimate_mean_power_db(window.data(), window.size());
            double duration_s = double(b.length) / actual_rate;

            std::optional<wifi_fingerprint::WifiFingerprint> fp_opt;
            std::optional<wifi::BeaconInfo> identity;

            if (result.mod == wifi::ModClass::OFDM && result.has_preamble_range) {
                fp_opt = wifi_fingerprint::extract_ofdm_fingerprint(
                    iq.data(), iq.size(), b.start, b.length, actual_rate, capture_center_hz,
                    target_freq_hz, target_freq_hz, result);
            } else if (result.mod == wifi::ModClass::DSSS) {
                // Unlike scanner.cpp's production loop, decode/fingerprint
                // EVERY DSSS burst here regardless of duration - see this
                // file's header comment for why the beacon-duration gate
                // doesn't apply to a deliberately-sent single test packet.
                auto dec = wifi::decode_dsss_burst(window.data(), window.size(), actual_rate,
                                                    capture_center_hz, target_freq_hz);
                if (dec.beacon) identity = dec.beacon;
                if (dec.preamble_found && !dec.sync_symbols.empty()) {
                    fp_opt = wifi_fingerprint::extract_dsss_fingerprint(target_freq_hz, dec);
                }
            }

            auto row = std::make_shared<BenchPacketRow>();
            row->time = ts;
            row->freq_mhz = target_freq_hz / 1e6;
            row->modulation = (result.mod == wifi::ModClass::DSSS) ? "DSSS" : "OFDM";
            row->power_db = power_db;
            row->bandwidth_khz = bw_hz / 1e3;
            row->duration_us = duration_s * 1e6;
            row->confidence = result.confidence;
            row->identity = std::move(identity);
            if (fp_opt.has_value()) {
                row->fp_cfo_ppm = fp_opt->cfo_ppm;
                row->fp_dc_dbc = fp_opt->dc_dbc;
                row->fp_dc_ang_deg = fp_opt->dc_ang_deg;
                row->fp_snr_db = fp_opt->snr_db;
                row->fp_evm_pct = fp_opt->evm_pct;
                row->fp_sync_corr = fp_opt->sync_corr;
                row->fp_n_samp = fp_opt->n_samp;
                // Structurally unset on DSSS regardless of gate outcome -
                // see wifi_fingerprint.hpp's extract_dsss_fingerprint()
                // comment (real-valued BPSK reference can't resolve
                // gain/phase imbalance by this technique).
                if (result.mod == wifi::ModClass::OFDM) {
                    row->fp_irr_db = fp_opt->irr_db;
                    row->fp_iq_eps = fp_opt->iq_eps;
                    row->fp_iq_phi_deg = fp_opt->iq_phi_deg;
                }
                if (fp_opt->gated_out) row->fp_gate_reason = fp_opt->gate_reason;
            }
            row->iq = std::move(window);
            row->sample_rate_hz = actual_rate;
            row->capture_center_hz = capture_center_hz;
            row->segment_center_hz = target_freq_hz;

            std::lock_guard<std::mutex> lock(packet_mutex_);
            row->seq = next_seq_++;
            packets_.push_back(std::move(row));
            if (packets_.size() > BENCH_PACKET_LOG_MAX) packets_.erase(packets_.begin());
        }
    }

    try {
        sdr_.reset();
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = false;
    }
    running_.store(false);
}

}  // namespace rfmon::bench
