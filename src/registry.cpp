#include "registry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmon {

DeviceRegistry::DeviceRegistry(int expire_cycles, double alpha)
    : expire_cycles_(expire_cycles), alpha_(alpha) {}

DeviceRegistry::Key DeviceRegistry::bucket_key(const std::string& band, double freq_hz) {
    if (band == BAND_SUB_GHZ) {
        double bucket_hz = std::round(freq_hz / 50e3) * 50e3;  // 50 kHz buckets
        return {band, bucket_hz};
    }
    const std::map<int, double>& channels =
        (band == BAND_WIFI_2G4) ? wifi_2g4_channels() : wifi_5g_channels();
    double best = channels.begin()->second;
    double best_dist = std::abs(best - freq_hz);
    for (const auto& [ch, f] : channels) {
        (void)ch;
        double dist = std::abs(f - freq_hz);
        if (dist < best_dist) {
            best_dist = dist;
            best = f;
        }
    }
    return {band, best};
}

void DeviceRegistry::update_cycle(const std::vector<Detection>& detections, double now) {
    std::map<Key, std::pair<Segment, std::string>> best_per_bucket;
    for (const auto& d : detections) {
        Key key = bucket_key(d.band, d.segment.center_hz);
        auto it = best_per_bucket.find(key);
        if (it == best_per_bucket.end() || d.segment.peak_db > it->second.first.peak_db) {
            best_per_bucket[key] = {d.segment, d.protocol_guess};
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, seg_and_protocol] : best_per_bucket) {
        update_one(key, seg_and_protocol.first, seg_and_protocol.second, now);
    }
}

void DeviceRegistry::update_one(const Key& key, const Segment& segment,
                                 const std::string& protocol_guess, double now) {
    double freq_mhz = segment.center_hz / 1e6;
    double bw_khz = segment.bandwidth_hz / 1e3;

    auto it = devices_.find(key);
    if (it == devices_.end()) {
        devices_[key] = Device{key.first,       freq_mhz,        bw_khz, protocol_guess,
                                segment.peak_db, now,             now,    1};
    } else {
        Device& dev = it->second;
        dev.freq_mhz = freq_mhz;
        dev.bandwidth_khz = bw_khz;
        dev.protocol_guess = protocol_guess;
        dev.power_db = (1 - alpha_) * dev.power_db + alpha_ * segment.peak_db;
        dev.last_seen = now;
        dev.hit_count += 1;
    }
    cycles_since_seen_[key] = 0;
}

void DeviceRegistry::end_cycle() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Key> stale;
    for (auto& [key, count] : cycles_since_seen_) {
        count += 1;
        if (count > expire_cycles_) stale.push_back(key);
    }
    for (const auto& key : stale) {
        devices_.erase(key);
        cycles_since_seen_.erase(key);
    }
}

std::vector<DeviceRow> DeviceRegistry::snapshot(double now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceRow> rows;
    rows.reserve(devices_.size());
    for (const auto& [key, dev] : devices_) {
        (void)key;
        rows.push_back(DeviceRow{dev.band, dev.freq_mhz, dev.bandwidth_khz, dev.protocol_guess,
                                  dev.power_db, dev.hit_count, now - dev.first_seen,
                                  now - dev.last_seen});
    }
    std::sort(rows.begin(), rows.end(),
              [](const DeviceRow& a, const DeviceRow& b) { return a.freq_mhz < b.freq_mhz; });
    return rows;
}

}  // namespace rfmon
