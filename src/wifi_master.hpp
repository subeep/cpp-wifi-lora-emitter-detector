// Persistent Wi-Fi network identities and provisional RF fingerprint clusters.
// FCS-valid beacon/probe-response observations are recorded independently of RF
// quality gates. A BSSID identifies a network interface, not necessarily a unique
// physical radio; several BSSIDs may share one transmitter. OUI names identify
// registry assignees; WPS strings are self-advertised hints.
//
// Schema 2 retains legacy meta + reading lines and adds typed identity snapshots.
// Old files load unchanged and acquire metadata at the next successful decode.
// Known SSIDs and WPS hints survive later frames that omit them, with separate
// observation timestamps. RF history remains bounded; identity counts and metadata
// survive compaction. Unknown RF clusters are never automatically merged into MACs.
//
// Fingerprint matching is unchanged: LoRa-derived placeholder tolerances and CFO
// exclusion still require independent Wi-Fi validation. No physical-device-count
// guarantee is implied by cluster IDs. Only DSSS currently supplies decoded MACs.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "wifi_fingerprint.hpp"

namespace rfmon::wifi_master {

// Same starting numbers as lora_master.hpp - see this file's header
// for why WiFi's much higher packet rate might call for something
// different once real data exists to size it against.
constexpr int MAX_READINGS_PER_DEVICE = 1000;
constexpr int COMPACT_TRIGGER_READINGS = MAX_READINGS_PER_DEVICE + 100;
constexpr int MATCH_MEDIAN_WINDOW = 20;

// Same tolerances as lora_master.hpp's fingerprint-cluster matcher, as
// a starting point - see this file's header for why cfo_ppm's
// exclusion specifically needs its own WiFi validation rather than
// inheriting LoRa's real-hardware justification unexamined.
constexpr double IRR_TOLERANCE_DB = 8.0;
constexpr double DC_DBC_TOLERANCE_DB = 8.0;
constexpr double IQ_EPS_TOLERANCE = 0.02;
constexpr double IQ_PHI_TOLERANCE_DEG = 3.0;

// One accepted fingerprint reading, exactly as computed - every field
// stored (source spec Rule 2: store the measurement and its
// conditions), even cfo_ppm despite its exclusion from matching, since
// it's still useful to look back on, and irr_db/iq_eps/iq_phi_deg even
// on a DSSS reading where they're structurally unset (0.0) - see
// wifi_fingerprint.cpp's own comment on why DSSS can't identify those
// three via this technique.
struct WifiMasterReading {
    int64_t ts = 0;  // absolute Unix epoch seconds - survives restarts
    double channel_hz = 0.0;
    double bandwidth_hz = 0.0;
    std::string phy;  // "OFDM" or "DSSS"
    double cfo_ppm = 0.0;
    double irr_db = 0.0;
    double iq_eps = 0.0;
    double iq_phi_deg = 0.0;
    double dc_dbc = 0.0;
    double dc_ang_deg = 0.0;
    double snr_db = 0.0;
    double evm_pct = 0.0;
    double sync_corr = 0.0;
};

// One GUI row's worth of summary for one persisted device.
struct WifiMasterRow {
    std::string device_key;  // a MAC "aa:bb:cc:dd:ee:ff", or "WIFI-FP-0001"
    bool key_is_mac = false;
    int64_t first_seen_ts = 0;
    int64_t last_seen_ts = 0;
    double last_channel_hz = 0.0;
    std::string last_phy;
    int reading_count = 0;
    WifiMasterReading latest;
    std::optional<wifi::BeaconInfo> identity;
    int64_t identity_ts = 0;
    int64_t ssid_seen_ts = 0;
    int64_t wps_seen_ts = 0;
    std::string ssid_source, wps_source;
    uint64_t identity_count = 0;
    double monitored_channel_hz = 0;
    std::string vendor;
    std::string vendor_source;

};

class WifiMasterList {
public:
    // `dir` is created (including parents) if missing, then scanned
    // for existing device files to reload - see the .cpp for the
    // on-disk format (one NDJSON file per device, filename derived
    // from the device key: a permanent meta line followed by reading
    // lines - same shape as lora_master.hpp's, only the key changes).
    explicit WifiMasterList(std::string dir);

    // Returns the persistent key; rejects gated-out readings. Records an accepted fingerprint
    // reading. `mac`, when present, is used as the EXACT device key
    // (no fuzzy matching - a decoded MAC is ground truth); when absent,
    // falls back to lora_master.hpp-style fuzzy fingerprint-cluster
    // matching. `ts` is the absolute Unix epoch second this reading
    // was taken.
    std::string record_reading(std::optional<std::string> mac, const std::string& phy, double channel_hz,
                         double bandwidth_hz, const wifi_fingerprint::WifiFingerprint& fp,
                         int64_t ts);

    // A verified identity observation is independent of fingerprint acceptance.
    // Hidden SSIDs do not erase a previously learned name. No automatic merge
    // of fingerprint clusters into BSSIDs: their equivalence is not established.
    std::string record_identity(const wifi::BeaconInfo& info, double monitored_channel_hz, int64_t ts);
    std::string storage_error() const;

    // Sorted by last_seen_ts, most recently seen first.
    std::vector<WifiMasterRow> snapshot() const;

private:
    struct Device {
        std::string key;
        bool key_is_mac = false;
        int64_t first_seen_ts = 0;
        int64_t last_seen_ts = 0;
        double last_channel_hz = 0.0;
        std::string last_phy;
        std::optional<wifi::BeaconInfo> identity;
        int64_t identity_ts = 0, ssid_seen_ts = 0, wps_seen_ts = 0;
        std::string ssid_source, wps_source;
        uint64_t identity_count = 0;
        double monitored_channel_hz = 0;
        size_t identity_lines = 0;
        std::deque<WifiMasterReading> readings;  // newest at the back
    };

    // Index into devices_ whose recent-reading median falls within
    // tolerance of the given quadrature/mixer params, restricted to
    // fingerprint-cluster (non-MAC) devices, or -1 if none does.
    // Caller must hold mutex_.
    int find_fp_match(double irr_db, double dc_dbc, double iq_eps, double iq_phi_deg) const;
    // Exact match by key, restricted to MAC devices, or -1. Caller
    // must hold mutex_.
    int find_mac_match(const std::string& mac) const;

    std::string device_path(const std::string& key) const;
    void load_all_devices();
    void append_line(const std::string& key, const std::string& line);
    void compact_device_file(const Device& dev);
    std::string identity_line(const Device& dev) const;
    std::string storage_error_;

    std::string dir_;
    mutable std::mutex mutex_;
    std::vector<Device> devices_;
    int next_fp_id_ = 1;
};

}  // namespace rfmon::wifi_master
