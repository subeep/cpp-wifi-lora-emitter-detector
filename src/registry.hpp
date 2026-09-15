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
#include <tuple>
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
    // True when a correlator positively identified the modulation (see
    // wifi_phy.hpp), rather than this being a bare energy segment.
    //
    // Needed because both kinds of detection for the same Wi-Fi channel
    // land in the SAME frequency bucket, and the bucket previously kept
    // whichever had the higher peak_db - but those two peak_db values
    // come from different measurements on wildly different scales
    // (estimate_mean_power_db()'s time-domain mean, around -40dB, versus
    // spectrogram_max_db()'s un-normalized |FFT|^2, around +14dB). The
    // energy segment therefore won every time, and the modulation label
    // was silently discarded before it could ever reach the GUI. An
    // identified modulation is strictly more informative than unlabelled
    // energy, so it wins on that basis rather than on an incomparable
    // number.
    bool modulation_confirmed = false;
    // Optional per-source identity WITHIN a frequency bucket, so one
    // Wi-Fi channel can carry several tracked emitters (see Key). Empty
    // means "no per-source identity", which is the case for every
    // sub-GHz/LoRa detection and reproduces the previous
    // one-device-per-bucket behaviour exactly.
    std::string source_id;
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

    // (band, bucket_hz, source_id). The third element lets ONE frequency
    // bucket hold several independently-tracked emitters, which a Wi-Fi
    // channel genuinely needs - six BSSIDs sharing channel 9 is normal,
    // and keying purely by channel centre could only ever represent one
    // of them.
    //
    // source_id is EMPTY for everything that has no per-source identity,
    // which is every sub-GHz/LoRa detection and any Wi-Fi row not
    // attributed to a specific source. An empty id reproduces the old
    // one-device-per-bucket behaviour exactly, so nothing outside the
    // Wi-Fi multi-source path changes.
    //
    // It is deliberately an opaque string rather than a cluster index:
    // the only identity stable enough to persist across sweeps is a
    // decoded BSSID, so this is shaped to carry one.
    using Key = std::tuple<std::string, double, std::string>;

    static Key bucket_key(const std::string& band, double freq_hz,
                           const std::string& source_id = std::string());
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
