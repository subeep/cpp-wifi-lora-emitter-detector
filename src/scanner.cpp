#include "scanner.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "classifier.hpp"
#include "lora_phy.hpp"
#include "spectrum.hpp"

namespace rfmon {

namespace {
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string current_time_hhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return std::string(buf);
}

// Python-repr-like rendering: printable ASCII as-is, else \xHH escapes.
std::string payload_to_repr(const std::vector<uint8_t>& payload) {
    std::string repr = "b'";
    for (uint8_t b : payload) {
        if (b >= 0x20 && b < 0x7f && b != '\'' && b != '\\') {
            repr += static_cast<char>(b);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", b);
            repr += buf;
        }
    }
    repr += "'";
    return repr;
}
}  // namespace

Scanner::Scanner() = default;

Scanner::~Scanner() { stop(); }

void Scanner::start() {
    stop_flag_ = false;
    thread_ = std::thread(&Scanner::run, this);
}

void Scanner::stop() {
    stop_flag_ = true;
    if (thread_.joinable()) thread_.join();
}

void Scanner::set_active_band(const std::string& band) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    active_band_ = band;
}

std::string Scanner::active_band() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return active_band_;
}

void Scanner::set_device_type(SdrDeviceType type) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    device_type_ = type;
}

SdrDeviceType Scanner::device_type() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return device_type_;
}

void Scanner::set_threshold_db(double db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    threshold_db_ = db;
}

double Scanner::threshold_db() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return threshold_db_;
}

void Scanner::set_gain(std::optional<double> gain_db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    gain_db_ = gain_db;
    gain_dirty_ = true;
}

std::optional<double> Scanner::gain() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return gain_db_;
}

void Scanner::set_lora_lock_freq(std::optional<double> freq_hz) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    lora_lock_freq_ = freq_hz;
}

std::optional<double> Scanner::lora_lock_freq() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return lora_lock_freq_;
}

std::vector<DeviceRow> Scanner::snapshot(const std::string& band) const {
    // registry_for() is non-const (map-lookup by reference); the
    // registries themselves are separately mutex-protected, so a
    // const_cast here is safe - it never mutates Scanner's own state.
    return const_cast<Scanner*>(this)->registry_for(band).snapshot(now_seconds());
}

ScannerStatus Scanner::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

std::vector<LoraPacketRow> Scanner::lora_packets() const {
    std::lock_guard<std::mutex> lock(lora_log_mutex_);
    return lora_packet_log_;
}

DeviceRegistry& Scanner::registry_for(const std::string& band) {
    if (band == BAND_SUB_GHZ) return registry_lora_;
    if (band == BAND_WIFI_2G4) return registry_wifi24_;
    return registry_wifi5_;
}

void Scanner::note_capture_health(bool got_data) {
    rx_fail_streak_ = got_data ? 0 : (rx_fail_streak_ + 1);
    bool stalled = rx_fail_streak_ >= kRxStallThreshold;
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.rx_stalled = stalled;
}

// X310 connections over Ethernet fail intermittently (confirmed via
// repeated `uhd_usrp_probe` runs against this same unit, independent of
// this app - roughly 2 in 5 attempts, not just a rare one-off) with
// uhd::rfnoc_error "Failure to create rfnoc_graph" or an io_error
// management-transaction timeout. At that failure rate 3 attempts still
// has a non-negligible chance of exhausting all of them (~6%), so this
// retries more and spaces attempts out a bit more to drive the odds
// down further (~0.4% for 6 attempts) rather than requiring a manual
// retry via the device selector.
bool Scanner::connect_sdr(const DeviceProfile& profile, std::optional<double> gain) {
    std::exception_ptr last_error;
    for (int attempt = 0; attempt < 6 && !stop_flag_.load(); ++attempt) {
        // Explicitly destroy any previous handle *before* constructing
        // a new one (rather than relying on the assignment below to do
        // it implicitly) and swallow anything it throws. When a
        // handle's control channel has gone fully dark (link saturated,
        // device unplugged, etc.), even its destructor's best-effort
        // hardware shutdown can throw a real uhd::exception from deep
        // inside UHD's teardown path - confirmed in practice as an
        // uncaught 'uhd::op_timeout' that took the whole process down.
        // That's the handle we're discarding anyway, so it doesn't
        // matter if its teardown was clean.
        try {
            sdr_.reset();
        } catch (...) {
        }
        try {
            sdr_ = std::make_unique<UsrpCapture>(profile.antenna, gain, 0, profile.device_args);
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

// Only called while BAND_SUB_GHZ is active, once per scan cycle, on
// one channel from LORA_LISTEN_CHANNELS_HZ (rotating channel each
// cycle rather than all 3 every cycle, to keep each cycle's added time
// to ~LORA_LISTEN_DURATION_S instead of 3x that - UI responsiveness).
// Ported from the validated Python tools/lora_listen.py: for each
// candidate SF, try a full decode first, and only if that fails (most
// likely for real third-party hardware whose header encoding isn't
// reverse-engineered yet - see lora_phy.hpp) fall back to a bare burst
// detection. Every SF in the list is tried independently, exactly like
// the Python version - not just the first hit.
void Scanner::run_lora_listen_step(double freq_hz) {
    auto [iq, actual_rate, overflow] =
        sdr_->capture(freq_hz, LORA_LISTEN_SAMPLE_RATE_HZ, LORA_LISTEN_DURATION_S);
    note_capture_health(!iq.empty());
    if (iq.empty()) return;
    std::string ts = current_time_hhmmss();

    for (int sf : LORA_LISTEN_SF_LIST) {
        auto decoded = lora::demodulate(iq, sf);
        if (decoded.has_value() && decoded->header_valid) {
            LoraPacketRow row;
            row.time = ts;
            row.status = "decoded";
            row.freq_mhz = freq_hz / 1e6;
            row.sf = decoded->sf;
            row.cr = decoded->cr;
            row.payload_len = static_cast<int>(decoded->payload.size());
            row.crc_valid = decoded->crc_valid;
            row.cfo_bins = decoded->cfo_bins;
            row.payload_repr = payload_to_repr(decoded->payload);
            std::lock_guard<std::mutex> lock(lora_log_mutex_);
            lora_packet_log_.push_back(std::move(row));
            if (lora_packet_log_.size() > static_cast<size_t>(LORA_PACKET_LOG_MAX)) {
                lora_packet_log_.erase(lora_packet_log_.begin());
            }
            continue;
        }

        auto burst = lora::detect_burst(iq, sf);
        if (burst.has_value()) {
            LoraPacketRow row;
            row.time = ts;
            row.status = "detected";
            row.freq_mhz = freq_hz / 1e6;
            row.sf = burst->sf;
            row.cfo_bins = burst->cfo_bins;
            std::lock_guard<std::mutex> lock(lora_log_mutex_);
            lora_packet_log_.push_back(std::move(row));
            if (lora_packet_log_.size() > static_cast<size_t>(LORA_PACKET_LOG_MAX)) {
                lora_packet_log_.erase(lora_packet_log_.begin());
            }
        }
    }
}

void Scanner::run() {
    std::optional<double> initial_gain;
    SdrDeviceType type;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        initial_gain = gain_db_;
        gain_dirty_ = false;
        type = device_type_;
    }
    DeviceProfile profile = device_profile(type);
    if (!connect_sdr(profile, initial_gain)) return;

    std::string last_band_seen;
    int band_cycle_count = 0;

    while (!stop_flag_.load()) {
        std::string band;
        double threshold;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            band = active_band_;
            threshold = threshold_db_;
            if (gain_dirty_) {
                sdr_->set_gain(gain_db_);
                gain_dirty_ = false;
            }
        }
        if (band != last_band_seen) {
            band_cycle_count = 0;
            last_band_seen = band;
        }

        std::vector<ScanStep> plan = scan_plan_for_band(band);
        std::vector<Detection> detections;
        bool any_overflow = false;

        for (const auto& step : plan) {
            if (stop_flag_.load()) break;
            {
                // Mode changed mid-cycle: abandon the rest of this
                // cycle's steps and let the outer loop pick up the new
                // band fresh next iteration.
                std::lock_guard<std::mutex> lock(config_mutex_);
                if (active_band_ != band) break;
            }

            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.active_band = band;
                status_.active_step_label = step.label;
            }

            // Clamped to the connected device's actual transport
            // capacity (see DeviceProfile::max_sample_rate_hz in
            // config.hpp) - requesting more than a link can carry
            // doesn't just degrade gracefully, it can starve the
            // control channel and take the whole connection down.
            double request_rate = std::min(step.sample_rate_hz, profile.max_sample_rate_hz);
            std::vector<std::vector<std::complex<float>>> captures;
            double actual_rate = request_rate;
            for (int i = 0; i < SUB_CAPTURES_PER_STEP; ++i) {
                auto [iq, rate, overflow] =
                    sdr_->capture(step.center_hz, request_rate, SUB_CAPTURE_DURATION_S);
                actual_rate = rate;
                any_overflow = any_overflow || overflow;
                if (!iq.empty()) captures.push_back(std::move(iq));
            }
            note_capture_health(!captures.empty());
            if (captures.empty()) continue;

            Spectrum spec = max_hold_spectrum(captures, actual_rate);
            double guard_hz = std::max(actual_rate * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
            std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
            std::vector<bool> edge_mask =
                mask_edge_guard(spec.freqs_offset_hz, actual_rate, EDGE_GUARD_FRACTION);

            std::vector<Segment> segments =
                find_segments(spec.freqs_offset_hz, spec.psd_db, step.center_hz, edge_mask,
                              dc_mask, NOISE_FLOOR_PERCENTILE, threshold, MIN_SEGMENT_BINS,
                              MERGE_GAP_BINS);
            for (const auto& seg : segments) {
                detections.push_back(Detection{step.band, seg, classify(step.band, seg)});
            }
        }

        DeviceRegistry& reg = registry_for(band);
        double now = now_seconds();
        reg.update_cycle(detections, now);
        reg.end_cycle();
        ++band_cycle_count;

        // LoRa PHY listen: only while LoRa mode is active, one IN865
        // channel per cycle (rotating), alongside the energy scan above -
        // unless a single frequency has been locked via
        // set_lora_lock_freq(), in which case every cycle listens on
        // just that frequency instead of rotating.
        if (band == BAND_SUB_GHZ && !stop_flag_.load()) {
            bool still_active;
            std::optional<double> locked_freq;
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                still_active = (active_band_ == band);
                locked_freq = lora_lock_freq_;
            }
            if (still_active) {
                double freq_hz;
                if (locked_freq.has_value()) {
                    freq_hz = *locked_freq;
                } else {
                    freq_hz =
                        LORA_LISTEN_CHANNELS_HZ[lora_channel_idx_ % LORA_LISTEN_CHANNELS_HZ.size()];
                    ++lora_channel_idx_;
                }
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    status_.active_step_label = "LoRa PHY listen @ " +
                                                 std::to_string(freq_hz / 1e6) + " MHz" +
                                                 (locked_freq.has_value() ? " (locked)" : "");
                }
                run_lora_listen_step(freq_hz);
            }
        }

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.cycle_count = band_cycle_count;
            status_.last_overflow = any_overflow;
        }

        // If RX has genuinely stopped flowing for several cycles in a
        // row (see note_capture_health()) - observed in practice on the
        // X310 as the stream going silent mid-run, physical activity
        // LED included, not just a transient hiccup - recreate the UHD
        // device handle from scratch. UsrpCapture::capture()'s own
        // per-call streamer rebuild (see sdr_capture.cpp) isn't always
        // enough to recover from this; a full reconnect is.
        if (rx_fail_streak_ >= kRxStallThreshold && !stop_flag_.load()) {
            SdrDeviceType reconnect_type;
            std::optional<double> reconnect_gain;
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                reconnect_type = device_type_;
                reconnect_gain = gain_db_;
            }
            if (connect_sdr(device_profile(reconnect_type), reconnect_gain)) {
                rx_fail_streak_ = 0;
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.rx_stalled = false;
            }
        }
    }

    try {
        sdr_.reset();
    } catch (...) {
        // Same reasoning as connect_sdr()'s pre-reconnect reset: the
        // handle's own teardown can throw if its control channel is
        // already gone, and we're discarding it on the way out anyway.
    }
}

}  // namespace rfmon
