#pragma once

// A link's frame timing followed from capture to capture on the receiver clock (Lightbridge since
// 21 Nov 2026; 15 and 30 kHz OcuSync since 23 Nov, each with its own symbol period, see LinkTiming).
//
// Each capture gives where the link's symbol grid sits modulo one symbol (link_clock::Estimate::
// symbol_phase_s) at a known device time. A drone's symbols keep coming at its own clock rate
// while we retune and while it hops, so the grid positions of its captures lie on one line
// against device time, wrapped modulo the symbol period: the line's slope is the link's clock
// error (us per s = ppm), measured over the whole span instead of within one 0.5 s capture.
//
// The slope is found by search, not by chaining captures one after another: every slope within
// +-5 ppm (or 3 prior sigmas) of the prior is tried, and the one under which the captures'
// positions agree best modulo the symbol wins (their phasors' mean length, 1 when all agree).
// Gaps then need no bridging. A wrong slope can also fit when the captures' times have a common
// period (an alias P / period away), so the best must beat every peak beyond its own lobe by a
// margin. Captures then off the line by more than an eighth of a symbol are left out.
//
// Measured 21 Nov 2026 (Phantom 4, X310 device time): recorded sweep, 40 captures over 33 s,
// -3.96 ppm, 2.3 us rms; live run4, 14 captures 0.7-13 s apart over 85 s, -4.07 +- 0.03 ppm,
// 2.9 us rms (the earlier chaining broke after 2). A recording with captures in bursts every
// 23.5 s gave the right slope but an alias almost as good: not used. Synthetic, at the live
// cadence with a 1.2 ppm-biased prior: from 8 captures no wrong clock and no random positions
// accepted in 2400 draws (from 4-6 captures up to 10% wrong, so 8 are needed).
//
// The device clock restarts when the radio reconnects: only captures of one radio session are
// used (the largest such group).
//
// OcuSync (training corpus, 23 Nov 2026, tools/drone_corpus_check.cpp): the grid position at the
// nominal symbol rate (14 / 28 kHz) lay on one line over 1-13 s at 0.06-0.4 us rms on 67 of 71
// recordings of 15 and 30 kHz links of eleven models, slope within about 0.1 ppm of DroneID. Its
// per-capture noise is about a tenth of Lightbridge's while its period is shorter (71.4 / 35.7 us),
// and its single-capture link clock is good to 0.1-0.2 ppm, so the search spans +-1 ppm around it
// (aliases from a gap g sit period / g apart: 3.6 ppm for 20 s at 15 kHz).

#include "link_clock.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace rfmon::frame_timing {

inline constexpr double symbol_s = link_clock::lightbridge_symbol_s;

struct Sample {
    std::string run;
    std::uint64_t radio_session{};
    double device_s{};  // capture start on the device clock
    double phase_s{};   // symbol grid position after the capture start, modulo the symbol period
    double period_s{symbol_s};  // the link's nominal symbol period (link_clock::symbol_period_s)
};

// Measured constants per link (by its symbol period).
struct LinkTiming {
    double period_s{};
    double position_sigma_s{};       // per-capture position noise floor
    double max_rms_s{};              // a fit is used with at most this rms
    double search_half_width_ppm{};  // slopes searched at least this far either side of the prior
    double search_step_s{};          // slope step: this over the span (and at most 0.05 ppm)
    std::size_t min_captures{};      // a fit is used with at least this many captures on the line
    double prior_min_sigma_ppm{};    // the prior (mean single-capture clock) never tighter than this
    double line_up_floor_s{};        // two rows line up within 1.5x their own rms, at least this
    double misfit_s{};               // joint rms from this on: they do not line up
};
// Lightbridge (21 Nov 2026, X310): per-capture positions 2.2-2.9 us rms; one capture's clock
// scattered about 2.5x its own sigma, means of a sweep's readings 1.0-1.2 ppm off the chained
// clock; from 4-6 captures up to 10% wrong clocks at the live cadence, none from 8.
inline constexpr LinkTiming lightbridge_timing{symbol_s, 2.5e-6, 5e-6, 5, 0.5e-6, 8, 1.5, 2e-6, 10e-6};
// OcuSync (corpus, 23 Nov 2026): 0.06-0.4 us rms per line; single-capture clocks within 0.07 ppm
// of DroneID (median) with 0.04-0.2 ppm scatter; views of one link at band edges up to 1.5 us apart.
// Synthetic, captures 5-40 s apart (a link heard now and then, as the Mini 2 live), prior 0.3 ppm
// off, 0.3-1 us noise, 1500 draws: no wrong clock at any count; random positions accepted at 15 kHz
// 1-4 from 5 captures (28 from 4), at 30 kHz (aliases twice as close) 3-5 from 6 (24-29 from 5).
inline LinkTiming ocusync_timing(double period_s) {
    const std::size_t min_captures = period_s > 50e-6 ? 5 : 6;
    return {period_s, 0.25e-6, 1.5e-6, 1, period_s / 20, min_captures, 0.3, 1e-6, period_s / 10};
}
inline LinkTiming timing_for(double period_s) {
    return std::abs(period_s - symbol_s) < 1e-9 ? lightbridge_timing : ocusync_timing(period_s);
}
// Captures off the best line by more than an eighth of a symbol are not counted (a wrong copy is
// half a symbol away). A fit's span at least min_span_s, at least 80% of its captures on the line,
// its peak this far above any alias. The crystal drifts a few hundredths of a ppm per minute while
// it warms: never claim better than min_sigma_ppm.
inline constexpr double min_span_s = 5;
inline constexpr double min_kept_fraction = 0.8;
inline constexpr double min_peak_margin = 0.35;
inline constexpr double min_sigma_ppm = 0.05;
// Lightbridge values kept under their old names.
inline constexpr double position_sigma_s = lightbridge_timing.position_sigma_s;
inline constexpr double keep_window_s = symbol_s / 8;
inline constexpr std::size_t min_captures = lightbridge_timing.min_captures;
inline constexpr double max_rms_s = lightbridge_timing.max_rms_s;

struct Fit {
    LinkTiming timing{lightbridge_timing};
    std::size_t used{}, connected{};  // captures in the session; on the line
    double ppm{}, sigma_ppm{};
    double rms_s{}, span_s{};
    double peak{}, alias{};            // phasor mean length at the best slope; best outside its peak
    double origin_s{}, icpt_s{};       // the line: position(t) = icpt + slope * (t - origin), slope = -ppm * 1e-6
    bool good() const {
        return connected >= timing.min_captures && span_s >= min_span_s && rms_s <= timing.max_rms_s &&
               double(connected) >= min_kept_fraction * double(used) && peak - alias >= min_peak_margin;
    }
};

// Position of the symbol grid on the device clock, modulo the symbol period, in [0, period).
inline double grid_position(double device_s, double phase_s, double period_s = symbol_s) {
    double p = std::fmod(std::fmod(device_s, period_s) + phase_s, period_s);
    return p < 0 ? p + period_s : p;
}

// Fits the captures of the largest radio session (of one symbol period). prior_ppm /
// prior_sigma_ppm: the link's clock as known before (mean of its single-capture readings)
// centres the search.
inline std::optional<Fit> fit(const std::vector<Sample>& samples, double prior_ppm, double prior_sigma_ppm) {
    std::map<std::tuple<std::string, std::uint64_t, double>, std::vector<const Sample*>> sessions;
    for (const auto& s : samples)
        if (std::isfinite(s.device_s) && std::isfinite(s.phase_s) && s.period_s > 0)
            sessions[{s.run, s.radio_session, s.period_s}].push_back(&s);
    const std::vector<const Sample*>* group = nullptr;
    for (const auto& [key, v] : sessions)
        if (!group || v.size() > group->size()) group = &v;
    if (!group || group->size() < 2 || !std::isfinite(prior_ppm)) return std::nullopt;
    const LinkTiming lt = timing_for(group->front()->period_s);
    const double P = lt.period_s;
    const std::size_t n = group->size();
    double t_min = INFINITY, t_max = -INFINITY;
    for (const auto* s : *group) { t_min = std::min(t_min, s->device_s); t_max = std::max(t_max, s->device_s); }
    const double span = t_max - t_min;
    if (!(span > 0)) return std::nullopt;
    std::vector<double> t(n), pos(n);
    for (std::size_t k = 0; k < n; ++k) {
        t[k] = (*group)[k]->device_s - t_min;
        pos[k] = grid_position((*group)[k]->device_s, (*group)[k]->phase_s, P);
    }
    // Slopes (s per s, = -ppm * 1e-6) from -(prior + w) to -(prior - w). Each capture's phasor
    // exp(2 pi i (pos - slope t) / P) is stepped by one complex multiply per slope.
    constexpr double two_pi = 6.283185307179586;
    const double w = std::max(lt.search_half_width_ppm, 3 * prior_sigma_ppm);
    const double step = std::min(0.05e-6, lt.search_step_s / span);
    const double first = -(prior_ppm + w) * 1e-6;
    const std::size_t count = static_cast<std::size_t>(std::ceil(2 * w * 1e-6 / step)) + 1;
    std::vector<std::complex<double>> z(n), dz(n);
    for (std::size_t k = 0; k < n; ++k) {
        z[k] = std::polar(1.0, two_pi * (pos[k] - first * t[k]) / P);
        dz[k] = std::polar(1.0, -two_pi * step * t[k] / P);
    }
    std::vector<double> peak(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::complex<double> sum{};
        for (std::size_t k = 0; k < n; ++k) { sum += z[k]; z[k] *= dz[k]; }
        peak[i] = std::abs(sum) / double(n);
    }
    const std::size_t best = std::size_t(std::max_element(peak.begin(), peak.end()) - peak.begin());
    // The best peak's own lobe runs down to a valley on each side; an alias is any peak beyond
    // those valleys (aliases from a gap g sit symbol / g apart, with a valley in between). A fixed
    // exclusion of half the lobe width counted the lobe's own shoulder (0.64 of the peak for
    // evenly spread captures) as an alias: live 23 Nov 2026 the Phantom's rows, lining up at
    // 3-4 us over 100+ captures, read 0.97 against "alias" 0.63 and flickered to "possibly".
    std::size_t lo = best, hi = best;
    while (lo > 0 && peak[lo - 1] <= peak[lo]) --lo;
    while (hi + 1 < count && peak[hi + 1] <= peak[hi]) ++hi;
    double alias = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (i < lo || i > hi) alias = std::max(alias, peak[i]);
    // Unwrap each capture to the copy nearest the best line, then least squares on those within
    // an eighth of a symbol (twice: the refined line may keep a different set).
    double slope = first + double(best) * step;
    std::complex<double> mean{};
    for (std::size_t k = 0; k < n; ++k) mean += std::polar(1.0, two_pi * (pos[k] - slope * t[k]) / P);
    double icpt = std::arg(mean) / two_pi * P;
    std::vector<double> u(n);
    std::vector<bool> keep(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double pred = icpt + slope * t[k];
        u[k] = pos[k] + std::round((pred - pos[k]) / P) * P;
    }
    std::size_t kept = 0;
    for (int pass = 0; pass < 2; ++pass) {
        double s1 = 0, st = 0, stt = 0, su = 0, stu = 0;
        kept = 0;
        for (std::size_t k = 0; k < n; ++k) {
            keep[k] = std::abs(u[k] - icpt - slope * t[k]) <= P / 8;
            if (!keep[k]) continue;
            ++kept; s1 += 1; st += t[k]; stt += t[k] * t[k]; su += u[k]; stu += t[k] * u[k];
        }
        const double det = s1 * stt - st * st;
        if (kept < 2 || !(det > 0)) break;
        slope = (s1 * stu - st * su) / det;
        icpt = (stt * su - st * stu) / det;
    }
    Fit f;
    f.timing = lt;
    f.used = n;
    f.origin_s = t_min;
    f.icpt_s = icpt;
    f.peak = peak[best];
    f.alias = alias;
    f.ppm = -slope * 1e6;
    double ss = 0, tk_min = INFINITY, tk_max = -INFINITY, tm = 0;
    for (std::size_t k = 0; k < n; ++k)
        if (keep[k]) { tm += t[k]; tk_min = std::min(tk_min, t[k]); tk_max = std::max(tk_max, t[k]); }
    f.connected = 0;
    for (std::size_t k = 0; k < n; ++k) f.connected += keep[k];
    if (!f.connected) return f;
    tm /= double(f.connected);
    double stt = 0;
    for (std::size_t k = 0; k < n; ++k)
        if (keep[k]) {
            const double r = u[k] - icpt - slope * t[k];
            ss += r * r;
            stt += (t[k] - tm) * (t[k] - tm);
        }
    f.rms_s = std::sqrt(ss / double(f.connected));
    f.span_s = tk_max - tk_min;
    f.sigma_ppm = std::max(std::max(f.rms_s, lt.position_sigma_s) / std::sqrt(std::max(stt, 1e-12)) * 1e6, min_sigma_ppm);
    return f;
}

// Does a capture sit on a fitted line (within an eighth of a symbol, modulo the symbol)?
inline bool on_line(const Fit& f, const Sample& s) {
    const double P = f.timing.period_s;
    if (std::abs(s.period_s - P) > 1e-12) return false;
    const double pred = f.icpt_s - f.ppm * 1e-6 * (s.device_s - f.origin_s);
    const double d = std::remainder(grid_position(s.device_s, s.phase_s, P) - pred, P);
    return std::abs(d) <= P / 8;
}
}
