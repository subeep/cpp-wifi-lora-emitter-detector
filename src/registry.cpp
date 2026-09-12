#include "registry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmon {

namespace {

// Fixed-tolerance "same physical device" heuristic - a first pass, not
// the source spec's fuller robust-z / stability-weighted comparison
// (that needs multi-session data this project hasn't collected yet -
// see fingerprint.hpp's file header). Compared against a device's
// ROLLING MEDIAN over several readings, never a single raw one: a real
// TarangMini's per-packet IRR/DC swung far wider in practice (roughly
// -4 to -43dB) than the source spec's own "stable, class S" expectation
// (its example template shows a spread under 1dB) - a naive single-
// reading comparison would be unreliable, a several-reading median is
// at least somewhat smoothed.
constexpr double IRR_TOLERANCE_DB = 8.0;
constexpr double DC_DBC_TOLERANCE_DB = 8.0;
constexpr double IQ_EPS_TOLERANCE = 0.02;
constexpr double IQ_PHI_TOLERANCE_DEG = 3.0;
constexpr int MIN_READINGS_TO_MATCH = 3;
constexpr size_t FP_HISTORY_MAX = 10;

bool fingerprints_match(const FingerprintSnapshot& a, const FingerprintSnapshot& b) {
    return std::abs(a.irr_db - b.irr_db) <= IRR_TOLERANCE_DB &&
           std::abs(a.dc_dbc - b.dc_dbc) <= DC_DBC_TOLERANCE_DB &&
           std::abs(a.iq_eps - b.iq_eps) <= IQ_EPS_TOLERANCE &&
           std::abs(a.iq_phi_deg - b.iq_phi_deg) <= IQ_PHI_TOLERANCE_DEG;
}

double median_of(const std::deque<FingerprintSnapshot>& history,
                  double FingerprintSnapshot::*member) {
    std::vector<double> vals;
    vals.reserve(history.size());
    for (const auto& r : history) vals.push_back(r.*member);
    std::sort(vals.begin(), vals.end());
    size_t n = vals.size();
    if (n == 0) return 0.0;
    return (n % 2 == 1) ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
}

}  // namespace

DeviceRegistry::DeviceRegistry(double alpha) : alpha_(alpha) {}

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

std::optional<DeviceRegistry::Key> DeviceRegistry::find_fingerprint_match(
    const std::string& band, const FingerprintSnapshot& fp) const {
    for (const auto& [key, dev] : devices_) {
        if (key.first != band || !dev.fingerprint.has_value()) continue;
        if (dev.fingerprint->n_readings >= MIN_READINGS_TO_MATCH &&
            fingerprints_match(*dev.fingerprint, fp)) {
            return key;
        }
    }
    return std::nullopt;
}

void DeviceRegistry::push_fingerprint_reading(Device& dev, const FingerprintSnapshot& raw) {
    dev.fp_history.push_back(raw);
    if (dev.fp_history.size() > FP_HISTORY_MAX) dev.fp_history.pop_front();

    FingerprintSnapshot med;
    med.cfo_ppm = median_of(dev.fp_history, &FingerprintSnapshot::cfo_ppm);
    med.irr_db = median_of(dev.fp_history, &FingerprintSnapshot::irr_db);
    med.iq_eps = median_of(dev.fp_history, &FingerprintSnapshot::iq_eps);
    med.iq_phi_deg = median_of(dev.fp_history, &FingerprintSnapshot::iq_phi_deg);
    med.dc_dbc = median_of(dev.fp_history, &FingerprintSnapshot::dc_dbc);
    med.dc_ang_deg = median_of(dev.fp_history, &FingerprintSnapshot::dc_ang_deg);
    med.n_readings = int(dev.fp_history.size());
    dev.fingerprint = med;
}

void DeviceRegistry::update_cycle(const std::vector<Detection>& detections, double now) {
    // Non-fingerprinted detections (every WiFi one, and any Sub-GHz
    // energy-only segment with no burst-level fingerprint yet) keep
    // the original pure frequency-bucket behavior, picking the
    // strongest segment per bucket for this cycle.
    std::map<Key, std::pair<Segment, std::string>> best_per_bucket;
    std::vector<const Detection*> fingerprinted;
    for (const auto& d : detections) {
        if (d.fingerprint.has_value()) {
            fingerprinted.push_back(&d);
            continue;
        }
        Key key = bucket_key(d.band, d.segment.center_hz);
        auto it = best_per_bucket.find(key);
        if (it == best_per_bucket.end() || d.segment.peak_db > it->second.first.peak_db) {
            best_per_bucket[key] = {d.segment, d.protocol_guess};
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, seg_and_protocol] : best_per_bucket) {
        update_one(key, seg_and_protocol.first, seg_and_protocol.second, now, nullptr);
    }

    // Fingerprinted detections: try to match an EXISTING device by RF
    // fingerprint similarity first, regardless of which frequency
    // bucket it's nominally in - only falling back to the same
    // bucket-key lookup/creation above when no fingerprint match is
    // found (e.g. the very first few readings of a new device, before
    // it has enough history to match against).
    for (const Detection* dp : fingerprinted) {
        const Detection& d = *dp;
        std::optional<Key> matched = find_fingerprint_match(d.band, *d.fingerprint);
        Key key = matched.value_or(bucket_key(d.band, d.segment.center_hz));
        update_one(key, d.segment, d.protocol_guess, now, &(*d.fingerprint));
    }
}

void DeviceRegistry::update_one(const Key& key, const Segment& segment,
                                 const std::string& protocol_guess, double now,
                                 const FingerprintSnapshot* raw_fp) {
    double freq_mhz = segment.center_hz / 1e6;
    double bw_khz = segment.bandwidth_hz / 1e3;

    auto it = devices_.find(key);
    if (it == devices_.end()) {
        Device dev;
        dev.band = key.first;
        dev.freq_mhz = freq_mhz;
        dev.bandwidth_khz = bw_khz;
        dev.protocol_guess = protocol_guess;
        dev.power_db = segment.peak_db;
        dev.first_seen = now;
        dev.last_seen = now;
        dev.hit_count = 1;
        if (raw_fp) push_fingerprint_reading(dev, *raw_fp);
        devices_.emplace(key, std::move(dev));
    } else {
        Device& dev = it->second;
        // A fingerprint match can point at a device sitting in a
        // different bucket than this reading's own frequency - update
        // to the latest reading's frequency rather than freezing it at
        // whichever bucket the device happened to be created in.
        dev.freq_mhz = freq_mhz;
        dev.bandwidth_khz = bw_khz;
        dev.protocol_guess = protocol_guess;
        dev.power_db = (1 - alpha_) * dev.power_db + alpha_ * segment.peak_db;
        dev.last_seen = now;
        dev.hit_count += 1;
        if (raw_fp) push_fingerprint_reading(dev, *raw_fp);
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
