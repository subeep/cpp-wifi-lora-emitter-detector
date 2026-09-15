// Persistent, cross-run "master" LoRa emitter list - separate from the
// per-session Active-emitters registry (registry.hpp/.cpp), which
// forgets everything on restart and only keeps a 10-reading rolling
// median. This is the permanent record: every accepted (non-gated-out)
// fingerprint reading a device has ever produced, one on-disk file per
// device, capped at MAX_READINGS_PER_DEVICE readings so a device's
// history can span the many days needed to see real seasonal/
// temperature drift without growing forever - oldest readings are
// dropped first once the cap is hit. A device's own identity (its
// first_seen timestamp, its file) is never deleted automatically -
// only individual old readings get pruned once over the cap.
//
// WiFi has no equivalent yet - explicitly out of scope for now.
//
// Matching (the "simple comparison" placeholder - a real model-based
// approach is expected to replace this later) uses only the
// quadrature/mixer group (irr_db, dc_dbc, iq_eps, iq_phi_deg), never
// cfo_ppm: controlled real-hardware testing this same session showed
// CFO swinging tens of ppm burst-to-burst even for one confirmed
// physical device (most likely receiver-LO variance between capture
// windows, not device identity), so using it here would cause false
// splits. Nothing here ever throws away raw reading data because of a
// matching decision, so a smarter matcher can always re-cluster the
// same underlying history later without needing to re-collect it.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "fingerprint.hpp"

namespace rfmon::lora_master {

constexpr int MAX_READINGS_PER_DEVICE = 1000;
// Only compact (rewrite-trim) a device's file once it has grown this
// far past the cap, not on every single new reading - keeps the common
// case a cheap append instead of a full-file rewrite every time.
constexpr int COMPACT_TRIGGER_READINGS = MAX_READINGS_PER_DEVICE + 100;
// How many of a device's most recent readings the matcher's median is
// computed over - deliberately much smaller than the full 1000-reading
// history, so genuine slow parameter drift (the whole reason for
// keeping that much history) doesn't itself prevent matching a new
// reading against a device whose baseline has since moved on.
constexpr int MATCH_MEDIAN_WINDOW = 20;

// Same tolerances as the per-session registry's fingerprint matcher
// (registry.cpp) - carried over rather than re-derived, since this is
// the same class of "simple comparison" placeholder applied to a
// longer, persisted history instead of a 10-reading in-memory one.
constexpr double IRR_TOLERANCE_DB = 8.0;
constexpr double DC_DBC_TOLERANCE_DB = 8.0;
constexpr double IQ_EPS_TOLERANCE = 0.02;
constexpr double IQ_PHI_TOLERANCE_DEG = 3.0;

// One accepted fingerprint reading, exactly as computed - every field
// the fingerprint produces gets stored (source spec Rule 2: store the
// measurement and its conditions), even cfo_ppm despite it being
// excluded from matching, since it's still useful to look back on.
struct LoraMasterReading {
    int64_t ts = 0;  // absolute Unix epoch seconds - must survive process restarts, unlike steady_clock
    double freq_hz = 0.0;
    int sf = 0;
    double bw_hz = 0.0;
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

// One GUI row's worth of summary for one persisted device - the full
// reading history stays on disk/in memory rather than being copied out
// wholesale on every snapshot() call.
struct LoraMasterRow {
    int device_id = 0;
    std::string device_id_str;  // "LORA-0001" - formatted once here, not in main.cpp
    int64_t first_seen_ts = 0;
    int64_t last_seen_ts = 0;
    double last_freq_hz = 0.0;
    int last_sf = 0;
    double last_bw_hz = 0.0;
    int reading_count = 0;
    LoraMasterReading latest;  // most recent reading's own parameters, for the quick-glance columns
};

class LoraMasterList {
public:
    // `dir` is created (including parents) if missing, then scanned
    // for existing device files to reload - see the .cpp for the
    // on-disk format (one NDJSON file per device: a permanent meta
    // line followed by reading lines).
    explicit LoraMasterList(std::string dir);

    // Records one already-gated-in (fp.gated_out == false) fingerprint
    // reading - the caller is responsible for only calling this with a
    // passing fingerprint, since a gated-out one isn't device data,
    // it's noise (already logged separately - see
    // fingerprint.hpp's append_fingerprint_record()). Matches against
    // existing devices (see this file's header) or creates a new one,
    // then appends to that device's in-memory history and on-disk
    // file, pruning/compacting if it just crossed
    // COMPACT_TRIGGER_READINGS. `ts` is the absolute Unix epoch second
    // this reading was taken.
    void record_reading(double freq_hz, int sf, double bw_hz,
                         const fingerprint::LoraFingerprint& fp, int64_t ts);

    // Sorted by last_seen_ts, most recently seen first.
    std::vector<LoraMasterRow> snapshot() const;

private:
    struct Device {
        int id = 0;
        int64_t first_seen_ts = 0;
        int64_t last_seen_ts = 0;
        double last_freq_hz = 0.0;
        int last_sf = 0;
        double last_bw_hz = 0.0;
        std::deque<LoraMasterReading> readings;  // newest at the back
    };

    // Index into devices_ whose recent-reading median falls within
    // tolerance of the given quadrature/mixer params, or -1 if none
    // does (a new device should be created). Caller must hold mutex_.
    int find_match(double irr_db, double dc_dbc, double iq_eps, double iq_phi_deg) const;

    std::string device_path(int id) const;
    void load_all_devices();
    void append_reading_to_disk(int id, const LoraMasterReading& r) const;
    // Rewrites dev's whole file (meta line + every current reading) to
    // a temp file, then renames it over the original - rename() is
    // atomic on the same filesystem, so a crash mid-write never leaves
    // a half-written file in the real path; worst case, the pending
    // trim just didn't happen yet.
    void compact_device_file(const Device& dev) const;

    std::string dir_;
    mutable std::mutex mutex_;
    std::vector<Device> devices_;
    int next_id_ = 1;
};

}  // namespace rfmon::lora_master
