// Owns the B210 and a background scan thread. Continuously scans
// whichever band is currently "active" (as set from the UI thread),
// keeping one persistent DeviceRegistry per band so switching modes and
// back doesn't lose an accumulated list - only the active band's
// registry is updated while it's selected; the other two stay frozen.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "registry.hpp"
#include "sdr_capture.hpp"

namespace rfmon {

struct ScannerStatus {
    bool connected = false;
    std::string error;              // non-empty if SDR init failed
    std::string active_band;
    std::string active_step_label;  // which scan step is in flight right now
    int cycle_count = 0;            // cycles completed for the active band since it became active
    bool last_overflow = false;
    // True once several scan steps/LoRa listen calls in a row have come
    // back with zero samples (a real capture failure, not just an
    // ordinary "nothing above threshold" result) - see
    // Scanner::note_capture_health(). Distinct from `connected`: the
    // SDR handle is still alive, but samples have stopped flowing.
    bool rx_stalled = false;
};

// One observed LoRa PHY event (see lora_phy.hpp): either a full decode
// or a bare burst detection. Optional fields are unset for a
// detected-only row (no payload was recovered).
struct LoraPacketRow {
    std::string time;      // HH:MM:SS
    std::string status;    // "decoded" or "detected"
    double freq_mhz;
    int sf;
    double bandwidth_khz;  // which of LORA_LISTEN_BW_LIST_HZ this was found at -
                            // a real hypothesis that matched, not a measurement
    std::optional<int> cr;
    std::optional<int> payload_len;
    std::optional<bool> crc_valid;
    int cfo_bins;
    std::optional<std::string> payload_repr;

    // RF fingerprint (see fingerprint.hpp) - the "stable core" Tier-1
    // parameters plus the SNR they were gated on. Unset when extraction
    // was gated out (e.g. below the SNR floor) or not attempted for
    // this row (only the "detected" path currently extracts one - see
    // run_lora_listen_step()).
    std::optional<double> fp_cfo_ppm;
    std::optional<double> fp_irr_db;
    std::optional<double> fp_iq_eps;
    std::optional<double> fp_iq_phi_deg;
    std::optional<double> fp_dc_dbc;
    std::optional<double> fp_dc_ang_deg;
    std::optional<double> fp_snr_db;
    std::optional<std::string> fp_gate_reason;  // set only when gated out

    // Fit-quality covariates - shown regardless of gate outcome, once
    // actually computed (unset only if gating happened before the fit
    // ever ran, e.g. the SNR floor). A high SNR alone doesn't mean the
    // (SF, BW) hypothesis was correct - these reveal whether the fit
    // actually explains the signal, which is exactly what the
    // LORA_EVM_CEILING_PCT/LORA_SYNC_CORR_FLOOR gate uses them for.
    std::optional<double> fp_evm_pct;
    std::optional<double> fp_sync_corr;
};

class Scanner {
public:
    Scanner();
    ~Scanner();

    void start();  // spawns the background thread and connects to the SDR
    void stop();   // signals the thread to stop and joins it

    void set_active_band(const std::string& band);
    std::string active_band() const;

    // Which physical SDR to connect to. Only takes effect on the next
    // start() - changing it while running does not hot-swap hardware;
    // the caller should stop(), set_device_type(), then start() again
    // (see main.cpp's device selector, which does exactly this).
    void set_device_type(SdrDeviceType type);
    SdrDeviceType device_type() const;

    void set_threshold_db(double db);
    double threshold_db() const;

    void set_gain(std::optional<double> gain_db);  // nullopt = AGC
    std::optional<double> gain() const;

    // Pins the LoRa PHY listen step to one exact frequency every cycle
    // instead of rotating LORA_LISTEN_CHANNELS_HZ - useful when a known
    // real device's configured frequency doesn't fall on that list (see
    // newrocktest/TARANGMINI_LORA_FINDINGS.md for how this comes up in
    // practice). nullopt (default) = cycle all channels as usual.
    void set_lora_lock_freq(std::optional<double> freq_hz);
    std::optional<double> lora_lock_freq() const;

    std::vector<DeviceRow> snapshot(const std::string& band) const;
    ScannerStatus status() const;

    // Only populated while BAND_SUB_GHZ is active - see run()'s LoRa
    // PHY listen sub-loop, which runs alongside the usual energy scan.
    std::vector<LoraPacketRow> lora_packets() const;

private:
    void run();
    // Appends any energy-detected segment(s) found in this same
    // capture to `detections` (see .cpp) - reuses the one capture
    // already taken here for the dedicated codec attempts, rather than
    // requiring a second wideband capture just to keep the Active
    // emitters table populated while locked to one frequency.
    void run_lora_listen_step(double freq_hz, const DeviceProfile& profile, double threshold_db,
                               std::vector<Detection>& detections);
    DeviceRegistry& registry_for(const std::string& band);
    // Attempts to (re)connect sdr_, retrying a few times (X310
    // connections over Ethernet fail intermittently - see run()'s
    // comment). Updates status_.connected/error itself either way.
    // Used both for the initial connect and for the mid-run
    // auto-reconnect triggered by a sustained RX stall.
    bool connect_sdr(const DeviceProfile& profile, std::optional<double> gain);
    // Called after every scan step / LoRa listen attempt with whether it
    // returned any samples at all. Only touched from the background
    // thread (run() and its callees), so rx_fail_streak_ itself needs no
    // locking - only the status_ write it produces does.
    void note_capture_health(bool got_data);

    std::unique_ptr<UsrpCapture> sdr_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};

    mutable std::mutex config_mutex_;
    std::string active_band_ = BAND_WIFI_2G4;
    double threshold_db_ = DEFAULT_DETECTION_THRESHOLD_DB;
    std::optional<double> gain_db_ = DEFAULT_GAIN_DB;
    bool gain_dirty_ = false;
    std::optional<double> lora_lock_freq_;
    SdrDeviceType device_type_ = SdrDeviceType::B210;

    mutable std::mutex status_mutex_;
    ScannerStatus status_;

    DeviceRegistry registry_lora_;
    DeviceRegistry registry_wifi24_;
    DeviceRegistry registry_wifi5_;

    mutable std::mutex lora_log_mutex_;
    std::vector<LoraPacketRow> lora_packet_log_;
    size_t lora_channel_idx_ = 0;

    int rx_fail_streak_ = 0;
    static constexpr int kRxStallThreshold = 3;
};

}  // namespace rfmon
