#include "scanner.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "channelize.hpp"
#include "classifier.hpp"
#include "fingerprint.hpp"
#include "lora_phy.hpp"
#include "lora_phy_std.hpp"
#include "lora_capture.hpp"
#include "spectrum.hpp"
#include "security/wifi_mac_frame.hpp"
#include "wifi_burst_pipeline.hpp"
#include "wifi_dsss_rx.hpp"
#include "wifi_ofdm_rx.hpp"
#include "wifi_fingerprint.hpp"
#include "wifi_phy.hpp"
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
#include "cyclostationary/wifi_protocol_adapter.hpp"
#endif

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

Scanner::Scanner() : Scanner(std::string(PROJECT_ROOT_DIR) + "/data") {}

Scanner::Scanner(const std::string& data_root)
    : lora_master_(data_root + "/lora_master"),
      wifi_master_(data_root + "/wifi_master"),
      security_run_id_(wifi_security::new_run_id()) {
    security_config_.run_id = security_run_id_;
    // Baselines and incidents survive restarts (bounded; see wifi_security_baseline.hpp).
    security_config_.state_path = data_root + "/wifi_security/state.json";
    security_ = std::make_unique<wifi_security::WifiSecurityMonitor>(security_config_);
    lora_security_=std::make_unique<lora_security::Monitor>(data_root+"/lora_security/events.ndjson");
}

Scanner::~Scanner() { stop(); }

void Scanner::start() {
    stop_flag_ = false;
    security_->start();
    lora_security_->start();
    running_ = true;
    thread_ = std::thread(&Scanner::run, this);
}

void Scanner::stop() {
    stop_flag_ = true;
    if (thread_.joinable()) thread_.join();
    wifi_master_.flush();
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
    cyclo_shadow_.set_enabled(false);
#endif
    // After the scan thread: everything it submitted is drained and recorded.
    security_->stop();
    lora_security_->stop();
    running_ = false;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (capture_pending_) {
        capture_save_directory_.clear();
        capture_pending_ = false;
        capture_message_ = "Pending capture save cancelled because scanning stopped.";
    }
}

wifi_security::CaptureRecord Scanner::new_capture_record(const ScanStep& step, const DeviceProfile& profile,
                                                         double requested_rate_hz, double requested_duration_s,
                                                         const CaptureResult& capture) {
    wifi_security::CaptureRecord r;
    r.run_id = security_run_id_;
    r.capture_seq = ++security_capture_seq_;
    r.radio_session = radio_session_;
    r.band = step.band;
    r.channel_hz = step.center_hz - WIFI_CHANNEL_CAPTURE_OFFSET_HZ;
    r.channel = nearest_wifi_channel(step.band, r.channel_hz);
    r.capture_center_hz = step.center_hz;
    r.requested_rate_hz = requested_rate_hz;
    r.sample_rate_hz = capture.sample_rate_hz;
    r.requested_duration_s = requested_duration_s;
    r.antenna = profile.antenna;
    r.device = profile.device_args;
    const CaptureTiming& t = capture.timing;
    r.actual_rf_hz = t.actual_rf_hz;
    r.actual_dsp_hz = t.actual_dsp_hz;
    r.gain_db = t.gain_db;
    r.samples_requested = t.requested_samples;
    r.samples_received = capture.samples.size();
    r.clock = t.device_time_valid ? wifi_security::ClockDomain::UsrpDevice
              : (t.host_before_ns > 0 ? wifi_security::ClockDomain::HostOnly : wifi_security::ClockDomain::Unknown);
    r.device_time_ns = t.device_time_ns;
    r.host_before_ns = t.host_before_ns;
    r.host_after_ns = t.host_after_ns;
    for (const auto& o : t.overflows) {
        wifi_security::OverflowMark m;
        m.at_sample = o.at_sample;
        if (o.resume_time_valid) m.resume_device_ns = o.resume_device_ns;
        r.overflows.push_back(m);
    }
    r.timed_out = t.timed_out;
    r.exception = t.exception;
    r.retuned = t.retuned;
    r.burst_cap = WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE;
    return r;
}

std::shared_ptr<const wifi_security::SecuritySnapshot> Scanner::wifi_security_snapshot() const {
    return security_->snapshot();
}

wifi_security::QueueStats Scanner::wifi_security_queue_stats() const { return security_->queue_stats(); }

bool Scanner::wifi_security_command(const std::string& action, const std::string& key) {
    wifi_security::ControlCommand c;
    c.action = action;
    c.key = key;
    c.host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
    return security_->submit(std::move(c));
}

bool Scanner::set_wifi_security_recording(const std::string& path, uint64_t max_bytes) {
    if (running_) return false;
    security_config_.record_path = path;
    security_config_.record_max_bytes = max_bytes;
    security_ = std::make_unique<wifi_security::WifiSecurityMonitor>(security_config_);
    return true;
}

std::shared_ptr<const lora_security::Snapshot> Scanner::lora_security_snapshot() const {
    return lora_security_->snapshot();
}
bool Scanner::set_lora_security_recording(const std::string& path) {
    if(running_)return false;
    lora_security_=std::make_unique<lora_security::Monitor>(path);return true;
}

void Scanner::request_lora_capture_save(const std::string& directory) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (capture_pending_ || directory.empty()) return;
    capture_save_directory_ = directory;
    capture_pending_ = true;
    capture_message_ = "Waiting for next LoRa capture (Sub-GHz mode and connected receiver required).";
}
std::string Scanner::lora_capture_message() const {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    return capture_message_;
}
bool Scanner::lora_capture_pending() const {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    return capture_pending_;
}

void Scanner::set_active_band(const std::string& band) {
    std::lock_guard<std::mutex> lock(config_mutex_);
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
    if (active_band_ != band) cyclo_shadow_.invalidate();
#endif
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

bool Scanner::valid_wifi_sample_rate(std::optional<double> hz) {
    // Bounded application input, not a promise that a radio supports every value.
    return !hz || (std::isfinite(*hz) && *hz >= 1e6 && *hz <= 200e6);
}

bool Scanner::set_wifi_sample_rate(std::optional<double> hz) {
    if (running_ || !valid_wifi_sample_rate(hz)) return false;
    std::lock_guard<std::mutex> lock(config_mutex_);
    wifi_sample_rate_hz_ = hz;
    return true;
}

std::optional<double> Scanner::wifi_sample_rate() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return wifi_sample_rate_hz_;
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

void Scanner::set_wifi_fixed_channel(std::optional<int> channel) {
    std::lock_guard<std::mutex> lock(config_mutex_);
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
    if (wifi_fixed_channel_ != channel) cyclo_shadow_.invalidate();
#endif
    wifi_fixed_channel_ = channel;
}

std::optional<int> Scanner::wifi_fixed_channel() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return wifi_fixed_channel_;
}

void Scanner::set_lora_laboratory_mode(bool skip) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    lora_laboratory_mode_ = skip;
}

bool Scanner::lora_laboratory_mode() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return lora_laboratory_mode_;
}

void Scanner::set_lora_capture_seconds(double seconds) {
    if (!std::isfinite(seconds)) return;
    std::lock_guard<std::mutex> lock(config_mutex_);
    lora_capture_seconds_ = std::clamp(seconds, 1.0, 30.0);
}
double Scanner::lora_capture_seconds() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return lora_capture_seconds_;
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

std::vector<LoraDeviceRow> Scanner::lora_devices() const {
    std::lock_guard<std::mutex> lock(lora_log_mutex_);
    std::vector<LoraDeviceRow> out;
    for(const auto& [key,d]:lora_devices_)out.push_back(d);
    return out;
}

std::vector<lora_master::LoraMasterRow> Scanner::lora_master_snapshot() const {
    return lora_master_.snapshot();
}

std::vector<wifi_master::WifiMasterRow> Scanner::wifi_master_snapshot() const {
    return wifi_master_.snapshot();
}

std::map<std::string, uint64_t> Scanner::wifi_phy_format_counts() const {
    std::lock_guard<std::mutex> lock(bss_mutex_);
    return phy_format_counts_;
}

void Scanner::note_beacon(const wifi::BeaconInfo& b, double fallback_hz) {
    double hz = fallback_hz;
    if (b.channel >= 1 && b.channel <= 14) hz = b.channel == 14 ? 2484e6 : 2407e6 + 5e6 * b.channel;
    else if (b.channel >= 32) hz = 5000e6 + 5e6 * b.channel;
    std::lock_guard<std::mutex> lock(bss_mutex_);
    auto& f = bss_index_[b.bssid];
    if (!b.ssid.empty()) f.ssid = b.ssid;
    f.channel_hz = hz;
    if (b.bss_color >= 0 && !b.bss_color_disabled) f.bss_color = b.bss_color;
}

// Networks an 802.11ax (BSS colour) or 802.11ac uplink (partial AID) header
// points to: beacons on a channel inside the PPDU's bandwidth around the
// channel it was received on. Several BSSIDs can match (virtual BSSIDs of
// one radio share both values); all are listed.
std::string Scanner::network_hint(const wifi::HighThroughputSig& s, double channel_hz) const {
    if (!s.crc_ok) return {};
    const bool by_color = s.bss_color >= 0;
    const bool by_aid = s.group_id == 0 && s.partial_aid > 0;
    if (!by_color && !by_aid) return {};
    const double reach = std::max(10e6, s.bandwidth_mhz * 1e6 / 2);
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(bss_mutex_);
    for (const auto& [bssid, f] : bss_index_) {
        if (std::abs(f.channel_hz - channel_hz) > reach) continue;
        if (by_color ? f.bss_color != s.bss_color : wifi::vht_partial_aid(bssid) != s.partial_aid) continue;
        names.push_back((f.ssid.empty() ? std::string("hidden") : wifi::display_text(f.ssid)) + " (" + bssid + ")");
    }
    if (names.empty()) return {};
    std::string out = std::string(by_color ? "BSS colour " + std::to_string(s.bss_color) : "partial AID " + std::to_string(s.partial_aid)) + " -> ";
    for (size_t i = 0; i < names.size() && i < 3; ++i) out += (i ? ", " : "") + names[i];
    if (names.size() > 3) out += " +" + std::to_string(names.size() - 3) + " more";
    return out;
}

std::vector<WifiPacketRow> Scanner::wifi_packets() const {
    std::vector<WifiPacketRow> rows;
    {std::lock_guard<std::mutex> lock(wifi_log_mutex_);rows=wifi_packet_log_;}
    const auto associations=wifi_master_.associations();
    for(auto& r:rows)if(r.master_key.empty()){auto it=associations.find(r.enrolment_source);if(it!=associations.end())r.master_key=it->second;}
    return rows;
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
            sdr_ = device_factory_ ? device_factory_(profile, gain)
                                   : std::make_unique<UsrpCapture>(profile.antenna, gain, 0, profile.device_args);
            ++radio_session_;
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
// rather than re-capturing 3x. Production uses the receive-only explicit PHY
// and keeps every recovered packet. Legacy codecs require a laboratory opt-in.
// SF/BW results are hypotheses, not deduplicated transmitter identities.
void Scanner::run_lora_listen_step(double freq_hz, const DeviceProfile& profile,
                                    double threshold_db, std::vector<Detection>& detections) {
    const double host_start = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto capture_gain = gain();
    const double capture_seconds = std::min(lora_capture_seconds(), 32000000.0 / profile.lora_listen_capture_rate_hz);
    auto received = sdr_->capture_detailed(freq_hz, profile.lora_listen_capture_rate_hz, capture_seconds);
    auto& iq_raw=received.samples;const double actual_rate=received.sample_rate_hz;const bool overflow=received.overflow;
    LoraCapture provenance;
    provenance.sample_rate_hz=actual_rate;provenance.requested_sample_rate_hz=profile.lora_listen_capture_rate_hz;
    provenance.requested_center_hz=freq_hz;provenance.requested_duration_s=capture_seconds;
    provenance.host_start_unix_s=host_start;provenance.requested_gain_db=capture_gain;
    provenance.device_args=profile.device_args;provenance.antenna=profile.antenna;provenance.overflow=overflow;
    provenance.run_id=security_run_id_;provenance.radio_session=radio_session_;
    provenance.capture_seq=++lora_security_capture_seq_;provenance.timing=received.timing;
    note_capture_health(!iq_raw.empty());
    std::string save_directory;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        save_directory.swap(capture_save_directory_);
    }
    if (!save_directory.empty()) {
        std::string message;
        try {
            LoraCapture capture=provenance;
            capture.iq=iq_raw;
            message = "Saved: " + save_lora_capture(save_directory, capture);
            if (overflow) message += " (overflow: discontinuous IQ)";
        } catch (const std::exception& e) { message = std::string("Capture save failed: ") + e.what(); }
        std::lock_guard<std::mutex> lock(capture_mutex_);
        capture_message_ = std::move(message);
        capture_pending_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.last_overflow = overflow;
    }
    decode_lora_iq(iq_raw, provenance, threshold_db, detections, /*energy_segments=*/true);
}

// The LoRa decode half of run_lora_listen_step(), unchanged, also used by
// coordinated mode on channels derived from the sub-GHz visit capture. With
// energy_segments false the 500 kHz energy map is skipped: the wideband
// sub-GHz visit already feeds the Active emitters table.
void Scanner::decode_lora_iq(const std::vector<std::complex<float>>& iq_raw, const LoraCapture& provenance,
                             double threshold_db, std::vector<Detection>& detections, bool energy_segments) {
    const double actual_rate = provenance.sample_rate_hz;
    const double freq_hz = provenance.requested_center_hz;
    const bool overflow = provenance.overflow;
    const bool skip_sync_check = lora_laboratory_mode();
    std::vector<LoraPacketRow> security_rows;
    if (iq_raw.empty()) {
        lora_security_->submit(lora_security::make_batch(provenance,0,{},skip_sync_check));return;
    }

    if (energy_segments) {
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
    }

    std::string ts = current_time_hhmmss();

    std::vector<LoraPacketRow> display_rows;
    auto push_row = [&](LoraPacketRow row) {display_rows.push_back(std::move(row));};

    for (double bw_hz : LORA_LISTEN_BW_LIST_HZ) {
        double ratio = actual_rate / bw_hz;
        if (!std::isfinite(ratio) || ratio < 1 || ratio > 1024 || std::abs(ratio - std::round(ratio)) > 1e-6) continue;
        int decim = int(std::lround(ratio));
        std::vector<std::complex<float>> iq = decimate_boxcar(iq_raw, decim);
        double bw_khz = bw_hz / 1e3;

        for (int sf : LORA_LISTEN_SF_LIST) {
            auto observations = analyze_lora_packets(iq, sf, bw_hz, skip_sync_check);
            if (observations.empty()) continue;
            for(auto& row:observations)row.bandwidth_khz=bw_khz;
            security_rows.insert(security_rows.end(),observations.begin(),observations.end());
            const bool decoded_present = observations.front().header_valid;
            for (auto& decoded : observations) {
                if (!decoded.header_valid) continue;
                decoded.time = ts; decoded.freq_mhz = freq_hz / 1e6;
                decoded.bandwidth_khz = bw_khz;
                if (overflow) decoded.detail += " Capture overflow: sample continuity lost.";
                push_row(std::move(decoded));
            }
            // Legacy RF fingerprinting has its own preamble alignment/gates.
            // Do not attach those unrelated estimates to a decoded packet.
            auto observation = std::optional<LoraPacketRow>(std::move(observations.front()));
            observation->time = ts;
            observation->freq_mhz = freq_hz / 1e6;
            observation->bandwidth_khz = bw_khz;
            if (overflow) observation->detail += " Capture overflow: sample continuity lost.";
            auto burst = lora::detect_burst(iq, sf);
            if (!burst && !decoded_present) push_row(std::move(*observation));
            if (burst.has_value()) {
                // Preserve the independently gated RF fingerprint registry path
                // even when PHY decoding succeeds. Its older burst alignment is
                // not attached to the new packet's header/payload evidence.
                auto fp = fingerprint::extract_lora_fingerprint(iq, burst->sf, bw_hz, freq_hz,
                                                                  burst->start_sample,
                                                                  burst->preamble_len,
                                                                  burst->cfo_bins);
                fingerprint::append_fingerprint_record(LORA_FINGERPRINT_LOG_PATH, ts,
                                                        freq_hz / 1e6, burst->sf, bw_khz, fp);

                LoraPacketRow row = std::move(*observation);
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
                if (!decoded_present) push_row(std::move(row));

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
    if(!skip_sync_check)annotate_lora_receptions(display_rows,actual_rate,
        provenance.run_id+"/session-"+std::to_string(provenance.radio_session)+"/capture-"+std::to_string(provenance.capture_seq));
    {
        std::lock_guard<std::mutex> lock(lora_log_mutex_);
        // Devices named by CRC-valid LoRaWAN frames; hypotheses of one
        // reception count once.
        std::set<std::string> counted;
        const int64_t now_ts=now_epoch_seconds();
        for(const auto& row:display_rows){
            if(row.crc!=LoraPacketRow::Crc::Valid||!row.lorawan_device_id)continue;
            const std::string key=*row.lorawan_device_kind+" "+*row.lorawan_device_id;
            if(!row.reception_id.empty()&&!counted.insert(key+"|"+row.reception_id).second)continue;
            auto& d=lora_devices_[key];
            if(!d.frames){d.kind=*row.lorawan_device_kind;d.id=*row.lorawan_device_id;d.first_seen_ts=now_ts;}
            ++d.frames;++(row.lorawan_uplink?d.uplinks:d.downlinks);d.last_seen_ts=now_ts;
            d.last_freq_hz=row.freq_mhz*1e6;d.last_bw_hz=row.bandwidth_khz*1e3;d.last_sf=row.sf;d.last_type=row.lorawan_candidate.value_or("");
        }
        while(lora_devices_.size()>LORA_DEVICES_MAX)
            lora_devices_.erase(std::min_element(lora_devices_.begin(),lora_devices_.end(),[](const auto& a,const auto& b){return a.second.last_seen_ts<b.second.last_seen_ts;}));
        for(auto& row:display_rows)lora_packet_log_.push_back(std::move(row));
        if(lora_packet_log_.size()>size_t(LORA_PACKET_LOG_MAX))
            lora_packet_log_.erase(lora_packet_log_.begin(),lora_packet_log_.end()-LORA_PACKET_LOG_MAX);
    }
    lora_security_->submit(lora_security::make_batch(provenance,iq_raw.size(),security_rows,skip_sync_check));
}

// One step's processing, moved verbatim out of run() (security plan package B,
// capture/processing overlap): max-hold spectrum and energy segments for
// every band, then - for Wi-Fi - per-capture burst processing, identity /
// fingerprint / packet-row / security-monitor work and the per-hop registry
// flush. Sub-GHz steps get only the energy segments, appended to the
// cycle-wide `detections` exactly as before. The parameter names are the
// old locals, so the body is byte-for-byte the former inline block.
void Scanner::process_step_captures(const ScanStep& step, const std::string& band, double threshold,
                                    double actual_rate,
                                    const std::vector<std::vector<std::complex<float>>>& captures,
                                    std::vector<wifi_security::CaptureRecord>& capture_records,
                                    std::vector<Detection>& detections
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                                    , const CycloCaptureInputs* cyclo_inputs
#endif
                                    ) {
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
        for (size_t ci = 0; ci < captures.size(); ++ci) {
            const auto& cap = captures[ci];
            wifi_security::CaptureRecord& rec = capture_records[ci];
            wifi_master::EnrolmentBatch enrolment_batch;
            const auto processing_start = std::chrono::steady_clock::now();
            // Detect up to the security limit; the first
            // WIFI_MAX_BURSTS_PER_CAPTURE keep the full identity /
            // fingerprint / packet-row treatment, the rest are
            // security-only (see config.hpp).
            auto bursts = wifi::detect_bursts(cap.data(), cap.size(), actual_rate,
                                               threshold, WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE);
            rec.bursts_detected = bursts.size();
            rec.burst_cap_reached = bursts.size() >= WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE;
            rec.analysed_samples = rec.burst_cap_reached && !bursts.empty()
                                       ? bursts.back().start + bursts.back().length
                                       : cap.size();
            // Beacon-train timing is per-capture: each capture
            // has its own t0, so phases cannot be compared
            // across them. Cluster within each and keep the
            // most sources any one capture resolved.
            std::vector<double> burst_starts_s, burst_powers_db;
            // The pure per-burst work (classification + narrowband
            // gate, and the applicable decode) runs in parallel;
            // everything order-dependent below stays sequential in
            // burst order. Shared with the offline security runner
            // (wifi_burst_pipeline.hpp).
            const std::vector<wifi::BurstWork> works = wifi::process_bursts(
                cap.data(), cap.size(), bursts, actual_rate, step.center_hz, real_channel_hz, try_dsss);
            for (size_t bi = 0; bi < bursts.size(); ++bi) {
                const auto& b = bursts[bi];
                const wifi::BurstWork& work = works[bi];
                const wifi::BurstClassification& cls = work.cls;
                if (cls.outcome == wifi::BurstClassification::Outcome::Unknown) { ++rec.bursts_unknown; continue; }
                if (cls.outcome == wifi::BurstClassification::Outcome::Narrowband) { ++rec.bursts_narrowband; continue; }
                const auto& result = cls.result;
                const double bw_hz = cls.bandwidth_hz;
                const double power_db = cls.power_db;
                const double duration_s = cls.duration_s;
                ++(result.mod == wifi::ModClass::DSSS ? rec.bursts_dsss : rec.bursts_ofdm);

                // Past the identity/packet-row limit: security-only.
                // Count the decode outcome, submit any FCS-valid frame,
                // and touch nothing else (no fingerprint, identity,
                // row or cadence input).
                if (bi >= WIFI_MAX_BURSTS_PER_CAPTURE) {
                    ++rec.bursts_beyond_identity_limit;
                    const std::vector<uint8_t>* mpdu = nullptr;
                    const wifi::FrameRfFeatures* rf = nullptr;  // whole-frame RF features
                    std::string phy;
                    int rate_mbps = 0;
                    bool short_dsss = false;
                    if (work.ofdm) {
                        ++rec.ofdm_decode_attempts;
                        if (work.ofdm->fcs_valid) {
                            ++rec.ofdm_fcs_valid;
                            mpdu = &work.ofdm->mpdu; phy = "OFDM"; rate_mbps = work.ofdm->rate_mbps;
                            if (work.ofdm->rf) rf = &*work.ofdm->rf;
                        }
                    } else if (work.dsss.policy.decode) {
                        ++rec.dsss_decode_attempts;
                        if (work.dsss.policy.security_only) ++rec.dsss_security_only_attempts;
                        if (work.dsss.result->fcs_valid) {
                            ++rec.dsss_fcs_valid;
                            mpdu = &work.dsss.result->mpdu; phy = "DSSS"; rate_mbps = 1;
                            if (work.dsss.result->rf) rf = &*work.dsss.result->rf;
                            short_dsss = work.dsss.policy.security_only;
                        }
                    } else {
                        ++rec.dsss_not_attempted;
                    }
                    if (mpdu) {
                        auto ev = wifi_security::make_frame_event(rec, b.start, b.length, phy, rate_mbps,
                                                                  short_dsss, *mpdu, true);
                        if (rf) wifi_security::attach_rf_features(ev, *rf);
                        ev.power_db = power_db;
                        ev.bandwidth_hz = bw_hz;
                        ev.duration_us = duration_s * 1e6;
                        ev.confidence = result.confidence;
                        enrolment_batch.observations.push_back({ev, {}});
                        if (wifi_frame_observer_) { try { wifi_frame_observer_(ev); } catch (...) {} }
                        if (security_->submit(std::move(ev))) ++rec.events_submitted;
                        else ++rec.events_rejected_by_queue;
                    }
                    continue;
                }

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
                wifi::OfdmDecodeResult ofdm_decode;
                const int64_t packet_ts = now_epoch_seconds();
                if (result.mod == wifi::ModClass::OFDM && result.has_preamble_range) {
                    fp_opt = wifi_fingerprint::extract_ofdm_fingerprint(
                        cap.data(), cap.size(), b.start, b.length, actual_rate,
                        step.center_hz, real_channel_hz, real_channel_hz, result);
                }

                if (result.mod == wifi::ModClass::OFDM) {
                    // 4 us of context either side (wifi_burst_pipeline.hpp);
                    // OFDM beacons do not use the DSSS duration gate.
                    ofdm_decode = *work.ofdm;
                    ++rec.ofdm_decode_attempts;
                    if (ofdm_decode.fcs_valid) ++rec.ofdm_fcs_valid;
                    if (ofdm_decode.beacon) {
                        packet_identity = ofdm_decode.beacon;
                        fp_mac = packet_identity->bssid;
                        decoded_beacons[*fp_mac] = *packet_identity;
                        master_key = packet_identity->bssid;
                        note_beacon(*packet_identity, real_channel_hz);
                    }
                    if (ofdm_decode.header_valid) {
                        std::lock_guard<std::mutex> lock(bss_mutex_);
                        ++phy_format_counts_[!ofdm_decode.sig ? std::string("802.11a/g legacy")
                                             : ofdm_decode.sig->crc_ok ? ofdm_decode.sig->format
                                                                      : ofdm_decode.sig->format + " (header CRC failed)"];
                        if (ofdm_decode.sig && ofdm_decode.sig->crc_ok && ofdm_decode.fcs_valid)
                            ++phy_format_counts_[ofdm_decode.sig->format + " (payload decoded)"];
                    }
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
                //
                // Shorter DSSS bursts (deauth, auth, association,
                // EAPOL, control frames) are now also decoded, but
                // for security analysis only - they stay out of the
                // cadence clustering, identity store and
                // fingerprinting. wifi::dsss_burst_policy() is the
                // single place that split is decided.
                //
                // A beacon-shaped DSSS burst is worth a full decode
                // attempt: it is the one frame that names its own
                // network. Cheap to try and self-validating - the FCS
                // either passes or the frame is discarded, so a
                // returned beacon is real identity rather than
                // inference.
                wifi::DsssBurstPolicy dsss_policy;
                std::optional<wifi::DsssDecodeResult> dsss_decode;
                if (result.mod == wifi::ModClass::DSSS) {
                    dsss_policy = work.dsss.policy;
                    dsss_decode = work.dsss.result;
                    if (dsss_policy.decode) {
                        ++rec.dsss_decode_attempts;
                        if (dsss_policy.security_only) ++rec.dsss_security_only_attempts;
                        if (dsss_decode->fcs_valid) ++rec.dsss_fcs_valid;
                    } else {
                        ++rec.dsss_not_attempted;
                    }
                }
                if (dsss_policy.beacon_cadence) {
                    burst_starts_s.push_back(double(b.start) / actual_rate);
                    burst_powers_db.push_back(power_db);
                }
                if (dsss_policy.identity_and_fingerprint) {
                    const auto& dec = *dsss_decode;
                    if (dec.beacon) {
                        decoded_beacons[dec.beacon->bssid] = *dec.beacon;
                        note_beacon(*dec.beacon, real_channel_hz);
                        // A decoded beacon's BSSID is a real,
                        // zero-ambiguity 48-bit MAC - use it as
                        // the master-list's primary key rather
                        // than fingerprint-cluster matching
                        // (see wifi_master.hpp's file header).
                        fp_mac = dec.beacon->bssid;
                        packet_identity = dec.beacon;
                        // Persist verified identity even if RF extraction fails its gates.
                        master_key = dec.beacon->bssid;
                    }
                    if (dec.preamble_found && !dec.sync_symbols.empty()) {
                        fp_opt = wifi_fingerprint::extract_dsss_fingerprint(real_channel_hz,
                                                                             dec);
                    }
                }

                // Anonymous RF clusters stay separate from claimed transmitter identities.
                // Their asynchronous cluster assignment is visible in the master list;
                // packet rows do not invent a cluster key before the consumer assigns it.
                if (fp_opt && !fp_opt->gated_out && !ofdm_decode.fcs_valid &&
                    !(dsss_decode && dsss_decode->fcs_valid)) {
                    enrolment_batch.anonymous.push_back({result.mod == wifi::ModClass::DSSS ? "DSSS" : "OFDM",
                        real_channel_hz, bw_hz, packet_ts, *fp_opt,
                        wifi_master::enrolment_observation_key(rec.run_id,rec.radio_session,rec.capture_seq,b.start,result.mod==wifi::ModClass::DSSS?"DSSS":"OFDM")});
                }

                // Security monitor input: every FCS-valid MPDU from
                // either chain, with its capture position and RF
                // context. submit() never blocks; a full queue drops
                // and the loss is reported downstream.
                {
                    const wifi::DsssDecodeResult* d = dsss_decode && dsss_decode->fcs_valid ? &*dsss_decode : nullptr;
                    const bool ofdm_ok = result.mod == wifi::ModClass::OFDM && ofdm_decode.fcs_valid;
                    if (d || ofdm_ok) {
                        auto ev = wifi_security::make_frame_event(
                            rec, b.start, b.length, d ? "DSSS" : "OFDM", d ? 1 : ofdm_decode.rate_mbps,
                            d && dsss_policy.security_only, d ? d->mpdu : ofdm_decode.mpdu, true);
                        if (d ? d->rf.has_value() : ofdm_decode.rf.has_value())
                            wifi_security::attach_rf_features(ev, d ? *d->rf : *ofdm_decode.rf);
                        ev.power_db = power_db;
                        ev.bandwidth_hz = bw_hz;
                        ev.duration_us = duration_s * 1e6;
                        ev.confidence = result.confidence;
                        if (fp_opt) {
                            ev.fp_cfo_ppm = fp_opt->cfo_ppm;
                            ev.fp_dc_dbc = fp_opt->dc_dbc;
                            ev.fp_snr_db = fp_opt->snr_db;
                            ev.fp_evm_pct = fp_opt->evm_pct;
                            ev.fp_sync_corr = fp_opt->sync_corr;
                            if (result.mod == wifi::ModClass::OFDM) {
                                ev.fp_irr_db = fp_opt->irr_db;
                                ev.fp_iq_eps = fp_opt->iq_eps;
                                ev.fp_iq_phi_deg = fp_opt->iq_phi_deg;
                            }
                            if (fp_opt->gated_out) ev.fp_gate_reason = fp_opt->gate_reason;
                        }
                        enrolment_batch.observations.push_back({ev, fp_opt});
                        if (wifi_frame_observer_) { try { wifi_frame_observer_(ev); } catch (...) {} }
                        if (security_->submit(std::move(ev))) ++rec.events_submitted;
                        else ++rec.events_rejected_by_queue;
                    }
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
                if (result.mod == wifi::ModClass::OFDM) {
                    row.decode_status = ofdm_decode.status;
                    if (ofdm_decode.sig && ofdm_decode.sig->crc_ok) {
                        row.phy_format = ofdm_decode.sig->format;
                        row.network_hint = network_hint(*ofdm_decode.sig, real_channel_hz);
                    }
                    row.ofdm_rate_mbps = ofdm_decode.rate_mbps;
                    row.psdu_length = ofdm_decode.psdu_length;
                    row.fcs_valid = ofdm_decode.fcs_valid;
                } else if (dsss_decode) {
                    row.fcs_valid = dsss_decode->fcs_valid;
                    row.security_decode_only = dsss_policy.security_only;
                    if (row.identity) row.decode_status = "Decoded DSSS beacon/probe response";
                    else if (dsss_policy.security_only && dsss_decode->beacon)
                        row.decode_status = "FCS-valid beacon/probe response (short burst; identity not recorded)";
                    else row.decode_status = dsss_decode->status;
                } else {
                    row.decode_status = "Identity not decoded";
                }
                // Name the frame type of any FCS-valid MPDU, from
                // either receive chain. Bytes without a valid FCS
                // are never interpreted.
                {
                    const std::vector<uint8_t>* mpdu = nullptr;
                    if (result.mod == wifi::ModClass::OFDM && ofdm_decode.fcs_valid) mpdu = &ofdm_decode.mpdu;
                    if (dsss_decode && dsss_decode->fcs_valid) mpdu = &dsss_decode->mpdu;
                    if (mpdu) {
                        auto frame = wifi_security::parse_mac_frame(mpdu->data(), mpdu->size());
                        row.frame_type = wifi_security::frame_type_label(frame);
                    }
                }
                row.master_key = std::move(master_key);
                row.enrolment_source=wifi_master::enrolment_observation_key(rec.run_id,rec.radio_session,rec.capture_seq,b.start,row.modulation);
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
            rec.processed = true;
            rec.processing_s = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                             processing_start).count();
            // Optional add-on gets read-only IQ after all existing packet work.
            // It must return promptly; failures never change existing monitor decisions.
            if (wifi_iq_observer_) { try { wifi_iq_observer_(cap, rec); } catch (...) {} }
            enrolment_batch.capture = rec;
            wifi_master_.submit(std::move(enrolment_batch));
            security_->submit(std::move(rec));
            auto srcs = wifi::find_beacon_sources(burst_starts_s, burst_powers_db);
            source_count = std::max(source_count, int(srcs.size()));
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
            // Additive tail call, after existing security/identity work. Use
            // the raw ranges even when the legacy classifier rejected them.
            if (cyclo_inputs && ci < cyclo_inputs->size() && cyclo_shadow_.enabled()) {
                const auto& input = (*cyclo_inputs)[ci];
                const auto hints = cyclo::collect_burst_hints(bursts.size(), input.prefix,
                    bursts.size() >= WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE, [&](std::size_t i) {
                        return cyclo::SampleTile{bursts[i].start, bursts[i].length};
                    });
                const auto plan=cyclo::select_sample_tiles(input.prefix,hints).plan;
                const auto protocols=cyclo::collect_wifi_protocol_observations(bursts.size(),input.prefix,plan,
                    cyclo::wifi_fact_context(input.context),[&](std::size_t i) {
                        return cyclo::wifi_fact_from_existing_decode(bursts[i],works[i],cap.size(),actual_rate,real_channel_hz);
                    });
                cyclo_shadow_.try_submit(cap, input.prefix, input.context, hints, protocols);
            }
#endif
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
            const auto& chans = step.band == BAND_WIFI_5G ? wifi_5g_channels() : wifi_2g4_channels();
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

// Runs on the Wi-Fi processing lane. Reproduces, per step, exactly what the
// inline path did: empty-capture records first (they were submitted during
// acquisition, i.e. after the previous step's work and before this step's),
// then the step's processing. A step whose captures all failed only submits
// its records, matching the inline `continue`. For Wi-Fi the cycle-wide
// detections vector is always empty at step start (each step flushes and
// clears it), so a local one is equivalent.
void Scanner::run_wifi_lane_job(WifiStepJob& job) {
    if (job.cycle_end) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.cycle_count = job.cycle_count;
        status_.last_overflow = job.last_overflow;
        return;
    }
    if (job.observer_only) {
        if (wifi_iq_observer_)
            for (size_t i = 0; i < job.captures.size() && i < job.capture_records.size(); ++i) {
                try { wifi_iq_observer_(job.captures[i], job.capture_records[i]); } catch (...) {}
            }
        retain_captures(job, job.capture_records);
        return;
    }
    // Processing moves each record into the security monitor; keep a copy of
    // the provenance for retained captures first.
    std::vector<wifi_security::CaptureRecord> records_for_retention;
    if (iq_retention()) records_for_retention = job.capture_records;
    for (auto& r : job.empty_records) security_->submit(std::move(r));
    if (!job.captures.empty()) {
        std::vector<Detection> detections;
        process_step_captures(job.step, job.band, job.threshold, job.actual_rate, job.captures,
                              job.capture_records, detections
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                              , &job.cyclo_inputs
#endif
                              );
    }
    retain_captures(job, records_for_retention);
}

void Scanner::retain_captures(WifiStepJob& job, const std::vector<wifi_security::CaptureRecord>& records) {
    std::lock_guard<std::mutex> lock(retention_mutex_);
    if (!iq_retention_) return;
    for (size_t i = 0; i < job.captures.size() && i < records.size(); ++i) {
        retained_.push_back({records[i],
                             std::make_shared<const std::vector<std::complex<float>>>(std::move(job.captures[i]))});
        while (retained_.size() > iq_retention_) retained_.pop_front();
    }
}

void Scanner::set_iq_retention(size_t captures) {
    std::lock_guard<std::mutex> lock(retention_mutex_);
    iq_retention_ = std::min<size_t>(captures, 64);
    while (retained_.size() > iq_retention_) retained_.pop_front();
}

size_t Scanner::iq_retention() const {
    std::lock_guard<std::mutex> lock(retention_mutex_);
    return iq_retention_;
}

std::optional<Scanner::RetainedCapture> Scanner::retained_capture(uint64_t capture_seq) const {
    std::lock_guard<std::mutex> lock(retention_mutex_);
    for (const auto& r : retained_)
        if (r.record.capture_seq == capture_seq) return r;
    return std::nullopt;
}

std::vector<uint64_t> Scanner::retained_capture_seqs() const {
    std::lock_guard<std::mutex> lock(retention_mutex_);
    std::vector<uint64_t> out;
    for (const auto& r : retained_) out.push_back(r.record.capture_seq);
    return out;
}

double Scanner::now_steady_s() { return now_seconds(); }

void Scanner::set_coordinated_schedule(std::optional<coord::Schedule> schedule) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    coordinated_schedule_ = std::move(schedule);
    ++coordinated_schedule_version_;
}

std::optional<coord::Schedule> Scanner::coordinated_schedule() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return coordinated_schedule_;
}

namespace {
double default_wifi_rate(const DeviceProfile& profile, std::optional<double> override_hz) {
    return override_hz.value_or(
        std::min(std::max(WIFI_2G4_SAMPLE_RATE_HZ, WIFI_5G_SAMPLE_RATE_HZ), profile.max_sample_rate_hz));
}
}  // namespace

coord::Schedule Scanner::effective_coordinated_schedule() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (coordinated_schedule_) return *coordinated_schedule_;
    return coord::default_schedule(default_wifi_rate(device_profile(device_type_), wifi_sample_rate_hz_));
}

CoordinatedStatus Scanner::coordinated_status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return coordinated_status_;
}

std::deque<coord::VisitRecord> Scanner::coordinated_visits() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return coordinated_visits_;
}

// Sub-GHz lane: the same energy-segment processing a sub-GHz scan step gets,
// then LoRa decoding on each LoRa channel derived from this one capture. Runs
// on a SerialLane, so nothing may escape (an exception would terminate).
void Scanner::run_subghz_visit_job(SubGhzVisitJob& job) {
    try {
        std::vector<wifi_security::CaptureRecord> no_records;  // sub-GHz steps have none
        process_step_captures(job.step, BAND_SUB_GHZ, job.threshold, job.actual_rate, job.captures, no_records,
                              coordinated_subghz_detections_);
    } catch (...) {
    }
    try {
        const auto& wide = job.captures.front();
        const double half_span = coord::usable_half_span(job.actual_rate);
        // The widest LoRa hypothesis; narrower ones are decimated from it in
        // decode_lora_iq(), exactly as for the 500 kHz LoRa listen capture.
        const double lora_rate = *std::max_element(LORA_LISTEN_BW_LIST_HZ.begin(), LORA_LISTEN_BW_LIST_HZ.end());
        const double ratio = job.actual_rate / lora_rate;
        if (ratio >= 1 && std::abs(ratio - std::round(ratio)) < 1e-9) {
            const size_t decim = size_t(std::llround(ratio));
            std::vector<double> channels;
            for (double f : LORA_LISTEN_CHANNELS_HZ)
                if (std::abs(f - job.step.center_hz) + lora_rate / 2 <= half_span) channels.push_back(f);
            std::vector<std::future<std::vector<std::complex<float>>>> derived;
            for (double f : channels)
                derived.push_back(std::async(std::launch::async, [&wide, &job, f, lora_rate] {
                    return derive_channel(wide, job.actual_rate, f - job.step.center_hz, lora_rate);
                }));
            for (size_t i = 0; i < channels.size(); ++i) {
                std::vector<std::complex<float>> iq = derived[i].get();
                LoraCapture provenance;
                provenance.sample_rate_hz = lora_rate;
                provenance.requested_sample_rate_hz = lora_rate;
                provenance.requested_center_hz = channels[i];
                provenance.requested_duration_s = double(iq.size()) / lora_rate;
                provenance.host_start_unix_s = job.host_start_unix_s;
                provenance.requested_gain_db = job.gain_db;
                provenance.device_args = job.device_args;
                provenance.antenna = job.antenna;
                provenance.overflow = job.overflow;
                provenance.run_id = security_run_id_;
                provenance.radio_session = job.radio_session;
                provenance.capture_seq = ++lora_security_capture_seq_;
                // Output sample k is input sample k*decim (channelize.hpp), so
                // device time is unchanged and sample indices scale by decim.
                CaptureTiming t = job.timing;
                t.requested_samples /= decim;
                for (auto& o : t.overflows) o.at_sample /= decim;
                // UHD RX convention: centre = actual_rf - actual_dsp, so a channel
                // centred higher by (channel - step centre) has a smaller DSP shift.
                t.actual_dsp_hz -= channels[i] - job.step.center_hz;
                provenance.timing = t;
                decode_lora_iq(iq, provenance, job.threshold, coordinated_subghz_detections_, /*energy_segments=*/false);
            }
        }
    } catch (...) {
    }
    if (job.cycle_end) {
        registry_lora_.update_cycle(coordinated_subghz_detections_, now_seconds());
        coordinated_subghz_detections_.clear();
    }
}

void Scanner::run_coordinated(const DeviceProfile& profile, std::optional<double> wifi_rate_override) {
    const double wifi_rate = default_wifi_rate(profile, wifi_rate_override);
    const auto lower_priority = [] { (void)setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), WIFI_LANE_NICE); };
    coordinated_subghz_detections_.clear();
    // Lanes are declared after everything their jobs touch and are drained
    // (destroyed) before this function returns, so no job outlives the mode.
    SerialLane<WifiStepJob> wifi_lane([this](WifiStepJob& j) { run_wifi_lane_job(j); }, lower_priority);
    SerialLane<SubGhzVisitJob> sub_lane([this](SubGhzVisitJob& j) { run_subghz_visit_job(j); }, lower_priority);
    // One job running plus one queued per lane; a third visit is dropped and
    // counted rather than making acquisition wait.
    constexpr size_t kLaneOutstanding = 2;

    coord::Schedule schedule;
    uint64_t version = ~uint64_t(0);
    std::vector<size_t> cursors;
    std::vector<double> first_wrap_s;         // per slot, -1 until the cursor first wraps
    std::vector<uint64_t> first_wrap_visits;  // slot visit count at that wrap
    uint64_t cycle = 0, visit_seq = 0;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        coordinated_status_ = CoordinatedStatus{};
        coordinated_status_.active = true;
        coordinated_visits_.clear();
        status_.cycle_count = 0;
    }
    std::fprintf(stderr, "Coordinated mode: started\n");
    const auto still_coordinated = [this] {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return active_band_ == BAND_COORDINATED;
    };
    const auto publish_visit = [this](const coord::VisitRecord& r) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (!coordinated_visits_.empty() && coordinated_visits_.back().visit_seq == r.visit_seq) coordinated_visits_.back() = r;
        else coordinated_visits_.push_back(r);
        while (coordinated_visits_.size() > 160) coordinated_visits_.pop_front();
        if (r.in_progress) coordinated_status_.current = r;
        else coordinated_status_.current.reset();
    };

    while (!stop_flag_.load() && still_coordinated()) {
        double threshold;
        std::string conflict;
        bool new_schedule = false;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            threshold = threshold_db_;
            if (wifi_fixed_channel_) conflict = "A fixed Wi-Fi channel is set; clear it to run coordinated mode.";
            else if (lora_lock_freq_) conflict = "A LoRa frequency lock is set; clear it to run coordinated mode.";
            if (coordinated_schedule_version_ != version) {
                version = coordinated_schedule_version_;
                schedule = coordinated_schedule_.value_or(coord::default_schedule(wifi_rate));
                new_schedule = true;
            }
            if (gain_dirty_) {
                sdr_->set_gain(gain_db_);
                gain_dirty_ = false;
            }
        }
        const std::string error = coord::validate(schedule, std::max(profile.max_sample_rate_hz, wifi_rate));
        if (new_schedule) {
            // A schedule change is a new configuration: cursors and measured
            // sweeps restart; earlier visit records stay as history.
            cursors.assign(schedule.slots.size(), 0);
            first_wrap_s.assign(schedule.slots.size(), -1.0);
            first_wrap_visits.assign(schedule.slots.size(), 0);
            std::lock_guard<std::mutex> lock(status_mutex_);
            coordinated_status_.config_hash = coord::schedule_hash(schedule);
            coordinated_status_.estimate = coord::estimate(schedule);
            coordinated_status_.slots.clear();
            for (const auto& slot : schedule.slots) {
                CoordinatedSlotStatus s;
                s.band = slot.band;
                s.enabled = slot.enabled;
                s.list_size = slot.visits.size();
                coordinated_status_.slots.push_back(s);
            }
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            coordinated_status_.conflict = conflict;
            coordinated_status_.config_error = error;
            if (!conflict.empty() || !error.empty()) {
                status_.active_band = BAND_COORDINATED;
                status_.active_step_label = conflict.empty() ? "Coordinated: invalid schedule" : "Coordinated: refused (conflict)";
            }
        }
        if (!conflict.empty() || !error.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // Plan the whole cycle first so the last sub-GHz visit can commit the
        // sub-GHz registry (one commit per cycle, as the sub-GHz mode does).
        struct Planned { size_t slot, visit; bool last_of_slot; };
        std::vector<Planned> planned;
        std::vector<bool> wrapped(schedule.slots.size(), false);
        for (size_t si = 0; si < schedule.slots.size(); ++si) {
            const size_t before = cursors[si];
            const auto idx = coord::next_visits(schedule.slots[si], cursors[si]);
            const size_t n = schedule.slots[si].visits.size();
            wrapped[si] = !idx.empty() && before + idx.size() >= n;
            for (size_t k = 0; k < idx.size(); ++k) planned.push_back({si, idx[k], k + 1 == idx.size()});
        }

        const double cycle_start = now_seconds();
        bool abandoned = false;
        bool any_overflow = false;
        for (const auto& p : planned) {
            if (stop_flag_.load() || !still_coordinated()) { abandoned = true; break; }
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                if (coordinated_schedule_version_ != version || wifi_fixed_channel_ || lora_lock_freq_) { abandoned = true; }
            }
            if (abandoned) break;
            const coord::Slot& slot = schedule.slots[p.slot];
            const coord::Visit& v = slot.visits[p.visit];
            coord::VisitRecord rec;
            rec.cycle = cycle;
            rec.visit_seq = ++visit_seq;
            rec.band = v.band;
            rec.label = v.label;
            rec.kind = v.kind;
            rec.center_hz = v.center_hz;
            rec.rate_hz = v.sample_rate_hz;
            rec.usable_half_span_hz = coord::usable_half_span(v.sample_rate_hz);
            rec.start_s = now_seconds();
            rec.in_progress = true;
            publish_visit(rec);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.active_band = v.band;
                status_.active_step_label = v.label;
            }
            const double host_start = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const double capture_s = slot.dwell_s + schedule.preroll_s;
            CaptureResult cr = sdr_->capture_detailed(v.center_hz, v.sample_rate_hz, capture_s, schedule.settle_s,
                                                      /*retune_always=*/true);
            rec.end_s = now_seconds();
            rec.in_progress = false;
            const double actual_rate = cr.sample_rate_hz;
            if (std::isfinite(actual_rate) && actual_rate > 0) {
                rec.rate_hz = actual_rate;
                rec.usable_half_span_hz = coord::usable_half_span(actual_rate);
                rec.acquired_s = double(cr.samples.size()) / actual_rate;
            }
            rec.valid = !cr.samples.empty() && !cr.overflow && !cr.timing.timed_out && !cr.timing.exception;
            any_overflow = any_overflow || cr.overflow;
            note_capture_health(!cr.samples.empty());
            const ScanStep step{v.band, v.center_hz, v.sample_rate_hz, v.label};
            bool admitted = false;
            if (v.kind == coord::VisitKind::SubGhz) {
                if (!cr.samples.empty()) {
                    SubGhzVisitJob job;
                    job.step = step;
                    job.threshold = threshold;
                    job.actual_rate = actual_rate;
                    job.host_start_unix_s = host_start;
                    job.gain_db = gain();
                    job.timing = cr.timing;
                    job.overflow = cr.overflow;
                    job.radio_session = radio_session_;
                    job.device_args = profile.device_args;
                    job.antenna = profile.antenna;
                    // Commit the registry with the slot's last visit; a dropped
                    // last visit leaves detections for the next commit.
                    job.cycle_end = p.last_of_slot;
                    job.captures.push_back(std::move(cr.samples));
                    admitted = sub_lane.try_push(job, kLaneOutstanding);
                }
            } else {
                WifiStepJob job;
                job.step = step;
                job.band = v.band;
                job.threshold = threshold;
                job.actual_rate = actual_rate;
                job.observer_only = v.kind == coord::VisitKind::DroneOnly;
                auto record = new_capture_record(step, profile, v.sample_rate_hz, capture_s, cr);
                rec.capture_seq = record.capture_seq;  // links detector evidence back to this visit
                if (v.kind == coord::VisitKind::WifiChannel) {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    status_.wifi_requested_rate_hz = v.sample_rate_hz;
                    status_.wifi_actual_rate_hz = !cr.samples.empty() && std::isfinite(actual_rate) && actual_rate > 0
                        ? std::optional<double>(actual_rate) : std::nullopt;
                }
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                if (!job.observer_only && !cr.samples.empty() && cyclo_shadow_.enabled()) {
                    cyclo::CaptureContext context;
                    context.band_ghz = v.band == BAND_WIFI_2G4 ? 2 : 5;
                    context.radio_session = record.radio_session;
                    context.capture_sequence = record.capture_seq;
                    context.submission_epoch = cyclo_shadow_.epoch();
                    context.sample_rate_hz = cr.sample_rate_hz;
                    context.capture_center_hz = v.center_hz;
                    context.source_samples = cr.samples.size();
                    context.device_time_valid = cr.timing.device_time_valid;
                    context.first_device_time_ns = cr.timing.device_time_ns;
                    context.capture_overflow = cr.overflow;
                    context.timed_out = cr.timing.timed_out;
                    const auto prefix = cyclo::continuous_capture_prefix(cr.timing, cr.samples.size(), cr.overflow);
                    job.cyclo_inputs[0] = {context, prefix};
                }
#endif
                if (cr.samples.empty()) {
                    // Empty capture: still coverage for the security monitor,
                    // submitted through the lane so records stay in order.
                    if (!job.observer_only) job.empty_records.push_back(std::move(record));
                } else {
                    job.capture_records.push_back(std::move(record));
                    job.captures.push_back(std::move(cr.samples));
                }
                const bool has_iq = !job.captures.empty();
                if (has_iq || !job.empty_records.empty())
                    admitted = wifi_lane.try_push(job, kLaneOutstanding) && has_iq;
            }
            rec.analysed = admitted;
            publish_visit(rec);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                auto& s = coordinated_status_.slots[p.slot];
                ++s.visits;
                if (rec.valid) { ++s.valid_visits; s.acquired_s += rec.acquired_s; }
                if (admitted) { ++s.analysed_visits; s.analysed_s += rec.acquired_s; }
                else if (rec.acquired_s > 0) ++s.lane_drops;
                s.cursor = cursors[p.slot];
                s.last_visit_end_s = rec.end_s;
                if (cr.overflow) ++coordinated_status_.overflows;
                if (rec.acquired_s == 0) ++coordinated_status_.empty_captures;
                if (p.last_of_slot && wrapped[p.slot]) {
                    // Wrap points fall 3 or 4 cycles apart for 14 centres at
                    // 4 per cycle, so average from the first wrap onwards.
                    if (first_wrap_s[p.slot] < 0) {
                        first_wrap_s[p.slot] = rec.end_s;
                        first_wrap_visits[p.slot] = s.visits;
                    } else if (s.visits > first_wrap_visits[p.slot]) {
                        s.last_sweep_s = (rec.end_s - first_wrap_s[p.slot]) * double(s.list_size) /
                                         double(s.visits - first_wrap_visits[p.slot]);
                    }
                }
            }
        }
        if (abandoned) continue;  // restarts with the new schedule / exits on mode change
        ++cycle;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            coordinated_status_.cycle = cycle;
            coordinated_status_.last_cycle_s = now_seconds() - cycle_start;
            status_.cycle_count = int(cycle);
            status_.last_overflow = any_overflow;
        }
        // Same sustained-RX-stall recovery as the band modes.
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

    wifi_lane.close();
    sub_lane.close();
    const auto ws = wifi_lane.stats();
    const auto ss = sub_lane.stats();
    std::fprintf(stderr, "Coordinated mode: stopped after %llu cycles; Wi-Fi lane %llu jobs, sub-GHz lane %llu jobs\n",
                 static_cast<unsigned long long>(cycle), static_cast<unsigned long long>(ws.jobs_done),
                 static_cast<unsigned long long>(ss.jobs_done));
    coordinated_subghz_detections_.clear();
    std::lock_guard<std::mutex> lock(status_mutex_);
    coordinated_status_.active = false;
    coordinated_status_.current.reset();
}

void Scanner::run() {
    {
        // Run-scoped lane status starts clean even if connecting fails below.
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.wifi_lane_enabled = false;
        status_.wifi_lane_wait_s = 0;
        status_.wifi_lane_high_water = 0;
        status_.wifi_requested_rate_hz = 0;
        status_.wifi_actual_rate_hz.reset();
    }
    std::optional<double> initial_gain;
    SdrDeviceType type;
    std::optional<double> wifi_rate_override;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        initial_gain = gain_db_;
        gain_dirty_ = false;
        type = device_type_;
        wifi_rate_override = wifi_sample_rate_hz_;
    }
    DeviceProfile profile = device_profile(type);
    if (!connect_sdr(profile, initial_gain)) return;

    // Re-evaluate overlap for the requested rate, then check UHD's actual
    // rate too: a coerced rate must not silently exceed the two-step budget.
    const auto lane_rate_fits = [](double rate) {
        return std::isfinite(rate) && rate > 0 &&
            2.0L * SUB_CAPTURES_PER_STEP * std::ceil(rate * SUB_CAPTURE_DURATION_S) *
                sizeof(std::complex<float>) <= WIFI_LANE_MAX_IQ_BYTES;
    };
    std::optional<SerialLane<WifiStepJob>> wifi_lane;
    {
        const double wifi_rate = wifi_rate_override.value_or(
            std::min(std::max(WIFI_2G4_SAMPLE_RATE_HZ, WIFI_5G_SAMPLE_RATE_HZ), profile.max_sample_rate_hz));
        const char* env = std::getenv("RFMON_WIFI_LANE");
        const bool env_off = env && std::string(env) == "0";
        const bool budget_ok = lane_rate_fits(wifi_rate);
        if (budget_ok && !env_off) {
            wifi_lane.emplace([this](WifiStepJob& j) { run_wifi_lane_job(j); },
                              [] {
                                  // Per-thread on Linux; inherited by the threads it creates.
                                  (void)setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), WIFI_LANE_NICE);
                              });
        }
        std::fprintf(stderr, "Wi-Fi processing lane: %s\n",
                     wifi_lane ? "enabled" : env_off ? "disabled (RFMON_WIFI_LANE=0)" : "disabled (IQ budget)");
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.wifi_lane_enabled = wifi_lane.has_value();
        status_.wifi_lane_wait_s = 0;
    }

    std::string last_band_seen;
    int band_cycle_count = 0;

    bool next_wifi_5g = false;
    while (!stop_flag_.load()) {
        std::string band;
        double threshold;
        std::optional<double> locked_freq;
        std::optional<int> fixed_channel;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            band = active_band_;
            threshold = threshold_db_;
            locked_freq = lora_lock_freq_;
            fixed_channel = wifi_fixed_channel_;
            if (gain_dirty_) {
                sdr_->set_gain(gain_db_);
                gain_dirty_ = false;
            }
        }
        const std::string requested_band = band;
        if (band == BAND_COORDINATED) {
            // Explicit coordinated mode owns its own lanes; finish any queued
            // band-mode Wi-Fi work first so the two never run together.
            if (wifi_lane) wifi_lane->wait_idle();
            run_coordinated(profile, wifi_rate_override);
            last_band_seen = requested_band;
            band_cycle_count = 0;
            continue;
        }
        if (band == BAND_WIFI_BOTH) {
            band = next_wifi_5g ? BAND_WIFI_5G : BAND_WIFI_2G4;
            next_wifi_5g = !next_wifi_5g;
            fixed_channel.reset(); // Both always sweeps both complete channel plans.
        } else next_wifi_5g = false;
        if (requested_band != last_band_seen) {
            band_cycle_count = 0;
            last_band_seen = requested_band;
        }
        // Sub-GHz/LoRa never overlaps Wi-Fi processing: finish every queued
        // Wi-Fi step (and its status marker) before a non-Wi-Fi cycle starts.
        const bool wifi_cycle = band == BAND_WIFI_2G4 || band == BAND_WIFI_5G;
        if (wifi_lane && !wifi_cycle) wifi_lane->wait_idle();

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
        // Fixed-channel Wi-Fi: keep only that channel's step. One step is
        // then one cycle, and its consecutive captures skip the retune.
        bool fixed_invalid = false;
        if (fixed_channel && (band == BAND_WIFI_2G4 || band == BAND_WIFI_5G)) {
            std::vector<ScanStep> only;
            for (const auto& st : plan)
                if (nearest_wifi_channel(band, st.center_hz - WIFI_CHANNEL_CAPTURE_OFFSET_HZ) == *fixed_channel)
                    only.push_back(st);
            if (only.empty()) fixed_invalid = true;
            else plan = std::move(only);
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            const bool wifi = band == BAND_WIFI_2G4 || band == BAND_WIFI_5G;
            status_.wifi_fixed_channel = wifi && !fixed_invalid ? fixed_channel : std::nullopt;
            status_.wifi_fixed_channel_invalid = wifi && fixed_invalid;
        }

        for (const auto& step : plan) {
            if (stop_flag_.load()) break;
            {
                // Mode changed mid-cycle: abandon the rest of this
                // cycle's steps and let the outer loop pick up the new
                // band fresh next iteration.
                std::lock_guard<std::mutex> lock(config_mutex_);
                if (active_band_ != requested_band || (requested_band != BAND_WIFI_BOTH && wifi_fixed_channel_ != fixed_channel)) break;
            }

            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.active_band = band;
                status_.active_step_label = step.label;
            }

            const bool wifi_step = step.band == BAND_WIFI_2G4 || step.band == BAND_WIFI_5G;
            // Explicit Wi-Fi requests bypass the historical transport default.
            // LoRa scan/listen rates retain their independent existing settings.
            double request_rate = wifi_step && wifi_rate_override ? *wifi_rate_override :
                std::min(step.sample_rate_hz, profile.max_sample_rate_hz);
            std::vector<std::vector<std::complex<float>>> captures;
            // Wi-Fi only: one coverage record per non-empty capture, kept
            // index-aligned with `captures`. Empty/failed captures are
            // submitted immediately - they are coverage too, just with
            // nothing sampled.
            bool lane_step = wifi_step && wifi_lane.has_value();
            std::vector<wifi_security::CaptureRecord> capture_records;
            std::vector<wifi_security::CaptureRecord> empty_records;  // lane steps: submitted by the lane
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
            CycloCaptureInputs cyclo_inputs{};
#endif
            double actual_rate = request_rate;
            for (int i = 0; i < SUB_CAPTURES_PER_STEP; ++i) {
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                const auto cyclo_capture_epoch = cyclo_shadow_.enabled() ? cyclo_shadow_.epoch() : 0;
#endif
                // Wi-Fi: consecutive captures on the same frequency skip the
                // retune and settle delay (the LO has not moved), which was
                // pure dead time between sub-captures. Sub-GHz/LoRa keep the
                // historical always-retune behaviour.
                CaptureResult cr = sdr_->capture_detailed(step.center_hz, request_rate, SUB_CAPTURE_DURATION_S,
                                                          RETUNE_SETTLE_S, /*retune_always=*/!wifi_step);
                actual_rate = cr.sample_rate_hz;
                if (lane_step && !lane_rate_fits(actual_rate)) {
                    wifi_lane->close();
                    const auto stats = wifi_lane->stats();
                    wifi_lane.reset();
                    lane_step = false;
                    // Earlier empty captures in this step were deferred to the
                    // lane. It is drained; deliver them before current input.
                    for (auto& record : empty_records) security_->submit(std::move(record));
                    empty_records.clear();
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    status_.wifi_lane_enabled = false;
                    status_.wifi_lane_wait_s = stats.producer_wait_s_total;
                    status_.wifi_lane_high_water = stats.high_water;
                }
                if (wifi_step) {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    status_.wifi_requested_rate_hz = request_rate;
                    status_.wifi_actual_rate_hz = !cr.samples.empty() && std::isfinite(actual_rate) && actual_rate > 0
                        ? std::optional<double>(actual_rate) : std::nullopt;
                }
                any_overflow = any_overflow || cr.overflow;
                if (wifi_step) {
                    auto rec = new_capture_record(step, profile, request_rate, SUB_CAPTURE_DURATION_S, cr);
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                    if (!cr.samples.empty() && captures.size() < cyclo_inputs.size() && cyclo_shadow_.enabled()) {
                        cyclo::CaptureContext context;
                        context.band_ghz = step.band == BAND_WIFI_2G4 ? 2 : 5;
                        context.radio_session = rec.radio_session;
                        context.capture_sequence = rec.capture_seq;
                        context.submission_epoch = cyclo_capture_epoch;
                        context.sample_rate_hz = cr.sample_rate_hz;
                        context.capture_center_hz = step.center_hz;
                        context.source_samples = cr.samples.size();
                        context.device_time_valid = cr.timing.device_time_valid;
                        context.first_device_time_ns = cr.timing.device_time_ns;
                        context.capture_overflow = cr.overflow;
                        context.timed_out = cr.timing.timed_out;
                        // Never phase-concatenate across an overflow. Only the
                        // known prefix before the first gap can be copied.
                        const auto prefix = cyclo::continuous_capture_prefix(cr.timing, cr.samples.size(), cr.overflow);
                        cyclo_inputs[captures.size()] = {context, prefix};
                    }
#endif
                    if (cr.samples.empty()) {
                        if (lane_step) empty_records.push_back(std::move(rec));
                        else security_->submit(std::move(rec));
                    } else {
                        capture_records.push_back(std::move(rec));
                    }
                }
                if (!cr.samples.empty()) captures.push_back(std::move(cr.samples));
            }
            note_capture_health(!captures.empty());
            if (lane_step) {
                // Hand the whole step to the lane and go straight on to the
                // next capture. push() waits (holding no lock) only if the
                // previous step is still being processed; the radio is idle
                // between captures, so waiting loses nothing.
                WifiStepJob job;
                job.step = step;
                job.band = band;
                job.threshold = threshold;
                job.actual_rate = actual_rate;
                job.captures = std::move(captures);
                job.capture_records = std::move(capture_records);
                job.empty_records = std::move(empty_records);
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                job.cyclo_inputs = cyclo_inputs;
#endif
                const double waited = wifi_lane->push(std::move(job), 1);
                const size_t high_water = wifi_lane->stats().high_water;
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.wifi_lane_wait_s += waited;
                status_.wifi_lane_high_water = high_water;
                continue;
            }
            if (captures.empty()) continue;

            process_step_captures(step, band, threshold, actual_rate, captures, capture_records, detections
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                                  , &cyclo_inputs
#endif
                                  );
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
                    // A locked LoRa cycle has no wideband step to publish this.
                    status_.active_band = band;
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

        if (wifi_lane && wifi_cycle) {
            // Published by the lane after this cycle's last step is committed,
            // so "cycle completed" still means processed, as before.
            WifiStepJob marker;
            marker.cycle_end = true;
            marker.cycle_count = band_cycle_count;
            marker.last_overflow = any_overflow;
            wifi_lane->push(std::move(marker), 2);
            const size_t high_water = wifi_lane->stats().high_water;
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.wifi_lane_high_water = high_water;
        } else {
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

    // Drain and join the lane before the radio goes and before stop() stops
    // the security monitor (stop() joins this thread first), so every queued
    // step is processed and nothing is submitted to a stopped monitor.
    if (wifi_lane) {
        wifi_lane->close();
        const auto st = wifi_lane->stats();
        std::fprintf(stderr, "Wi-Fi processing lane: %llu jobs, %llu producer waits (%.2f s total, %.2f s max), high water %zu\n",
                     static_cast<unsigned long long>(st.jobs_done), static_cast<unsigned long long>(st.producer_waits),
                     st.producer_wait_s_total, st.producer_wait_s_max, st.high_water);
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
