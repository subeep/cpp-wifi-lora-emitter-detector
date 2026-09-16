// Persistent, cross-run "master" Wi-Fi emitter list - the Wi-Fi analog
// of lora_master.hpp/.cpp, but keyed differently: unlike LoRa, most
// 802.11 traffic carries a real, decodable, zero-ambiguity 48-bit MAC/
// BSSID, so a decoded MAC is the PRIMARY identity key here whenever one
// is available, with the RF fingerprint doing the LoRa-style fuzzy
// clustering only as a fallback for traffic that never decodes one -
// see the WiFi fingerprinting research plan's identity_strategy for
// the full reasoning (don't port LoRa's fingerprint-is-the-identity
// model verbatim).
//
// MAC availability today: only DSSS beacons decode a BSSID at all
// (wifi_frame.cpp's parse_beacon(), already wired into scanner.cpp for
// the per-session registry) - general MPDU/frame-control address
// decode for non-beacon DSSS frames, and any OFDM decode whatsoever,
// do not exist in this project (wifi_frame.hpp's own header: scoping
// OFDM decode "would roughly triple" the project). So in practice
// today, only beacon-carrying DSSS bursts get a MAC-keyed device;
// everything else - all OFDM, and DSSS data frames - falls through to
// fingerprint-cluster keying, exactly like LoRa. The two key TYPES
// share one device table so a future MAC-decode extension (e.g. OFDM
// MAC decode landing elsewhere) needs no schema change here.
//
// Matching for a fingerprint-cluster device reuses lora_master.hpp's
// exact algorithm (tolerance gate on the quadrature/mixer group, then
// nearest-by-L1-distance tie-break, cfo_ppm excluded) and its exact
// tolerance NUMBERS as a starting point - NOT yet validated against
// real Wi-Fi hardware. This matters more here than it did copying
// registry.cpp's numbers into lora_master.hpp originally: the research
// plan flags that WiFi's own literature (PARADIS) ranks CFO as the
// single most effective radiometric feature, the OPPOSITE of LoRa's
// own real-hardware finding that CFO swings too much burst-to-burst to
// use for matching - excluding it here is a documented, deliberately
// conservative placeholder pending that WiFi-specific test, not a
// settled conclusion carried over from LoRa's evidence.
//
// Retention also starts at LoRa's exact numbers (1000 readings/device,
// lazy compaction) as the simplest testable choice, NOT the research
// plan's more elaborate time-bucketed scheme - WiFi's much higher
// packet rate could in principle fill this far faster than LoRa's did,
// but no real capture volume exists yet to size a bucketing scheme
// against. Revisit (see this file's comment where MAX_READINGS_PER_DEVICE
// is defined) once real field data shows whether it's actually a
// problem.
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
};

class WifiMasterList {
public:
    // `dir` is created (including parents) if missing, then scanned
    // for existing device files to reload - see the .cpp for the
    // on-disk format (one NDJSON file per device, filename derived
    // from the device key: a permanent meta line followed by reading
    // lines - same shape as lora_master.hpp's, only the key changes).
    explicit WifiMasterList(std::string dir);

    // Records one already-gated-in (fp.gated_out == false) fingerprint
    // reading. `mac`, when present, is used as the EXACT device key
    // (no fuzzy matching - a decoded MAC is ground truth); when absent,
    // falls back to lora_master.hpp-style fuzzy fingerprint-cluster
    // matching. `ts` is the absolute Unix epoch second this reading
    // was taken.
    void record_reading(std::optional<std::string> mac, const std::string& phy, double channel_hz,
                         double bandwidth_hz, const wifi_fingerprint::WifiFingerprint& fp,
                         int64_t ts);

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
    void append_reading_to_disk(const std::string& key, const WifiMasterReading& r) const;
    void compact_device_file(const Device& dev) const;

    std::string dir_;
    mutable std::mutex mutex_;
    std::vector<Device> devices_;
    int next_fp_id_ = 1;
};

}  // namespace rfmon::wifi_master
