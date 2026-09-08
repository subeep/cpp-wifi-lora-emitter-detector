// Tracks currently-active emitters across scan cycles, with expiry.
//
// A "device" here is really a recurring occupied frequency bucket, not
// a verified physical device (energy detection can't distinguish two
// co-channel emitters, and can't extract any real identity like a MAC
// address). Buckets are keyed so the same physical emitter reported by
// different scan steps (e.g. a Wi-Fi channel visible in both
// overlapping capture windows) collapses to one entry.
//
// Thread-safe: update_cycle()/end_cycle() are called from the scanner
// thread, snapshot() is called from the UI thread.

#pragma once

#include <map>
#include <mutex>
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

struct Detection {
    std::string band;
    Segment segment;
    std::string protocol_guess;
};

class DeviceRegistry {
public:
    explicit DeviceRegistry(int expire_cycles = DEFAULT_EXPIRE_CYCLES,
                             double alpha = POWER_EMA_ALPHA);

    // detections gathered across every scan step in ONE full cycle.
    // Multiple raw segments can legitimately land in the same bucket
    // within a single cycle - collapse to one observation so
    // hit_count means "seen in N distinct scan cycles," not "how many
    // raw segments happened to hash to this bucket."
    void update_cycle(const std::vector<Detection>& detections, double now);

    // Age out stale devices - call once per full cycle, after update_cycle().
    void end_cycle();

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
    };

    using Key = std::pair<std::string, double>;  // (band, bucket_hz)

    static Key bucket_key(const std::string& band, double freq_hz);
    void update_one(const Key& key, const Segment& segment, const std::string& protocol_guess,
                    double now);

    mutable std::mutex mutex_;
    std::map<Key, Device> devices_;
    std::map<Key, int> cycles_since_seen_;
    int expire_cycles_;
    double alpha_;
};

}  // namespace rfmon
