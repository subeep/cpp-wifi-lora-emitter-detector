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
#include <utility>
#include <vector>

#include "config.hpp"
#include "drones/droneid_packet.hpp"

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
    std::string name;           // display only (several slots can share a band); not in the hash
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

// Drone bands the schedule covers end to end: every frequency in them falls inside some visit's
// usable span (usable_half_span() below), not only the Wi-Fi channels.
struct DroneBand { double low_hz, high_hz; const char* name; };
inline constexpr DroneBand drone_bands[] = {
    {2400e6, 2483.5e6, "2.4 GHz"}, {5150e6, 5250e6, "5.2 GHz"}, {5725e6, 5850e6, "5.8 GHz"}};

// How far a documented DroneID channel may sit from the capture centre for the native worker to
// decode it there (tools/drone_native_worker.cpp: |channel - centre| <= rate x 0.45 - 5 MHz).
inline double droneid_decode_reach_hz(double rate_hz) { return rate_hz * 0.45 - 5e6; }
// Below this rate the gaps would take dozens of extra visits; the default schedule adds none.
inline constexpr double MIN_GAP_FILL_RATE_HZ = 10e6;

inline double usable_half_span(double rate_hz) { return 0.5 * rate_hz * (1.0 - 2 * EDGE_GUARD_FRACTION); }

// Parts of [low, high] outside every centre's usable span (gaps under 1 kHz are ignored).
inline std::vector<std::pair<double, double>> uncovered(double low, double high, const std::vector<double>& centres,
                                                        double half_span) {
    std::vector<std::pair<double, double>> spans;
    for (double c : centres) spans.push_back({c - half_span, c + half_span});
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<double, double>> gaps;
    double x = low;
    for (const auto& [a, b] : spans) {
        if (b <= x) continue;
        if (a > x) gaps.push_back({x, std::min(a, high)});
        x = std::max(x, b);
        if (x >= high) break;
    }
    if (x < high) gaps.push_back({x, high});
    gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [](const auto& g) { return g.second - g.first < 1e3; }), gaps.end());
    return gaps;
}

// Drone-only visits that close the drone bands for a schedule whose ordinary visits sit at
// `centres`: first a visit for each documented DroneID channel no centre can decode (at
// channel + the usual capture offset), then evenly spaced visits across each remaining gap.
struct DroneGapVisits { std::vector<Visit> droneid, gaps; };
inline DroneGapVisits drone_gap_visits(double rate_hz, std::vector<double> centres) {
    DroneGapVisits out;
    if (!(rate_hz >= MIN_GAP_FILL_RATE_HZ)) return out;
    const double reach = droneid_decode_reach_hz(rate_hz);
    const double dc_guard = std::max(rate_hz * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    const auto band_of = [](double hz) { return hz < 3e9 ? BAND_WIFI_2G4 : BAND_WIFI_5G; };
    char text[96];
    if (reach >= WIFI_CHANNEL_CAPTURE_OFFSET_HZ) {
        std::vector<double> channels(std::begin(drone::droneid::channels_24), std::end(drone::droneid::channels_24));
        channels.insert(channels.end(), std::begin(drone::droneid::channels_58), std::end(drone::droneid::channels_58));
        for (double ch : channels) {
            const bool reached = std::any_of(centres.begin(), centres.end(), [&](double c) {
                return std::abs(ch - c) <= reach && std::abs(ch - c) > dc_guard;
            });
            if (reached) continue;
            const double centre = ch + WIFI_CHANNEL_CAPTURE_OFFSET_HZ;
            std::snprintf(text, sizeof text, "DroneID %.1f MHz (drone/ML only)", ch / 1e6);
            out.droneid.push_back({band_of(ch), VisitKind::DroneOnly, centre, rate_hz, text});
            centres.push_back(centre);
        }
    }
    const double half = usable_half_span(rate_hz);
    for (const auto& band : drone_bands)
        for (const auto& [a, b] : uncovered(band.low_hz, band.high_hz, centres, half)) {
            const int n = int(std::ceil((b - a) / (2 * half) - 1e-9));
            for (int k = 0; k < n; ++k) {
                const double centre = a + (b - a) * (2 * k + 1) / (2.0 * n);
                std::snprintf(text, sizeof text, "%s gap %.1f MHz (drone/ML only)", band.name, centre / 1e6);
                out.gaps.push_back({band_of(centre), VisitKind::DroneOnly, centre, rate_hz, text});
            }
        }
    return out;
}

// The fewest of `candidates` whose usable spans cover [low, high], taken greedily from the low
// end (each time the candidate that covers the current edge and reaches furthest), plus the
// nearest candidate for any documented DroneID channel near the band that none of those can
// decode. Parts no candidate covers are skipped (left to gap visits). Indices into candidates.
inline std::vector<std::size_t> covering_subset(const std::vector<double>& candidates, double low, double high,
                                                double rate_hz) {
    const double half = usable_half_span(rate_hz);
    std::vector<std::size_t> out;
    double x = low;
    while (x < high) {
        std::optional<std::size_t> best;
        double next = high;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const double c = candidates[i];
            if (c - half <= x && c + half > x && (!best || c > candidates[*best])) best = i;
            if (c - half > x) next = std::min(next, c - half);
        }
        if (!best) { x = next; continue; }  // nothing covers x: on to the next candidate's span
        out.push_back(*best);
        x = candidates[*best] + half;
    }
    const double reach = droneid_decode_reach_hz(rate_hz);
    const double dc_guard = std::max(rate_hz * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    const auto decodes = [&](double ch, double c) { return std::abs(ch - c) <= reach && std::abs(ch - c) > dc_guard; };
    std::vector<double> channels(std::begin(drone::droneid::channels_24), std::end(drone::droneid::channels_24));
    channels.insert(channels.end(), std::begin(drone::droneid::channels_58), std::end(drone::droneid::channels_58));
    for (double ch : channels) {
        if (ch < low - 2 * half || ch > high + 2 * half) continue;
        if (std::any_of(out.begin(), out.end(), [&](std::size_t i) { return decodes(ch, candidates[i]); })) continue;
        std::optional<std::size_t> nearest;
        for (std::size_t i = 0; i < candidates.size(); ++i)
            if (decodes(ch, candidates[i]) &&
                (!nearest || std::abs(ch - candidates[i]) < std::abs(ch - candidates[*nearest])))
                nearest = i;
        if (nearest) out.push_back(*nearest);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Drone-first layout ("split C"), executed in this order every cycle:
//   Sub-GHz          865.5 MHz                                      1 visit per cycle
//   2.4 GHz sweep    the fewest 2.4 GHz centres that cover 2400-2483.5 MHz and decode every
//                    2.4 GHz DroneID channel (at 20 MSps: DroneID 2399.5, Wi-Fi ch 1, 4, 7,
//                    10, 13) plus the band-edge gap visit                4
//   2.4 GHz Wi-Fi    the other 2.4 GHz Wi-Fi channels (Wi-Fi decoding)   1
//   5.8 GHz          Wi-Fi ch 149-165 and the 5.8 GHz DroneID visits     4
//   5.2 GHz          Wi-Fi ch 36-48                                      1
//   5.8 GHz gaps     drone-only visits closing 5725-5850 MHz             2
//   5.2 GHz gaps     drone-only visits closing 5150-5250 MHz             1
// Every Wi-Fi channel is still visited centred; drone time goes first to 2.4 and 5.8 GHz, where
// the drones seen here transmit. Below MIN_GAP_FILL_RATE_HZ the original three slots are kept
// (sub-GHz 2, 2.4 GHz 4, 5 GHz 6 per cycle) with no drone-only additions beyond 2399.5 MHz.
inline Schedule default_schedule(double wifi_rate_hz) {
    Schedule s;
    const auto wifi_visit = [&](const char* band, const char* prefix, int ch, double f) {
        return Visit{band, VisitKind::WifiChannel, f + WIFI_CHANNEL_CAPTURE_OFFSET_HZ, wifi_rate_hz,
                     std::string(prefix) + std::to_string(ch) + " (" + std::to_string(int(f / 1e6)) + " MHz)"};
    };
    Visit sub_visit{BAND_SUB_GHZ, VisitKind::SubGhz, SUB_GHZ_CENTER_HZ, SUB_GHZ_SAMPLE_RATE_HZ,
                    "Sub-GHz 863-868 MHz (energy + LoRa)"};
    std::vector<Visit> v24{{BAND_WIFI_2G4, VisitKind::DroneOnly, DRONEID_2399_5_HZ + WIFI_CHANNEL_CAPTURE_OFFSET_HZ,
                            wifi_rate_hz, "DroneID 2399.5 MHz (drone/ML only)"}};
    for (const auto& [ch, f] : wifi_2g4_channels()) v24.push_back(wifi_visit(BAND_WIFI_2G4, "2.4GHz Wi-Fi ch ", ch, f));
    std::vector<Visit> v52, v58;
    for (const auto& [ch, f] : wifi_5g_channels())
        (f < 5.5e9 ? v52 : v58).push_back(wifi_visit(BAND_WIFI_5G, "5GHz Wi-Fi ch ", ch, f));

    if (!(wifi_rate_hz >= MIN_GAP_FILL_RATE_HZ)) {
        Slot w5{BAND_WIFI_5G, true, 6, 0.512, v52, "5 GHz Wi-Fi"};
        w5.visits.insert(w5.visits.end(), v58.begin(), v58.end());
        s.slots = {Slot{BAND_SUB_GHZ, true, 2, 0.512, {sub_visit}, "Sub-GHz"},
                   Slot{BAND_WIFI_2G4, true, 4, 0.512, v24, "2.4 GHz"}, w5};
        return s;
    }

    std::vector<double> c24;
    for (const auto& v : v24) c24.push_back(v.center_hz);
    const auto pick = covering_subset(c24, drone_bands[0].low_hz, drone_bands[0].high_hz, wifi_rate_hz);
    Slot sweep{BAND_WIFI_2G4, true, 4, 0.512, {}, "2.4 GHz sweep"};
    Slot rest24{BAND_WIFI_2G4, true, 1, 0.512, {}, "2.4 GHz Wi-Fi"};
    for (std::size_t i = 0; i < v24.size(); ++i)
        (std::binary_search(pick.begin(), pick.end(), i) ? sweep : rest24).visits.push_back(v24[i]);
    Slot s58{BAND_WIFI_5G, true, 4, 0.512, v58, "5.8 GHz"};
    Slot s52{BAND_WIFI_5G, true, 1, 0.512, v52, "5.2 GHz"};
    Slot gaps58{BAND_WIFI_5G, true, 2, 0.512, {}, "5.8 GHz gaps"};
    Slot gaps52{BAND_WIFI_5G, true, 1, 0.512, {}, "5.2 GHz gaps"};

    // Gap and DroneID visits are worked out against the sweep alone on 2.4 GHz, so the sweep
    // covers the band by itself.
    std::vector<double> centres;
    for (const auto* slot : {&sweep, &s58, &s52}) for (const auto& v : slot->visits) centres.push_back(v.center_hz);
    const auto extra = drone_gap_visits(wifi_rate_hz, centres);
    const auto slot_for = [&](const Visit& v, Slot& five_eight, Slot& five_two) -> Slot& {
        return v.center_hz < 3e9 ? sweep : v.center_hz < 5.5e9 ? five_two : five_eight;
    };
    for (const auto& v : extra.droneid) slot_for(v, s58, s52).visits.push_back(v);
    for (const auto& v : extra.gaps) slot_for(v, gaps58, gaps52).visits.push_back(v);
    for (Slot slot : {Slot{BAND_SUB_GHZ, true, 1, 0.512, {sub_visit}, "Sub-GHz"}, sweep, rest24, s58, s52, gaps58, gaps52}) {
        if (slot.visits.empty()) continue;
        // A short list (wide captures) is not visited twice in one cycle.
        slot.visits_per_cycle = std::min(slot.visits_per_cycle, int(slot.visits.size()));
        s.slots.push_back(std::move(slot));
    }
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
