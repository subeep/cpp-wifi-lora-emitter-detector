#include "scanner.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "classifier.hpp"
#include "fingerprint.hpp"
#include "lora_phy.hpp"
#include "lora_phy_std.hpp"
#include "spectrum.hpp"
#include "wifi_dsss_rx.hpp"
#include "wifi_fingerprint.hpp"
#include "wifi_phy.hpp"

// PROJECT_ROOT_DIR is set by CMakeLists.txt to the source tree root for
// any target that links lora_master.cpp - resolving the master-list
// directory from that (not the process's current working directory,
// which for this app has always been wherever it happened to be
// launched from, e.g. build/) is what makes "permanently stored" mean
// something: a `build/` wipe/rebuild must never touch this data. The
// literal fallback only matters if some future target links this file
// without setting the definition; every target that matters here does.
#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

namespace rfmon {

namespace {
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Absolute wall-clock seconds (Unix epoch) - unlike now_seconds() above
// (steady_clock, monotonic but with an arbitrary per-process epoch),
// this is what the persistent lora_master_ list needs: a timestamp
// that means the same thing before and after a process restart.
int64_t now_epoch_seconds() { return int64_t(std::time(nullptr)); }

std::string current_time_hhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return std::string(buf);
}

// Which numbered Wi-Fi channel a frequency is closest to, for the
// packet list's Channel column. Mirrors classifier.cpp's own
// nearest_channel() rather than sharing it - that one is file-local
// there, and this file already follows the project's duplicate-a-small-
// helper-rather-than-refactor-validated-code convention.
int nearest_wifi_channel(const std::string& band, double freq_hz) {
    const auto& channels =
        (band == BAND_WIFI_2G4) ? wifi_2g4_channels() : wifi_5g_channels();
    int best_ch = channels.begin()->first;
    double best_dist = std::abs(channels.begin()->second - freq_hz);
    for (const auto& [ch, f] : channels) {
        double dist = std::abs(f - freq_hz);
        if (dist < best_dist) {
            best_dist = dist;
            best_ch = ch;
        }
    }
    return best_ch;
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

// Decimate-by-factor with a basic boxcar (moving-average) anti-alias
// filter, rather than naive sample-dropping - used only when a
// device's achievable LoRa-listen capture rate isn't exactly the rate
// the LoRa codec assumes (125kHz, see lora_phy.hpp and
// DeviceProfile::lora_listen_capture_rate_hz in config.hpp). Not a
// rigorous polyphase resampler, but factor==2 (the only case this is
// currently used for - the X310) puts the new Nyquist edge exactly at
// the real LoRa signal's own occupied-bandwidth edge, so a simple
// 2-tap average is a real low-pass step, not just decoration.
// factor<=1 (the B210, already at exactly 125kHz) is a no-op copy.
std::vector<std::complex<float>> decimate_boxcar(const std::vector<std::complex<float>>& in,
                                                  int factor) {
    if (factor <= 1) return in;
    std::vector<std::complex<float>> out(in.size() / factor);
    for (size_t i = 0; i < out.size(); ++i) {
        std::complex<float> sum(0.0f, 0.0f);
        for (int j = 0; j < factor; ++j) sum += in[i * factor + j];
        out[i] = sum / float(factor);
    }
    return out;
}
}  // namespace

Scanner::Scanner()
    : lora_master_(std::string(PROJECT_ROOT_DIR) + "/data/lora_master"),
      wifi_master_(std::string(PROJECT_ROOT_DIR) + "/data/wifi_master") {}

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

std::vector<lora_master::LoraMasterRow> Scanner::lora_master_snapshot() const {
    return lora_master_.snapshot();
}

std::vector<wifi_master::WifiMasterRow> Scanner::wifi_master_snapshot() const {
    return wifi_master_.snapshot();
}

std::vector<WifiPacketRow> Scanner::wifi_packets() const {
    std::lock_guard<std::mutex> lock(wifi_log_mutex_);
    return wifi_packet_log_;
}

std::map<int, int> Scanner::wifi_source_counts(const std::string& band) const {
    std::lock_guard<std::mutex> lock(wifi_log_mutex_);
    auto it = wifi_source_counts_.find(band);
    return (it == wifi_source_counts_.end()) ? std::map<int, int>{} : it->second;
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
//
// Captures ONCE at profile.lora_listen_capture_rate_hz (500kHz, the
// largest bandwidth in LORA_LISTEN_BW_LIST_HZ - see config.hpp's
// DeviceProfile comment) and tries every (bandwidth, SF) combination
// against that single capture, decimating down per bandwidth hypothesis
// rather than re-capturing 3x. For each combination, tries (in order):
// the standards-compliant (SX1272/76-family) codec first - see
// lora_phy_std.hpp - since that's what real third-party hardware
// (TarangMini and presumably most commercial LoRa modules) actually
// transmits; then this project's own self-consistent codec
// (lora_phy.hpp), relevant for this app's own B210 TX/RX loopback and
// HackRF interop, not third-party devices; then, if neither header
// decodes, falls back to a bare burst detection. Every combination is
// tried independently - not just the first hit.
void Scanner::run_lora_listen_step(double freq_hz, const DeviceProfile& profile,
                                    double threshold_db, std::vector<Detection>& detections) {
    auto [iq_raw, actual_rate, overflow] = sdr_->capture(
        freq_hz, profile.lora_listen_capture_rate_hz, LORA_LISTEN_DURATION_S);
    note_capture_health(!iq_raw.empty());
    if (iq_raw.empty()) return;

    // Feed the Active emitters table from this same capture rather
    // than requiring a separate wideband one - this is the only
    // capture taken at all while locked to one frequency (see run()'s
    // skip_wideband_scan), so without this the emitter itself could
    // never show up there no matter how well the codec below detects
    // it. Growth is disabled (hysteresis_low == threshold_db) for the
    // same reason as the wideband scan's own sub-GHz call: LoRa
    // classification depends on tight bandwidth tolerances
    // (lora_bandwidths_hz()) that growth would blur.
    {
        Spectrum spec = max_hold_spectrum({iq_raw}, actual_rate);
        double guard_hz = std::max(actual_rate * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
        std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
        std::vector<bool> edge_mask =
            mask_edge_guard(spec.freqs_offset_hz, actual_rate, EDGE_GUARD_FRACTION);
        std::vector<Segment> segments =
            find_segments(spec.freqs_offset_hz, spec.psd_db, freq_hz, edge_mask, dc_mask,
                          NOISE_FLOOR_PERCENTILE, threshold_db, MIN_SEGMENT_BINS, MERGE_GAP_BINS,
                          threshold_db);
        for (const auto& seg : segments) {
            detections.push_back(Detection{BAND_SUB_GHZ, seg, classify(BAND_SUB_GHZ, seg)});
        }
    }

    std::string ts = current_time_hhmmss();

    auto push_row = [&](LoraPacketRow row) {
        std::lock_guard<std::mutex> lock(lora_log_mutex_);
        lora_packet_log_.push_back(std::move(row));
        if (lora_packet_log_.size() > static_cast<size_t>(LORA_PACKET_LOG_MAX)) {
            lora_packet_log_.erase(lora_packet_log_.begin());
        }
    };

    for (double bw_hz : LORA_LISTEN_BW_LIST_HZ) {
        int decim = std::max(1, int(std::lround(actual_rate / bw_hz)));
        std::vector<std::complex<float>> iq = decimate_boxcar(iq_raw, decim);
        double bw_khz = bw_hz / 1e3;

        for (int sf : LORA_LISTEN_SF_LIST) {
            auto std_decoded = lora::std_phy::demodulate(iq, sf);
            if (std_decoded.has_value() && std_decoded->header_valid) {
                LoraPacketRow row;
                row.time = ts;
                row.status = "decoded";
                row.freq_mhz = freq_hz / 1e6;
                row.sf = std_decoded->sf;
                row.bandwidth_khz = bw_khz;
                row.cr = std_decoded->cr;
                row.payload_len = static_cast<int>(std_decoded->payload.size());
                row.crc_valid = std_decoded->crc_valid;
                row.cfo_bins = std_decoded->cfo_bins;
                row.payload_repr = payload_to_repr(std_decoded->payload);
                push_row(std::move(row));
                continue;
            }

            auto decoded = lora::demodulate(iq, sf);
            if (decoded.has_value() && decoded->header_valid) {
                LoraPacketRow row;
                row.time = ts;
                row.status = "decoded";
                row.freq_mhz = freq_hz / 1e6;
                row.sf = decoded->sf;
                row.bandwidth_khz = bw_khz;
                row.cr = decoded->cr;
                row.payload_len = static_cast<int>(decoded->payload.size());
                row.crc_valid = decoded->crc_valid;
                row.cfo_bins = decoded->cfo_bins;
                row.payload_repr = payload_to_repr(decoded->payload);
                push_row(std::move(row));
                continue;
            }

            auto burst = lora::detect_burst(iq, sf);
            if (burst.has_value()) {
                // Fingerprint extraction (see fingerprint.hpp) - only
                // wired up on this path, not the two decode attempts
                // above: BurstDetection is the only one of the three
                // that carries preamble_len, and in practice it's the
                // only path that ever fires for TarangMini anyway
                // (its header/CRC never validates - see
                // TARANGMINI_LORA_FINDINGS.md). Runs off the preamble
                // alone, so it doesn't need a decode to have succeeded.
                auto fp = fingerprint::extract_lora_fingerprint(iq, burst->sf, bw_hz, freq_hz,
                                                                  burst->start_sample,
                                                                  burst->preamble_len,
                                                                  burst->cfo_bins);
                fingerprint::append_fingerprint_record(LORA_FINGERPRINT_LOG_PATH, ts,
                                                        freq_hz / 1e6, burst->sf, bw_khz, fp);

                LoraPacketRow row;
                row.time = ts;
                row.status = "detected";
                row.freq_mhz = freq_hz / 1e6;
                row.sf = burst->sf;
                row.bandwidth_khz = bw_khz;
                row.cfo_bins = burst->cfo_bins;
                if (fp.gated_out) {
                    row.fp_gate_reason = fp.gate_reason;
                } else {
                    row.fp_cfo_ppm = fp.cfo_ppm;
                    row.fp_irr_db = fp.irr_db;
                    row.fp_iq_eps = fp.iq_eps;
                    row.fp_iq_phi_deg = fp.iq_phi_deg;
                    row.fp_dc_dbc = fp.dc_dbc;
                    row.fp_dc_ang_deg = fp.dc_ang_deg;
                    row.fp_snr_db = fp.snr_db;
                }
                // n_samp is only ever set once the IQ-imbalance fit has
                // actually run (see fingerprint.cpp) - shown even when
                // the EVM/sync_corr gate itself is what rejected this
                // row, since they're the reason for that rejection.
                if (fp.n_samp > 0) {
                    row.fp_evm_pct = fp.evm_pct;
                    row.fp_sync_corr = fp.sync_corr;
                }
                push_row(std::move(row));

                // Feed the registry too, so a fingerprinted burst can
                // be matched/merged by RF identity (see
                // registry.hpp/.cpp) rather than only ever by frequency
                // bucket. peak_db here is a stand-in using snr_db, not
                // a real power measurement - this Detection's job is
                // identity matching, not power ranking.
                if (!fp.gated_out) {
                    Segment seg{freq_hz, bw_hz, fp.snr_db};
                    FingerprintSnapshot snap;
                    snap.cfo_ppm = fp.cfo_ppm;
                    snap.irr_db = fp.irr_db;
                    snap.iq_eps = fp.iq_eps;
                    snap.iq_phi_deg = fp.iq_phi_deg;
                    snap.dc_dbc = fp.dc_dbc;
                    snap.dc_ang_deg = fp.dc_ang_deg;
                    Detection det{BAND_SUB_GHZ, seg, classify(BAND_SUB_GHZ, seg), snap};
                    detections.push_back(std::move(det));

                    // Permanent, cross-run identity list (see
                    // lora_master.hpp) - separate from the session-only
                    // registry fed just above; only ever sees readings
                    // that already cleared every gate.
                    lora_master_.record_reading(freq_hz, burst->sf, bw_hz, fp,
                                                 now_epoch_seconds());
                }
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
        std::optional<double> locked_freq;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            band = active_band_;
            threshold = threshold_db_;
            locked_freq = lora_lock_freq_;
            if (gain_dirty_) {
                sdr_->set_gain(gain_db_);
                gain_dirty_ = false;
            }
        }
        if (band != last_band_seen) {
            band_cycle_count = 0;
            last_band_seen = band;
        }

        std::vector<Detection> detections;
        bool any_overflow = false;

        // While locked to one exact LoRa frequency, skip the generic
        // wideband energy scan entirely (below) and spend every cycle
        // on the dedicated listener instead - the wideband scan exists
        // to notice *other*, unlocated Sub-GHz emitters, which isn't
        // what "lock to frequency" is for, and it otherwise consumed
        // ~4 of every ~6.5 seconds a cycle could instead spend actually
        // listening for a brief LoRa burst at the one frequency that
        // matters here. Tradeoff: the Active emitters table stops
        // updating for anything but the locked frequency while this is
        // active (nothing feeds it), and any of its existing rows age
        // out per DEFAULT_EXPIRE_CYCLES faster in wall-clock time,
        // since cycles themselves now come much faster.
        bool skip_wideband_scan = (band == BAND_SUB_GHZ) && locked_freq.has_value();
        std::vector<ScanStep> plan = skip_wideband_scan ? std::vector<ScanStep>{} : scan_plan_for_band(band);

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

            // Hysteresis growth only helps the WiFi bands (see
            // HYSTERESIS_LOW_RATIO's comment) - LoRa classification
            // depends on comparatively tight bandwidth tolerances
            // (lora_bandwidths_hz()), and growth measuring a wider
            // segment risks pushing a real 125kHz signal into the
            // 250kHz bucket instead. Passing threshold itself as the
            // low bar disables growth (low == high) for that band,
            // reproducing the original non-hysteresis behavior exactly.
            double hysteresis_low =
                (step.band == BAND_SUB_GHZ) ? threshold : threshold * HYSTERESIS_LOW_RATIO;
            std::vector<Segment> segments =
                find_segments(spec.freqs_offset_hz, spec.psd_db, step.center_hz, edge_mask,
                              dc_mask, NOISE_FLOOR_PERCENTILE, threshold, MIN_SEGMENT_BINS,
                              MERGE_GAP_BINS, hysteresis_low);
            // Generic energy-detected segments - unrelated to WiFi
            // modulation classification below, still how BLE/Zigbee
            // narrowband and "Unknown emitter" rows get found, for
            // every band including LoRa/sub-GHz. Untouched.
            for (const auto& seg : segments) {
                detections.push_back(Detection{step.band, seg, classify(step.band, seg)});
            }

            // WiFi modulation detection: runs unconditionally against
            // this channel's own captures, independent of the segments
            // above - a correlator hit IS the detection signal (see
            // wifi_phy.hpp's file header for why bandwidth can't be a
            // gate here). Each WiFi ScanStep is already centered on one
            // real channel, offset by WIFI_CHANNEL_CAPTURE_OFFSET_HZ
            // (see config.hpp's scan_plan_for_band()) to keep that
            // channel's peak off the DC-guard notch - recover the real
            // channel center to mix baseband/report the emitter at,
            // rather than the offset tuned frequency.
            if (step.band == BAND_WIFI_2G4 || step.band == BAND_WIFI_5G) {
                bool try_dsss = (step.band == BAND_WIFI_2G4);  // no DSSS/CCK in 5GHz
                double real_channel_hz = step.center_hz - WIFI_CHANNEL_CAPTURE_OFFSET_HZ;
                int channel_num = nearest_wifi_channel(step.band, real_channel_hz);
                std::string ts = current_time_hhmmss();

                // Classify per BURST rather than per capture. Two wins
                // at once: every frame becomes its own reportable row
                // (see WifiPacketRow), and the correlators stop
                // grinding over 20M-sample buffers of mostly idle air,
                // which is what made a full 2.4GHz sweep take minutes.
                // Label the channel by which modulation owns the most
                // AIRTIME, not by whichever single burst scored highest.
                // OFDM bursts consistently score above DSSS ones, so a
                // peak-confidence pick made DSSS invisible even on
                // channels where it dominated - a channel carrying
                // mostly 1 Mbps beacons was still labelled OFDM because
                // one short OFDM frame edged it out.
                std::map<std::string, wifi::BeaconInfo> decoded_beacons;
                double airtime_dsss = 0.0, airtime_ofdm = 0.0;
                double best_bw_dsss = 0.0, best_power_dsss = -200.0;
                double best_bw_ofdm = 0.0, best_power_ofdm = -200.0;
                int source_count = 0;
                for (const auto& cap : captures) {
                    auto bursts = wifi::detect_bursts(cap.data(), cap.size(), actual_rate,
                                                       threshold, WIFI_MAX_BURSTS_PER_CAPTURE);
                    // Beacon-train timing is per-capture: each capture
                    // has its own t0, so phases cannot be compared
                    // across them. Cluster within each and keep the
                    // most sources any one capture resolved.
                    std::vector<double> burst_starts_s, burst_powers_db;
                    for (const auto& b : bursts) {
                        std::vector<std::complex<float>> window(
                            cap.begin() + long(b.start), cap.begin() + long(b.start + b.length));
                        auto result = wifi::classify_modulation(window, actual_rate,
                                                                  step.center_hz, real_channel_hz,
                                                                  try_dsss);
                        if (result.mod == wifi::ModClass::Unknown) continue;

                        double bw_hz = wifi::estimate_occupied_bandwidth_hz(
                            window.data(), window.size(), actual_rate);
                        // Nothing in Wi-Fi is narrowband. Rejecting
                        // sub-4MHz bursts here is what keeps Bluetooth,
                        // BLE and Zigbee - all of which share this band
                        // and can correlate against Barker - out of the
                        // packet list. See MIN_WIFI_BANDWIDTH_HZ.
                        if (bw_hz < wifi::MIN_WIFI_BANDWIDTH_HZ) continue;

                        double power_db =
                            wifi::estimate_mean_power_db(window.data(), window.size());
                        double duration_s = double(b.length) / actual_rate;

                        // RF fingerprint extraction (see
                        // wifi_fingerprint.hpp - a separate engine from
                        // LoRa's) - OFDM runs unconditionally on every
                        // burst that already cleared the bandwidth gate
                        // above; DSSS is scoped to the SAME beacon-
                        // duration-gated bursts as the decode attempt
                        // below (see that block's own comment for the
                        // "why beacon-plausible only" reasoning, which
                        // applies equally here - a documented scope
                        // limit, not a hard requirement of the math).
                        std::optional<wifi_fingerprint::WifiFingerprint> fp_opt;
                        std::optional<std::string> fp_mac;
                        std::optional<wifi::BeaconInfo> packet_identity;
                        std::string master_key;
                        const int64_t packet_ts = now_epoch_seconds();
                        if (result.mod == wifi::ModClass::OFDM && result.has_preamble_range) {
                            fp_opt = wifi_fingerprint::extract_ofdm_fingerprint(
                                cap.data(), cap.size(), b.start, b.length, actual_rate,
                                step.center_hz, real_channel_hz, real_channel_hz, result);
                        }

                        // Only feed BEACON-PLAUSIBLE bursts to the
                        // cadence clustering. This is not an
                        // optimisation, it is what makes the method work
                        // at all: with every burst included, a busy
                        // channel puts ~400 events into a 102.4ms phase
                        // space at ~1ms resolution, which saturates it -
                        // every phase bin is occupied on every interval,
                        // so periodicity carries no information and
                        // coincidental chains dominate. Measured: 26
                        // phantom sources per capture from purely
                        // aperiodic traffic at that density.
                        //
                        // A 2.4GHz beacon at the 1 Mbps basic rate runs
                        // ~2-3ms, while data frames are tens to a few
                        // hundred microseconds, so duration separates
                        // them cleanly and drops the candidate count by
                        // more than an order of magnitude.
                        if (result.mod == wifi::ModClass::DSSS &&
                            duration_s >= WIFI_BEACON_MIN_DURATION_S) {
                            burst_starts_s.push_back(double(b.start) / actual_rate);
                            burst_powers_db.push_back(power_db);

                            // A beacon-shaped DSSS burst is worth a full
                            // decode attempt: it is the one frame that
                            // names its own network. Cheap to try and
                            // self-validating - the FCS either passes or
                            // the frame is discarded, so a returned
                            // beacon is real identity rather than
                            // inference.
                            auto dec = wifi::decode_dsss_burst(window.data(), window.size(),
                                                                actual_rate, step.center_hz,
                                                                real_channel_hz);
                            if (dec.beacon) {
                                decoded_beacons[dec.beacon->bssid] = *dec.beacon;
                                // A decoded beacon's BSSID is a real,
                                // zero-ambiguity 48-bit MAC - use it as
                                // the master-list's primary key rather
                                // than fingerprint-cluster matching
                                // (see wifi_master.hpp's file header).
                                fp_mac = dec.beacon->bssid;
                                packet_identity = dec.beacon;
                                // Persist verified identity even if RF extraction fails its gates.
                                master_key = wifi_master_.record_identity(*dec.beacon, real_channel_hz, packet_ts);
                            }
                            if (dec.preamble_found && !dec.sync_symbols.empty()) {
                                fp_opt = wifi_fingerprint::extract_dsss_fingerprint(real_channel_hz,
                                                                                     dec);
                            }
                        }

                        if (fp_opt.has_value() && !fp_opt->gated_out) {
                            master_key = wifi_master_.record_reading(fp_mac,
                                                         result.mod == wifi::ModClass::DSSS
                                                             ? "DSSS"
                                                             : "OFDM",
                                                         real_channel_hz, bw_hz, *fp_opt,
                                                         packet_ts);
                        }

                        WifiPacketRow row;
                        row.time = ts;
                        row.freq_mhz = real_channel_hz / 1e6;
                        row.channel = channel_num;
                        row.modulation =
                            (result.mod == wifi::ModClass::DSSS) ? "DSSS" : "OFDM";
                        row.power_db = power_db;
                        row.bandwidth_khz = bw_hz / 1e3;
                        row.duration_us = duration_s * 1e6;
                        row.confidence = result.confidence;
                        row.identity = std::move(packet_identity);
                        row.master_key = std::move(master_key);
                        if (fp_opt.has_value()) {
                            row.fp_cfo_ppm = fp_opt->cfo_ppm;
                            row.fp_dc_dbc = fp_opt->dc_dbc;
                            row.fp_dc_ang_deg = fp_opt->dc_ang_deg;
                            row.fp_snr_db = fp_opt->snr_db;
                            row.fp_evm_pct = fp_opt->evm_pct;
                            row.fp_sync_corr = fp_opt->sync_corr;
                            // Left unset on every DSSS row regardless
                            // of gate outcome - see WifiPacketRow's own
                            // comment on why (structurally unset, not
                            // gated).
                            if (result.mod == wifi::ModClass::OFDM) {
                                row.fp_irr_db = fp_opt->irr_db;
                                row.fp_iq_eps = fp_opt->iq_eps;
                                row.fp_iq_phi_deg = fp_opt->iq_phi_deg;
                            }
                            if (fp_opt->gated_out) row.fp_gate_reason = fp_opt->gate_reason;
                        }
                        {
                            std::lock_guard<std::mutex> lock(wifi_log_mutex_);
                            wifi_packet_log_.push_back(std::move(row));
                            if (wifi_packet_log_.size() >
                                static_cast<size_t>(WIFI_PACKET_LOG_MAX)) {
                                wifi_packet_log_.erase(wifi_packet_log_.begin());
                            }
                        }

                        if (result.mod == wifi::ModClass::DSSS) {
                            airtime_dsss += duration_s;
                            if (power_db > best_power_dsss) {
                                best_power_dsss = power_db;
                                best_bw_dsss = bw_hz;
                            }
                        } else {
                            airtime_ofdm += duration_s;
                            if (power_db > best_power_ofdm) {
                                best_power_ofdm = power_db;
                                best_bw_ofdm = bw_hz;
                            }
                        }
                    }
                    auto srcs = wifi::find_beacon_sources(burst_starts_s, burst_powers_db);
                    source_count = std::max(source_count, int(srcs.size()));
                }

                wifi::ModClass best_mod = wifi::ModClass::Unknown;
                double best_bw = 0.0, best_power = 0.0;
                if (airtime_dsss > 0.0 || airtime_ofdm > 0.0) {
                    bool dsss_wins = airtime_dsss > airtime_ofdm;
                    best_mod = dsss_wins ? wifi::ModClass::DSSS : wifi::ModClass::OFDM;
                    best_bw = dsss_wins ? best_bw_dsss : best_bw_ofdm;
                    best_power = dsss_wins ? best_power_dsss : best_power_ofdm;
                }

                {
                    std::lock_guard<std::mutex> lock(wifi_log_mutex_);
                    wifi_source_counts_[step.band][channel_num] = source_count;
                }

                // Every decoded beacon becomes its own registry entry,
                // keyed by BSSID - real per-network identity rather than
                // one aggregate row per channel. The channel comes from
                // the beacon's OWN DS Parameter Set, not from whatever
                // we were tuned to: at this capture width an adjacent
                // channel's beacons decode here too, and attributing
                // them to the tuned channel would invent co-channel
                // emitters.
                for (const auto& [bssid, info] : decoded_beacons) {
                    double own_hz = real_channel_hz;
                    const auto& chans = wifi_2g4_channels();
                    auto cit = chans.find(info.channel);
                    if (cit != chans.end()) own_hz = cit->second;

                    Segment seg{own_hz, best_bw > 0.0 ? best_bw : 20e6, best_power};
                    std::string label = "WiFi AP " + bssid;
                    if (!info.ssid.empty()) label += " \"" + info.ssid + "\"";
                    else label += " (hidden)";
                    Detection det{step.band, seg, label};
                    det.modulation_confirmed = true;
                    det.source_id = bssid;
                    detections.push_back(std::move(det));
                }

                // The Active-emitters row for this channel is the best
                // burst seen on it this step, not a whole-buffer average.
                if (best_mod != wifi::ModClass::Unknown) {
                    Segment wifi_seg{real_channel_hz, best_bw, best_power};
                    Detection det{step.band, wifi_seg, classify(step.band, wifi_seg, best_mod)};
                    // Without this the row loses its own frequency
                    // bucket to a bare energy segment on the same
                    // channel and never reaches the GUI - see
                    // Detection::modulation_confirmed.
                    det.modulation_confirmed = true;
                    detections.push_back(std::move(det));
                }

                // Commit this channel's result immediately instead of
                // waiting for the whole sweep. The registry only ever
                // updated at a cycle boundary, and a full 2.4GHz sweep
                // is 52s+, so the table sat empty for a minute at a
                // time. Nothing is dropped by flushing early - the
                // registry has had no expiry since permanence was
                // added - and hit_count keeps its meaning because each
                // channel is still visited exactly once per sweep.
                //
                // Deliberately Wi-Fi only: a sub-GHz cycle has multiple
                // wideband steps that can each see the SAME emitter, so
                // flushing per step there would inflate hit_count. That
                // path keeps accumulating to the end of the cycle.
                registry_for(band).update_cycle(detections, now_seconds());
                detections.clear();
            }
        }

        ++band_cycle_count;

        // LoRa PHY listen: only while LoRa mode is active, one IN865
        // channel per cycle (rotating), alongside the energy scan above -
        // unless a single frequency has been locked via
        // set_lora_lock_freq(), in which case every cycle listens on
        // just that frequency instead of rotating (and the energy scan
        // above is skipped entirely - see skip_wideband_scan). Reuses
        // the same locked_freq read at the top of this cycle rather
        // than re-reading it - it's cycled through fast enough now
        // that any staleness from a lock change mid-cycle self-corrects
        // within one more cycle. Appends its own energy-detected
        // segment(s) into the same `detections` this cycle's registry
        // update uses below, rather than needing a separate wideband
        // capture just to keep Active emitters populated while locked
        // (see run_lora_listen_step()'s own comment).
        if (band == BAND_SUB_GHZ && !stop_flag_.load()) {
            bool still_active;
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                still_active = (active_band_ == band);
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
                run_lora_listen_step(freq_hz, profile, threshold, detections);
            }
        }

        DeviceRegistry& reg = registry_for(band);
        double now = now_seconds();
        reg.update_cycle(detections, now);

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
