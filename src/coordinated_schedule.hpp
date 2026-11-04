// Coordinated three-band schedule (upgradedplan.md §4, milestone P1): plain
// data and pure functions only. Scanner::run() executes a Schedule; the GUI
// previews it with estimate(). Nothing here touches the radio.
//
// Terms (kept distinct on purpose):
//  - band slot: one band's share of a cycle, executed in slot order;
//  - frequency visit: one tuned centre, observed for `dwell_s` of valid time;
//  - pre-roll: extra real samples captured before the dwell, for causal
//    consumers that need warm-up (the ML STFT view needs 8 ms);
//  - settle: retune/settling time, not observation time;
//  - cycle: every enabled slot once; full sweep: every configured centre of
//    one slot once, which may take several cycles (rotating cursor).
// Inference/decoder time is never part of the cycle: slow analysis shows up
// as dropped/unanalysed coverage, not as a longer dwell.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

namespace rfmon::coord {

// What a visit's samples are for. Wi-Fi channel visits feed the existing
// Wi-Fi PHY/security path plus drone/ML observers; drone-only visits feed
// only drone/ML observers (no Wi-Fi channel sits at that centre, so Wi-Fi
// PHY results there would be mislabelled); sub-GHz visits feed the energy
// scan and LoRa decoding on channels derived from the same capture.
enum class VisitKind { SubGhz, WifiChannel, DroneOnly };

inline const char* kind_name(VisitKind k) {
    return k == VisitKind::SubGhz ? "sub-GHz" : k == VisitKind::WifiChannel ? "Wi-Fi channel" : "drone-only";
}

struct Visit {
    std::string band;  // BAND_SUB_GHZ / BAND_WIFI_2G4 / BAND_WIFI_5G
    VisitKind kind = VisitKind::WifiChannel;
    double center_hz = 0;
    double sample_rate_hz = 0;
    std::string label;
};

struct Slot {
    std::string band;
    bool enabled = true;
    int visits_per_cycle = 1;
    double dwell_s = 0.512;
    std::vector<Visit> visits;  // rotated through by a cursor
};

struct Schedule {
    std::vector<Slot> slots;  // executed in this order, then repeated
    double settle_s = RETUNE_SETTLE_S;
    double preroll_s = 0.008;
};

// DJI DroneID 2.4 GHz channel that no Wi-Fi channel capture contains
// (2414.5/2429.5/2444.5/2459.5 MHz fall inside ch 1/4/7/10 captures). The
// capture is offset like the Wi-Fi captures so the signal avoids the DC notch.
constexpr double DRONEID_2399_5_HZ = 2399.5e6;

inline Schedule default_schedule(double wifi_rate_hz) {
    Schedule s;
    Slot sub{BAND_SUB_GHZ, true, 2, 0.512, {}};
    sub.visits.push_back({BAND_SUB_GHZ, VisitKind::SubGhz, SUB_GHZ_CENTER_HZ, SUB_GHZ_SAMPLE_RATE_HZ,
                          "Sub-GHz 863-868 MHz (energy + LoRa)"});
    Slot w24{BAND_WIFI_2G4, true, 4, 0.512, {}};
    w24.visits.push_back({BAND_WIFI_2G4, VisitKind::DroneOnly, DRONEID_2399_5_HZ + WIFI_CHANNEL_CAPTURE_OFFSET_HZ,
                          wifi_rate_hz, "DroneID 2399.5 MHz (drone/ML only)"});
    for (const auto& [ch, f] : wifi_2g4_channels())
        w24.visits.push_back({BAND_WIFI_2G4, VisitKind::WifiChannel, f + WIFI_CHANNEL_CAPTURE_OFFSET_HZ, wifi_rate_hz,
                              "2.4GHz Wi-Fi ch " + std::to_string(ch) + " (" + std::to_string(int(f / 1e6)) + " MHz)"});
    Slot w5{BAND_WIFI_5G, true, 6, 0.512, {}};
    for (const auto& [ch, f] : wifi_5g_channels())
        w5.visits.push_back({BAND_WIFI_5G, VisitKind::WifiChannel, f + WIFI_CHANNEL_CAPTURE_OFFSET_HZ, wifi_rate_hz,
                             "5GHz Wi-Fi ch " + std::to_string(ch) + " (" + std::to_string(int(f / 1e6)) + " MHz)"});
    s.slots = {sub, w24, w5};
    return s;
}

// Largest single visit capture kept in host memory (fc32), per visit.
constexpr double MAX_VISIT_IQ_BYTES = 512.0 * 1024 * 1024;

// "" when the schedule can run; otherwise the first reason it cannot.
inline std::string validate(const Schedule& s, double max_rate_hz) {
    if (!std::isfinite(s.settle_s) || s.settle_s < 0 || s.settle_s > 2) return "Settle time must be 0-2 s";
    if (!std::isfinite(s.preroll_s) || s.preroll_s < 0 || s.preroll_s > 0.5) return "Pre-roll must be 0-0.5 s";
    int enabled = 0;
    for (const auto& slot : s.slots) {
        if (!slot.enabled) continue;
        ++enabled;
        const std::string name = slot.band;
        if (slot.band != BAND_SUB_GHZ && slot.band != BAND_WIFI_2G4 && slot.band != BAND_WIFI_5G)
            return "Unknown band slot " + name;
        if (slot.visits.empty()) return name + ": no frequency visits configured";
        if (slot.visits_per_cycle < 1 || slot.visits_per_cycle > 64) return name + ": visits per cycle must be 1-64";
        if (!std::isfinite(slot.dwell_s) || slot.dwell_s < 0.05 || slot.dwell_s > 10) return name + ": dwell must be 0.05-10 s";
        for (const auto& v : slot.visits) {
            if (v.band != slot.band) return name + ": visit " + v.label + " belongs to another band";
            if (!std::isfinite(v.center_hz) || v.center_hz <= 0) return name + ": invalid centre for " + v.label;
            if (!std::isfinite(v.sample_rate_hz) || v.sample_rate_hz <= 0 || v.sample_rate_hz > max_rate_hz)
                return name + ": rate for " + v.label + " is outside this receiver's limit";
            if ((v.kind == VisitKind::SubGhz) != (slot.band == BAND_SUB_GHZ)) return name + ": visit kind does not match band";
            if (v.kind == VisitKind::SubGhz) {
                // LoRa channels are derived by integer decimation to 500 kHz.
                const double ratio = v.sample_rate_hz / 500e3;
                if (ratio < 1 || std::abs(ratio - std::round(ratio)) > 1e-9) return name + ": rate must be a multiple of 500 kHz";
            }
            if ((slot.dwell_s + s.preroll_s) * v.sample_rate_hz * 8 > MAX_VISIT_IQ_BYTES)
                return name + ": one visit would exceed the 512 MiB capture bound";
        }
    }
    if (!enabled) return "Enable at least one band slot";
    return "";
}

// Stable FNV-1a over a canonical text form: identifies the configuration a
// result came from. Changing anything that affects acquisition changes it.
inline std::string schedule_hash(const Schedule& s) {
    std::string text;
    char buf[160];
    std::snprintf(buf, sizeof buf, "settle=%.9g;preroll=%.9g;", s.settle_s, s.preroll_s);
    text += buf;
    for (const auto& slot : s.slots) {
        std::snprintf(buf, sizeof buf, "[%s;%d;%d;%.9g]", slot.band.c_str(), int(slot.enabled), slot.visits_per_cycle, slot.dwell_s);
        text += buf;
        for (const auto& v : slot.visits) {
            std::snprintf(buf, sizeof buf, "(%d;%.3f;%.3f)", int(v.kind), v.center_hz, v.sample_rate_hz);
            text += buf;
        }
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

struct SlotEstimate {
    std::string band;
    bool enabled = false;
    double valid_s = 0;      // dwell time per cycle
    double overhead_s = 0;   // pre-roll + settle per cycle
    int cycles_per_sweep = 0; // worst case: cycles until every configured centre was visited once
    // Average time between visits to the same centre under the rotating
    // cursor: (centres / visits per cycle) x cycle. 14 centres at 4 per cycle
    // revisit every 3.5 cycles on average (alternating 3 and 4).
    double sweep_s = 0;
};
struct Estimate {
    double valid_s = 0, overhead_s = 0, cycle_s = 0;  // excludes host/transport overhead
    std::vector<SlotEstimate> slots;
};

inline Estimate estimate(const Schedule& s) {
    Estimate e;
    for (const auto& slot : s.slots) {
        SlotEstimate se;
        se.band = slot.band;
        se.enabled = slot.enabled && !slot.visits.empty() && slot.visits_per_cycle > 0;
        if (se.enabled) {
            se.valid_s = slot.visits_per_cycle * slot.dwell_s;
            se.overhead_s = slot.visits_per_cycle * (s.preroll_s + s.settle_s);
            se.cycles_per_sweep = int((slot.visits.size() + size_t(slot.visits_per_cycle) - 1) / size_t(slot.visits_per_cycle));
        }
        e.valid_s += se.valid_s;
        e.overhead_s += se.overhead_s;
        e.slots.push_back(se);
    }
    e.cycle_s = e.valid_s + e.overhead_s;
    for (size_t i = 0; i < e.slots.size(); ++i) {
        auto& se = e.slots[i];
        const auto& slot = s.slots[i];
        if (se.enabled) se.sweep_s = double(slot.visits.size()) / slot.visits_per_cycle * e.cycle_s;
    }
    return e;
}

// Indices of the visits this cycle executes for one slot, advancing the
// slot's cursor so the next cycle resumes at the next unvisited centre.
// A list shorter than visits_per_cycle repeats centres within the cycle.
inline std::vector<size_t> next_visits(const Slot& slot, size_t& cursor) {
    std::vector<size_t> out;
    if (!slot.enabled || slot.visits.empty()) return out;
    const size_t n = slot.visits.size();
    cursor %= n;
    for (int i = 0; i < slot.visits_per_cycle; ++i) out.push_back((cursor + size_t(i)) % n);
    cursor = (cursor + size_t(slot.visits_per_cycle)) % n;
    return out;
}

// One completed (or in-progress) visit, as the scanner records it. Times are
// steady-clock seconds (the same clock as now_steady_s()).
struct VisitRecord {
    uint64_t cycle = 0, visit_seq = 0;
    uint64_t capture_seq = 0;  // Wi-Fi/drone visits: the capture record's sequence (0: sub-GHz / none)
    std::string band, label;
    VisitKind kind = VisitKind::WifiChannel;
    double center_hz = 0, rate_hz = 0, usable_half_span_hz = 0;
    double start_s = 0, end_s = 0;  // acquisition start/end
    double acquired_s = 0;          // valid samples received / rate
    bool valid = false;             // samples received, contiguous, no timeout
    bool analysed = false;          // admitted to its processing lane
    bool in_progress = false;
};

inline double usable_half_span(double rate_hz) { return 0.5 * rate_hz * (1.0 - 2 * EDGE_GUARD_FRACTION); }

// Freshness of an entry while the receiver is time-shared (upgradedplan §3.3).
enum class Freshness { ReceivingNow, RecentBandUnobserved, NotReacquired, InsufficientCoverage, Expired };

inline const char* freshness_name(Freshness f) {
    switch (f) {
        case Freshness::ReceivingNow: return "Receiving now";
        case Freshness::RecentBandUnobserved: return "Recent; frequency not observed since";
        case Freshness::NotReacquired: return "Not reacquired on later visit (not proof of departure)";
        case Freshness::InsufficientCoverage: return "Insufficient coverage since last seen";
        case Freshness::Expired: return "Historical / expired";
    }
    return "";
}

// `grace_s` lets a later covering visit's own results arrive before that
// visit counts as a failed reacquisition.
inline Freshness freshness(double frequency_hz, double last_seen_s, double now_s, const std::deque<VisitRecord>& visits,
                           double expire_s, double grace_s = 2.0) {
    if (!(now_s - last_seen_s <= expire_s)) return Freshness::Expired;
    auto covers = [&](const VisitRecord& v) { return std::abs(frequency_hz - v.center_hz) <= v.usable_half_span_hz; };
    bool later_valid = false, later_invalid = false;
    for (const auto& v : visits) {
        if (!covers(v)) continue;
        if (v.in_progress) return Freshness::ReceivingNow;
        if (v.start_s <= last_seen_s || v.end_s + grace_s > now_s) continue;
        (v.valid && v.analysed ? later_valid : later_invalid) = true;
    }
    if (later_valid) return Freshness::NotReacquired;
    if (later_invalid) return Freshness::InsufficientCoverage;
    return Freshness::RecentBandUnobserved;
}

}  // namespace rfmon::coord
