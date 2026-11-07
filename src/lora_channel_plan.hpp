// LoRa listen channel plans: regional presets, user-entered lists, and the
// capture schedule that covers them. Pure software; no radio or GUI.
//
// Wideband listening tunes ONE capture so several channels fall inside its
// usable band, then extracts each channel at the 500 kHz decode rate with
// lora_channelizer.hpp. Every channel in that capture is then observed for
// the whole capture window instead of one channel in N. The default width is
// 20 MHz, the rate the Wi-Fi, drone and coordinated paths already use.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"

namespace rfmon::lora_plan {

struct Preset {
    const char* name;
    std::vector<double> channels_hz;
    const char* note;
};

// Channel centres only; regional duty-cycle and dwell rules are the
// transmitter's concern, not a passive receiver's.
inline const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"Lab default (IN865 + 866.9)", LORA_LISTEN_CHANNELS_HZ,
         "IN865 default channels plus the bench TarangMini's 866.9 MHz."},
        {"IN865 default", {865.0625e6, 865.4025e6, 865.985e6}, "LoRaWAN IN865 join/default channels."},
        {"EU868 default", {868.1e6, 868.3e6, 868.5e6}, "LoRaWAN EU868 mandatory channels."},
        {"EU868 8-channel (TTN)", {867.1e6, 867.3e6, 867.5e6, 867.7e6, 867.9e6, 868.1e6, 868.3e6, 868.5e6},
         "Common 8-channel gateway plan."},
        {"US915 sub-band 2", {903.9e6, 904.1e6, 904.3e6, 904.5e6, 904.7e6, 904.9e6, 905.1e6, 905.3e6, 904.6e6},
         "Channels 8-15 (125 kHz) and 65 (500 kHz)."},
        {"AU915 sub-band 2", {916.8e6, 917.0e6, 917.2e6, 917.4e6, 917.6e6, 917.8e6, 918.0e6, 918.2e6, 917.5e6},
         "Channels 8-15 (125 kHz) and 65 (500 kHz)."},
        {"AS923-1 default", {923.2e6, 923.4e6}, "LoRaWAN AS923 default channels."},
        {"KR920 default", {922.1e6, 922.3e6, 922.5e6}, "LoRaWAN KR920 default channels."},
        {"IN865 + EU868 (863-870 MHz)",
         {865.0625e6, 865.4025e6, 865.985e6, 866.9e6, 867.1e6, 867.3e6, 867.5e6, 867.7e6, 867.9e6, 868.1e6, 868.3e6, 868.5e6,
          869.525e6},
         "Both regional plans plus the EU868 RX2 channel; fits one 20 MHz capture."},
    };
    return p;
}

constexpr size_t MAX_CHANNELS = 32;
constexpr double MIN_CHANNEL_HZ = 100e6, MAX_CHANNEL_HZ = 6e9;

// Sorted, de-duplicated (to 1 kHz) and bounded copy of `channels`.
inline std::vector<double> normalise(std::vector<double> channels) {
    channels.erase(std::remove_if(channels.begin(), channels.end(), [](double f) {
        return !std::isfinite(f) || f < MIN_CHANNEL_HZ || f > MAX_CHANNEL_HZ;
    }), channels.end());
    std::sort(channels.begin(), channels.end());
    channels.erase(std::unique(channels.begin(), channels.end(),
                               [](double a, double b) { return std::abs(a - b) < 1e3; }),
                   channels.end());
    if (channels.size() > MAX_CHANNELS) channels.resize(MAX_CHANNELS);
    return channels;
}

// "865.0625, 865.4025 866.9" (MHz, comma/space/semicolon separated).
inline std::vector<double> parse_channels_mhz(const std::string& text, std::string* error = nullptr) {
    std::vector<double> out;
    const char* p = text.c_str();
    while (*p) {
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t' || *p == '\n') ++p;
        if (!*p) break;
        char* end = nullptr;
        const double mhz = std::strtod(p, &end);
        if (end == p) {
            if (error) *error = std::string("Not a number near \"") + std::string(p).substr(0, 12) + "\"";
            return {};
        }
        const double hz = mhz * 1e6;
        if (!std::isfinite(hz) || hz < MIN_CHANNEL_HZ || hz > MAX_CHANNEL_HZ) {
            if (error) *error = "Channel outside 100-6000 MHz";
            return {};
        }
        out.push_back(hz);
        p = end;
    }
    out = normalise(out);
    if (out.empty() && error) *error = "Enter at least one channel in MHz";
    return out;
}

inline std::string format_channels_mhz(const std::vector<double>& channels) {
    std::string s;
    for (double f : channels) {
        char b[32];
        std::snprintf(b, sizeof b, "%s%.4f", s.empty() ? "" : ", ", f / 1e6);
        s += b;
    }
    return s;
}

// The part of a capture's band kept clear of its anti-alias edges; the same
// guard the coordinated scheduler applies (coordinated_schedule.hpp).
inline double usable_half_span(double rate_hz) { return 0.5 * rate_hz * (1.0 - 2 * EDGE_GUARD_FRACTION); }

struct Capture {
    double center_hz = 0, rate_hz = 0;
    std::vector<double> channels_hz;
    bool wideband = false;
    double dc_clearance_hz = 0;  // nearest channel centre to the tuned centre (wideband only)
    // Nearest any channel's IQ-imbalance image (2*centre - f) comes to a planned
    // channel, its own included (wideband only; see mirror_clearance()).
    double mirror_clearance_hz = 0;
    bool covers_range = false;   // the requested extra range (PlanOptions::cover_*) is inside too
};

// Receiver IQ imbalance puts a weak conjugate image of a signal at f on the
// other side of the tuned centre, at 2*centre - f, where it decodes as an
// inverted-IQ packet (seen live with the X310, which cannot correct RX IQ
// balance). The image of channel a lands on channel b when a + b is near
// 2*centre; this is the smallest |a + b - 2*centre| over all pairs, a == b
// included (a channel near the centre images onto itself).
inline double mirror_clearance(const std::vector<double>& channels, double center) {
    double nearest = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < channels.size(); ++i)
        for (size_t j = i; j < channels.size(); ++j)
            nearest = std::min(nearest, std::abs(channels[i] + channels[j] - 2 * center));
    return nearest;
}

// Default image guard: the decoder accepts a frequency offset up to half the
// signal bandwidth (BW500: 250 kHz), so an image this far off a channel
// cannot be decoded on it.
constexpr double DEFAULT_MIN_MIRROR_CLEARANCE_HZ = 250e3;

// Wideband rates tried in order; each is an exact X310 clock division
// (200 MHz / n) and a multiple of the 500 kHz decode rate.
inline const std::vector<double>& wideband_rates() {
    static const std::vector<double> r = {2e6, 2.5e6, 4e6, 5e6, 8e6, 10e6, 20e6};
    return r;
}

// Tuned centre for `channels` at `rate`: every channel (and, when given and
// possible, the range [cover_lo, cover_hi]) inside the usable band. Preference:
// centres at least `min_clearance` from every channel centre (receiver DC/LO
// residue) and at least `min_mirror` of mirror clearance, nearest the middle
// of what is covered; else, among DC-clear centres, the most mirror clearance;
// else the most DC clearance. Returns false when the channels do not fit.
inline bool place_center(const std::vector<double>& channels, double rate, double channel_rate, double& center,
                         double& clearance, bool& covers_range, double cover_lo = 0, double cover_hi = 0,
                         double min_clearance = 100e3, double* mirror_out = nullptr,
                         double min_mirror = DEFAULT_MIN_MIRROR_CLEARANCE_HZ) {
    const double usable = usable_half_span(rate);
    const double reach = usable - channel_rate / 2;
    if (reach <= 0 || channels.empty()) return false;
    double lo = channels.back() - reach, hi = channels.front() + reach;
    if (lo > hi) return false;
    double first = channels.front(), last = channels.back();
    covers_range = false;
    if (cover_hi > cover_lo) {
        const double clo = std::max(lo, cover_hi - usable), chi = std::min(hi, cover_lo + usable);
        if (clo <= chi) {
            lo = clo;
            hi = chi;
            covers_range = true;
            first = std::min(first, cover_lo);
            last = std::max(last, cover_hi);
        }
    }
    const double mid = std::clamp(0.5 * (first + last), lo, hi);
    auto clear_at = [&](double c) {
        double nearest = std::numeric_limits<double>::infinity();
        for (double f : channels) nearest = std::min(nearest, std::abs(f - c));
        return nearest;
    };
    // Candidates: whole kHz across [lo, hi].
    const double step = std::max(1e3, std::round((hi - lo) / 2000 / 1e3) * 1e3);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    double best_ok = nan, best_dc = nan, best_dc_mirror = -1, best_any = mid, best_clear = -1;
    for (double c = std::ceil(lo / 1e3) * 1e3; c <= hi; c += step) {
        const double cl = clear_at(c);
        if (cl > best_clear) { best_clear = cl; best_any = c; }
        if (cl < min_clearance) continue;
        const double mc = mirror_clearance(channels, c);
        if (mc >= min_mirror && (std::isnan(best_ok) || std::abs(c - mid) < std::abs(best_ok - mid))) best_ok = c;
        if (mc > best_dc_mirror || (mc == best_dc_mirror && std::abs(c - mid) < std::abs(best_dc - mid))) {
            best_dc_mirror = mc;
            best_dc = c;
        }
    }
    center = !std::isnan(best_ok) ? best_ok : !std::isnan(best_dc) ? best_dc : best_any;
    clearance = clear_at(center);
    if (mirror_out) *mirror_out = mirror_clearance(channels, center);
    return true;
}

struct PlanOptions {
    double channel_rate = 500e3;   // per-channel decode rate
    double max_rate = 20e6;        // radio link / per-capture sample ceiling
    bool wideband = true;
    double preferred_rate = 0;     // 0: narrowest rate that fits; else this rate when allowed
    double cover_lo = 0, cover_hi = 0;  // also keep this range in the capture when possible
    double min_dc_clearance = 100e3;
    double min_mirror_clearance = DEFAULT_MIN_MIRROR_CLEARANCE_HZ;
};

// Captures that together cover every channel once per rotation.
inline std::vector<Capture> plan(const std::vector<double>& channels_in, const PlanOptions& o) {
    const auto channels = normalise(channels_in);
    std::vector<Capture> out;
    if (channels.empty()) return out;
    auto single = [&](double f) {
        Capture c;
        c.center_hz = f;
        c.rate_hz = o.channel_rate;
        c.channels_hz = {f};
        out.push_back(c);
    };
    if (!o.wideband) {
        for (double f : channels) single(f);
        return out;
    }
    auto usable_rate = [&](double r) {
        const double ratio = r / o.channel_rate;
        return r <= o.max_rate + 1 && std::abs(ratio - std::round(ratio)) < 1e-9 && ratio >= 2;
    };
    std::vector<double> rates;
    const bool fixed = o.preferred_rate > 0 && usable_rate(o.preferred_rate);
    if (fixed) rates = {o.preferred_rate};
    else
        for (double r : wideband_rates()) if (usable_rate(r)) rates.push_back(r);
    size_t i = 0;
    while (i < channels.size()) {
        size_t j = i + 1;
        if (!rates.empty())
            while (j < channels.size() && (channels[j] - channels[i]) / 2 + o.channel_rate / 2 <= usable_half_span(rates.back())) ++j;
        // A fixed width also captures a lone channel wideband (broad energy view);
        // auto mode keeps a lone channel at the decode rate, as before.
        if (rates.empty() || (j - i < 2 && !fixed)) {
            single(channels[i]);
            i = j;
            continue;
        }
        std::vector<double> cluster(channels.begin() + long(i), channels.begin() + long(j));
        Capture best;
        bool found = false;
        // Narrowest rate whose centre is DC- and image-clear; failing that, the
        // rate giving the best centre (DC-clear first, then mirror clearance).
        auto rank = [&](double dc, double mirror) {
            return std::make_pair(dc >= o.min_dc_clearance ? 1 : 0,
                                  dc >= o.min_dc_clearance ? std::min(mirror, o.min_mirror_clearance) : dc);
        };
        for (double r : rates) {
            double c = 0, clear = 0, mirror = 0;
            bool covers = false;
            if (!place_center(cluster, r, o.channel_rate, c, clear, covers, o.cover_lo, o.cover_hi, o.min_dc_clearance,
                              &mirror, o.min_mirror_clearance))
                continue;
            if (!found || rank(clear, mirror) > rank(best.dc_clearance_hz, best.mirror_clearance_hz)) {
                best.center_hz = c;
                best.rate_hz = r;
                best.dc_clearance_hz = clear;
                best.mirror_clearance_hz = mirror;
                best.covers_range = covers;
                found = true;
            }
            if (best.dc_clearance_hz >= o.min_dc_clearance && best.mirror_clearance_hz >= o.min_mirror_clearance) break;
        }
        if (!found) {
            for (double f : cluster) single(f);
        } else {
            best.channels_hz = cluster;
            best.wideband = true;
            out.push_back(best);
        }
        i = j;
    }
    return out;
}

// Auto width (narrowest rate that fits), no extra range: the original API.
inline std::vector<Capture> plan(const std::vector<double>& channels, double channel_rate, double max_rate,
                                 bool wideband, double min_dc_clearance = 100e3) {
    PlanOptions o;
    o.channel_rate = channel_rate;
    o.max_rate = max_rate;
    o.wideband = wideband;
    o.min_dc_clearance = min_dc_clearance;
    return plan(channels, o);
}

// True when one wideband capture is the whole rotation and it keeps
// [lo, hi] in its usable band: it can stand in for a separate energy scan.
inline bool single_capture_covers(const std::vector<Capture>& rotation, double lo, double hi) {
    if (rotation.size() != 1 || !rotation[0].wideband) return false;
    const auto& c = rotation[0];
    const double h = usable_half_span(c.rate_hz);
    return lo >= c.center_hz - h && hi <= c.center_hz + h;
}

// What the scanner actually listened to since the plan last changed.
struct ListenStats {
    uint64_t captures = 0;
    double last_capture_s = 0, last_decode_s = 0;
    double last_center_hz = 0, last_rate_hz = 0;
    size_t last_channels = 0;
    bool last_wideband = false;
    std::map<double, double> channel_listened_s;  // channel -> seconds of IQ decoded
    double first_listen_steady_s = 0;             // steady clock at the first listen's start
    // Wideband processing stages of the last capture (wall seconds).
    double last_channelize_s = 0, last_energy_s = 0, last_analyze_s = 0, last_commit_s = 0;
};

// Energy-scan rows the GUI already has (registry DeviceRow fields, copied in
// by the caller so this header has no registry dependency).
struct Observation {
    double freq_hz = 0, bandwidth_hz = 0, power_db = 0;
    int hits = 0;
    bool lora_like = false;
};
struct Suggestion {
    double freq_hz = 0, bandwidth_hz = 0, power_db = 0;
    int hits = 0;
};

// LoRa-like energy not already within `tolerance_hz` of a planned channel,
// snapped to a 12.5 kHz grid and merged within 50 kHz. Energy shape only:
// a suggestion is not a decoded LoRa packet.
inline std::vector<Suggestion> suggest(const std::vector<Observation>& obs, const std::vector<double>& planned,
                                       double tolerance_hz = 40e3, size_t limit = 8) {
    std::vector<Suggestion> out;
    for (const auto& o : obs) {
        if (!o.lora_like || !std::isfinite(o.freq_hz) || o.freq_hz < MIN_CHANNEL_HZ) continue;
        const double f = std::round(o.freq_hz / 12.5e3) * 12.5e3;
        bool planned_already = false;
        for (double p : planned) planned_already = planned_already || std::abs(p - f) <= tolerance_hz;
        if (planned_already) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const Suggestion& s) { return std::abs(s.freq_hz - f) <= 50e3; });
        if (it == out.end()) {
            out.push_back({f, o.bandwidth_hz, o.power_db, o.hits});
        } else {
            it->hits += o.hits;
            if (o.power_db > it->power_db) {
                it->power_db = o.power_db;
                it->freq_hz = f;
                it->bandwidth_hz = o.bandwidth_hz;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) {
        return a.hits != b.hits ? a.hits > b.hits : a.power_db > b.power_db;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace rfmon::lora_plan
