// Tracks currently-active emitters across scan cycles. Permanent once
// seen - see this file's own history for why: an earlier version aged
// entries out after a few cycles, which stopped making sense once a
// locked LoRa listener could cycle every ~2s (an entry only survived
// ~8s without a fresh hit) and defeated the entire point of
// fingerprinting a device in the first place (a row that disappears
// isn't something you can accumulate a fingerprint against).
//
// A "device" here is normally really a recurring occupied frequency
// bucket, not a verified physical device (bare energy detection can't
// distinguish two co-channel emitters, or extract any real identity
// like a MAC address) - EXCEPT for a Sub-GHz/LoRa entry that has
// accumulated an RF fingerprint (see fingerprint.hpp), where matching
// is done by fingerprint similarity first, frequency bucket only as a
// fallback - see find_fingerprint_match() in registry.cpp for why and
// its tolerances. WiFi has no fingerprint data (yet) and keeps using
// pure frequency-bucket matching, unchanged.
//
// Thread-safe: update_cycle() is called from the scanner thread,
// snapshot() from the UI thread.

#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "detector.hpp"

namespace rfmon {

struct DeviceRow {
    std::string band;
    double freq_mhz;
    double bandwidth_khz;
    std::string protocol_guess;
    double power_db;
    int hit_count;
    double age_s;
    double last_seen_s_ago;
};

// Tier-1 "stable core" RF fingerprint parameters (see fingerprint.hpp)
// - duplicated here as a plain small struct rather than pulling in a
// dependency on fingerprint.hpp, matching how this project generally
// prefers a small duplicated shape over a cross-module dependency for
// something this stable. `n_readings` is only meaningful on a Device's
// *stored* rolling-median snapshot (see push_fingerprint_reading()) -
// a single fresh Detection's raw reading leaves it at its default.
struct FingerprintSnapshot {
    double cfo_ppm = 0.0;
    double irr_db = 0.0;
    double iq_eps = 0.0;
    double iq_phi_deg = 0.0;
    double dc_dbc = 0.0;
    double dc_ang_deg = 0.0;
    int n_readings = 0;
};

struct Detection {
    std::string band;
    Segment segment;
    std::string protocol_guess;
    // Set only for a LoRa "detected" burst that cleared the SNR gate
    // (see fingerprint.hpp) - a single raw reading, not yet aggregated.
    std::optional<FingerprintSnapshot> fingerprint;
};

class DeviceRegistry {
public:
    explicit DeviceRegistry(double alpha = POWER_EMA_ALPHA);

    // detections gathered across every scan step in ONE full cycle.
    // Multiple raw segments can legitimately land in the same bucket
    // within a single cycle - collapse to one observation so
    // hit_count means "seen in N distinct scan cycles," not "how many
    // raw segments happened to hash to this bucket."
    void update_cycle(const std::vector<Detection>& detections, double now);

    // Thread-safe snapshot, sorted by frequency.
    std::vector<DeviceRow> snapshot(double now) const;

private:
    struct Device {
        std::string band;
        double freq_mhz;
        double bandwidth_khz;
        std::string protocol_guess;
        double power_db;
        double first_seen;
        double last_seen;
        int hit_count = 1;
        std::optional<FingerprintSnapshot> fingerprint;  // rolling-median snapshot
        std::deque<FingerprintSnapshot> fp_history;      // raw readings, capped
    };

    using Key = std::pair<std::string, double>;  // (band, bucket_hz)

    static Key bucket_key(const std::string& band, double freq_hz);
    // Searches every existing device in `band` with an established
    // fingerprint (>= MIN_READINGS_TO_MATCH readings) for one whose
    // rolling snapshot is within tolerance of `fp` - see registry.cpp.
    // Must be called with mutex_ already held.
    std::optional<Key> find_fingerprint_match(const std::string& band,
                                               const FingerprintSnapshot& fp) const;
    void push_fingerprint_reading(Device& dev, const FingerprintSnapshot& raw);
    void update_one(const Key& key, const Segment& segment, const std::string& protocol_guess,
                    double now, const FingerprintSnapshot* raw_fp);

    mutable std::mutex mutex_;
    std::map<Key, Device> devices_;
    double alpha_;
};

}  // namespace rfmon
