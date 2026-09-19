// Owns the B210 and a background scan thread. Continuously scans
// whichever band is currently "active" (as set from the UI thread),
// keeping one persistent DeviceRegistry per band so switching modes and
// back doesn't lose an accumulated list - only the active band's
// registry is updated while it's selected; the other two stay frozen.

#pragma once

#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "lora_master.hpp"
#include "lora_observation.hpp"
#include "registry.hpp"
#include "sdr_capture.hpp"
#include "wifi_master.hpp"

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

// One detected Wi-Fi transmission - a single burst found by
// wifi::detect_bursts() and then classified on its own window, rather
// than a whole-capture verdict. FCS-valid DSSS/OFDM beacon/probe responses
// additionally carry decoded identity and the persistent record key.
struct WifiPacketRow {
    std::string time;  // HH:MM:SS
    double freq_mhz = 0.0;
    int channel = 0;
    std::string modulation;  // "DSSS" or "OFDM"
    double power_db = 0.0;
    double bandwidth_khz = 0.0;
    double duration_us = 0.0;
    double confidence = 0.0;  // [0,1], comparable across modulations
    std::optional<wifi::BeaconInfo> identity;  // FCS-valid decoded beacon/probe response
    std::string decode_status;
    int ofdm_rate_mbps = 0;
    size_t psdu_length = 0;
    bool fcs_valid = false;
    std::string master_key;  // persistent BSSID or accepted fingerprint-cluster key

    // RF fingerprint (see wifi_fingerprint.hpp) - unset when extraction
    // wasn't attempted for this row (OFDM without a usable L-LTF range,
    // or a DSSS burst too short to be beacon-plausible - see
    // scanner.cpp's own comment on that scope limit) or was gated out
    // (fp_gate_reason set in that case). irr_db/iq_eps/iq_phi_deg stay
    // unset on every DSSS row even when the rest of the fingerprint
    // passed - see wifi_fingerprint.cpp's own comment on why a
    // real-valued BPSK reference can't resolve those three via this
    // technique, not a gating decision.
    std::optional<double> fp_cfo_ppm;
    std::optional<double> fp_irr_db;
    std::optional<double> fp_iq_eps;
    std::optional<double> fp_iq_phi_deg;
    std::optional<double> fp_dc_dbc;
    std::optional<double> fp_dc_ang_deg;
    std::optional<double> fp_snr_db;
    std::optional<double> fp_evm_pct;
    std::optional<double> fp_sync_corr;
    std::optional<std::string> fp_gate_reason;  // set only when attempted but gated out
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
    // One-shot save of the next LoRa capture, consumed by the scan worker.
    void request_lora_capture_save(const std::string& directory);
    std::string lora_capture_message() const;
    bool lora_capture_pending() const;
    void set_lora_lock_freq(std::optional<double> freq_hz);
    std::optional<double> lora_lock_freq() const;

    // Diagnostic-only: bypasses the SX-reference decoder's sync-word
    // value check (see lora_phy_std.hpp's demodulate() comment for why
    // that gate currently rejects real third-party captures). Off by
    // default - a header_valid=true row seen with this on is verified
    // only by its own checksum, not the sync word; see
    // LoraPacketRow::sync_check_skipped, surfaced in the GUI so this
    // is never mistaken for a fully-verified decode.
    void set_lora_skip_sync_check(bool skip);
    bool lora_skip_sync_check() const;

    std::vector<DeviceRow> snapshot(const std::string& band) const;
    ScannerStatus status() const;

    // Only populated while BAND_SUB_GHZ is active - see run()'s LoRa
    // PHY listen sub-loop, which runs alongside the usual energy scan.
    std::vector<LoraPacketRow> lora_packets() const;

    // Persistent, cross-run LoRa "master emitter" list (see
    // lora_master.hpp) - a separate identity system from registry_lora_
    // above; fed only by non-gated LoRa fingerprint readings, and never
    // forgets anything across restarts.
    std::vector<lora_master::LoraMasterRow> lora_master_snapshot() const;

    // Only populated while a Wi-Fi band is active - one row per
    // individually detected and classified burst.
    std::vector<WifiPacketRow> wifi_packets() const;

    // How many distinct transmitting sources were resolved on each
    // Wi-Fi channel, keyed by channel number (see
    // wifi::find_beacon_sources). This is an inferred LOWER bound from
    // beacon timing, never a decoded device count - co-phased BSSes
    // merge, and virtual/multi-BSSID networks on one radio are
    // physically indistinguishable here by construction.
    std::map<int, int> wifi_source_counts(const std::string& band) const;

    // Persistent, cross-run Wi-Fi "master emitter" list (see
    // wifi_master.hpp) - the Wi-Fi analog of lora_master_snapshot()
    // above, fed by FCS-valid identities and accepted RF readings independently. Keyed
    // by decoded MAC/BSSID when one is available (DSSS and legacy OFDM
    // beacons/probe responses), fingerprint-cluster otherwise.
    std::vector<wifi_master::WifiMasterRow> wifi_master_snapshot() const;
    std::string wifi_storage_error() const { return wifi_master_.storage_error(); }

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
    bool lora_skip_sync_check_ = false;
    SdrDeviceType device_type_ = SdrDeviceType::B210;

    mutable std::mutex status_mutex_;
    ScannerStatus status_;

    DeviceRegistry registry_lora_;
    DeviceRegistry registry_wifi24_;
    DeviceRegistry registry_wifi5_;

    mutable std::mutex lora_log_mutex_;
    std::vector<LoraPacketRow> lora_packet_log_;
    size_t lora_channel_idx_ = 0;
    mutable std::mutex capture_mutex_;
    std::string capture_save_directory_;
    std::string capture_message_;
    bool capture_pending_ = false;

    mutable std::mutex wifi_log_mutex_;
    std::vector<WifiPacketRow> wifi_packet_log_;
    std::map<std::string, std::map<int, int>> wifi_source_counts_;

    lora_master::LoraMasterList lora_master_;
    wifi_master::WifiMasterList wifi_master_;

    int rx_fail_streak_ = 0;
    static constexpr int kRxStallThreshold = 3;
};

}  // namespace rfmon
