#pragma once

// Do two rows come from the same crystal? (Display only: the Drones page's "Same drone?" marks.)
//
// Two DroneID rows compare their offsets in ppm, close in time. A link-only row compares its
// 15 kHz link grids with a DroneID row's ppm, allowing for the 5 kHz ambiguity of a link grid,
// and its link clock (crystal ppm from symbol timing, link_clock.hpp) with the same ppm; the
// stronger agreement of the two is shown. Two rows of one link type (Lightbridge, 15 or 30 kHz
// OcuSync) with frame timing first fit it together (frame_timing.hpp), then compare their clocks
// (from frame timing when each row's own fit is good).
// Thresholds come from the live Mini 2 flight on 19 Nov 2026 (X310): DroneID packets of one drone
// agreed within about 0.05 ppm between bands; its link captures sat 170-420 Hz from its
// prediction; a different drone's crystal differs by about 0.25-0.5 ppm (KU Leuven DroneRF).

#include "evidence_claims.hpp"
#include <cstdio>
#include <string>

namespace rfmon::drone_claims {

inline constexpr double droneid_same_ppm = 0.10;       // two DroneID rows this close: same crystal
inline constexpr double droneid_different_ppm = 0.30;  // this far apart: different crystals
inline constexpr double link_likely_hz = 350;          // link grids within this of the prediction ...
inline constexpr std::size_t link_likely_captures = 3; // ... in this many captures: probably the same
inline constexpr double link_possible_hz = 600;        // within this: possibly the same
inline constexpr double link_different_hz = 1200;      // this far, in at least two captures: different
inline constexpr std::size_t link_different_captures = 2;
// Link clocks (each reading carries its own sigma; a row's readings are averaged). Measured
// 21 Nov 2026: OcuSync link clocks within 0.05 ppm of DroneID on three drones; one Phantom 4
// Lightbridge visit scatters about 1 ppm; Lightbridge drones measured so far sit 0.5-4 ppm apart.
inline constexpr double link_clock_agree_sigmas = 2;        // within this many sigma: consistent
inline constexpr double link_clock_different_sigmas = 3;    // this many sigma apart ...
inline constexpr double link_clock_different_ppm = 0.5;     // ... and at least this far: different
inline constexpr double link_clock_likely_sigma = 0.15;     // consistent, with sigma this small: likely
inline constexpr double link_clock_possible_sigma = 0.6;    // consistent, with sigma this small: possible

struct CrystalMatch {
    enum Kind { None, Possible, Likely, Different } kind = None;
    std::string reason;
};

// A difference between two crystal readings with its error, judged by the link-clock thresholds.
inline CrystalMatch::Kind judge_link_clock(double difference_ppm, double sigma_ppm) {
    const double d = std::abs(difference_ppm);
    if (d >= link_clock_different_ppm && d >= link_clock_different_sigmas * sigma_ppm) return CrystalMatch::Different;
    if (d > link_clock_agree_sigmas * sigma_ppm) return CrystalMatch::None;
    if (sigma_ppm <= link_clock_likely_sigma) return CrystalMatch::Likely;
    if (sigma_ppm <= link_clock_possible_sigma) return CrystalMatch::Possible;
    return CrystalMatch::None;
}

inline const char* link_kind(const std::vector<LinkClockSample>& clocks) {
    return !clocks.empty() && clocks.front().lightbridge ? "Lightbridge" : "OcuSync";
}

// A row's frame timing over its captures (frame_timing.hpp), when good. The prior is the mean of
// its single-capture clocks with a widened error, never tighter than the link's
// prior_min_sigma_ppm: live (21 Nov 2026) one Lightbridge capture scattered about 2.5x its own
// sigma, and the means of a recorded sweep's readings sat 1.0-1.2 ppm off the chained clock.
inline constexpr double frame_prior_min_sigma_ppm = frame_timing::lightbridge_timing.prior_min_sigma_ppm;
inline double frame_period(const Detection& row) {
    return row.frame_samples.empty() ? 0 : row.frame_samples.front().period_s;
}
inline std::optional<frame_timing::Fit> frame_fit(const Detection& row) {
    if (row.frame_samples.empty()) return std::nullopt;
    const auto lt = frame_timing::timing_for(frame_period(row));
    if (row.frame_samples.size() < lt.min_captures) return std::nullopt;
    const auto m = link_clock_mean(row.link_clocks);
    auto f = frame_timing::fit(row.frame_samples, m ? m->ppm : 0,
                               m ? std::max(2.5 * m->sigma_ppm, lt.prior_min_sigma_ppm) : 3.0);
    if (!f || !f->good()) return std::nullopt;
    return f;
}

// The row's best clock: chained frame timing when good, else the mean of its link clocks.
struct RowClock {
    double ppm{}, sigma_ppm{};
    std::size_t captures{};
    bool frame{};
};
inline std::optional<RowClock> row_clock(const Detection& row) {
    if (const auto f = frame_fit(row)) return RowClock{f->ppm, f->sigma_ppm, f->connected, true};
    if (const auto m = link_clock_mean(row.link_clocks)) return RowClock{m->ppm, m->sigma_ppm, m->captures, false};
    return std::nullopt;
}

// Two rows' frame timing (same link type): their captures fitted together. The same transmitter
// lines up as well as each row alone; a different one sits anywhere within half a symbol of the
// line (Lightbridge +-51 us, 15 kHz OcuSync +-36 us, 30 kHz +-18 us). A misfit only says
// "different" within one band. Lightbridge values below; each link's own in frame_timing.hpp.
inline constexpr double frame_misfit_s = frame_timing::lightbridge_timing.misfit_s;
inline constexpr double frame_line_up_floor_s = frame_timing::lightbridge_timing.line_up_floor_s;
// Shares of each row's captures on the joint line: at least this to line up; below the second,
// for either row, they do not.
inline constexpr double frame_on_line_share = 0.8, frame_off_line_share = 0.5;
inline CrystalMatch compare_rows_frame_timing(const Detection& a, const Detection& b) {
    if (std::abs(frame_period(a) - frame_period(b)) > 1e-12) return {};  // different link types
    const auto fa = frame_fit(a), fb = frame_fit(b);
    if (!fa || !fb) return {};
    const auto& lt = fa->timing;
    auto both = a.frame_samples;
    both.insert(both.end(), b.frame_samples.begin(), b.frame_samples.end());
    const double wa = 1 / (fa->sigma_ppm * fa->sigma_ppm), wb = 1 / (fb->sigma_ppm * fb->sigma_ppm);
    const auto f = frame_timing::fit(both, (wa * fa->ppm + wb * fb->ppm) / (wa + wb),
                                     std::max(std::abs(fa->ppm - fb->ppm), std::hypot(fa->sigma_ppm, fb->sigma_ppm)));
    if (!f) return {};
    char text[256];
    // Lines up: scatter together within 1.5x the rows' own (at least 2 us each). On the Phantom
    // sweep split into rows, shifting one row's timing by 3 us still lined up, 6-10 us did not
    // (undecided), 20 us and more read "different": an unrelated link at the same clock lines
    // up by chance about 8% of the time, and its clock must also match to keep doing so.
    const double alone = std::max({fa->rms_s, fb->rms_s, lt.line_up_floor_s});
    // Each row's own captures on the joint line: a different link has most of its captures off
    // it, however few it has. (Requiring every capture on it was too strict: live 23 Nov 2026 the
    // Phantom's 5.2 and 5.8 GHz rows, each lining up with its 2.4 GHz row, read "different"
    // because 1 of 88 captures was off.)
    const auto share = [&](const Detection& row) {
        std::size_t on = 0;
        for (const auto& smp : row.frame_samples) on += frame_timing::on_line(*f, smp);
        return row.frame_samples.empty() ? 0.0 : double(on) / double(row.frame_samples.size());
    };
    const double sa = share(a), sb = share(b), least = std::min(sa, sb);
    const bool clear = f->peak - f->alias >= frame_timing::min_peak_margin;
    if (clear && least >= frame_on_line_share && f->rms_s <= 1.5 * alone) {
        std::snprintf(text, sizeof text, "frame timing lines up across both rows: %.1f us rms over %zu captures and %.0f s "
                      "(an unrelated link would sit up to %.0f us off)", f->rms_s * 1e6, f->connected, f->span_s,
                      lt.period_s / 2 * 1e6);
        return {CrystalMatch::Likely, text};
    }
    if (least >= frame_on_line_share && f->rms_s < lt.misfit_s) {
        // Between lining up and a clear misfit: no stronger than "possibly", whatever the clocks say.
        std::snprintf(text, sizeof text, "frame timing roughly lines up: %.1f us rms together against %.1f and %.1f us "
                      "for each row alone", f->rms_s * 1e6, fa->rms_s * 1e6, fb->rms_s * 1e6);
        return {CrystalMatch::Possible, text};
    }
    if ((least < frame_off_line_share || f->rms_s >= lt.misfit_s) && a.band == b.band) {
        std::snprintf(text, sizeof text, "frame timing does not line up: on the joint line %.0f%% and %.0f%% of each row's "
                      "captures, %.1f us rms (each row alone: %.1f and %.1f us rms)", 100 * sa, 100 * sb, f->rms_s * 1e6,
                      fa->rms_s * 1e6, fb->rms_s * 1e6);
        return {CrystalMatch::Different, text};
    }
    return {};
}

// Two link-only rows' link clocks in words, with the judgement ("" when not comparable).
inline CrystalMatch compare_rows_link_clock(const Detection& a, const Detection& b) {
    if (const auto fa = frame_fit(a), fb = frame_fit(b); fa && fb) {
        const double d = fa->ppm - fb->ppm, sigma = std::hypot(fa->sigma_ppm, fb->sigma_ppm);
        const auto kind = judge_link_clock(d, sigma);
        char text[256];
        std::snprintf(text, sizeof text,
                      "link clocks from frame timing %s by %.2f +- %.2f ppm (%+.2f ppm over %zu captures, %+.2f ppm over %zu)",
                      std::abs(d) <= link_clock_agree_sigmas * sigma ? "agree" : "differ", std::abs(d), sigma,
                      fa->ppm, fa->connected, fb->ppm, fb->connected);
        return {kind, text};
    }
    const auto c = compare_link_clocks(a.link_clocks, b.link_clocks);
    if (!c) return {};
    const auto kind = judge_link_clock(c->difference_ppm, c->sigma_ppm);
    char text[256];
    std::snprintf(text, sizeof text,
                  "link clocks %s by %.2f +- %.2f ppm (%s %+.2f ppm over %zu capture%s, %s %+.2f ppm over %zu)",
                  std::abs(c->difference_ppm) <= link_clock_agree_sigmas * c->sigma_ppm ? "agree" : "differ",
                  std::abs(c->difference_ppm), c->sigma_ppm, link_kind(a.link_clocks), c->a.ppm, c->a.captures,
                  c->a.captures == 1 ? "" : "s", link_kind(b.link_clocks), c->b.ppm, c->b.captures);
    return {kind, text};
}

inline CrystalMatch crystal_match(const Detection& a, const Detection& b) {
    char text[256];
    if (!a.droneid_offsets.empty() && !b.droneid_offsets.empty()) {
        const auto c = compare_droneid(a.droneid_offsets, b.droneid_offsets);
        if (!c) return {};
        const double d = std::abs(c->difference_ppm);
        std::snprintf(text, sizeof text, "DroneID crystal offsets %s by %.2f ppm (%zu packet%s compared)",
                      d <= droneid_same_ppm ? "agree" : "differ", d, c->pairs, c->pairs == 1 ? "" : "s");
        if (d <= droneid_same_ppm) return {CrystalMatch::Likely, text};
        if (d >= droneid_different_ppm) return {CrystalMatch::Different, text};
        return {};
    }
    // A DroneID row whose link has frame timing, against a link-only row of the same link type:
    // a frame-timing verdict that settles it ("likely" or "different") comes first (it does not
    // depend on the crystals being apart, which two units of one model often are not).
    if (a.droneid_offsets.empty() != b.droneid_offsets.empty())
        if (auto f = compare_rows_frame_timing(a, b); f.kind == CrystalMatch::Likely || f.kind == CrystalMatch::Different)
            return f;
    for (const auto& [link, packet] : {std::pair{&a, &b}, std::pair{&b, &a}}) {
        if (!link->droneid_offsets.empty() || packet->droneid_offsets.empty()) continue;
        if (link->link_grids.empty() && link->link_clocks.empty()) continue;
        // The link clock against the DroneID crystal.
        const auto by_clock = [&]() -> CrystalMatch {
            const auto f = fit_link_clock(link->link_clocks, packet->droneid_offsets);
            if (!f) return {};
            const auto kind = judge_link_clock(f->difference_ppm, f->sigma_ppm);
            if (kind == CrystalMatch::None) return {};
            std::snprintf(text, sizeof text,
                          "%s link clock %s the DroneID crystal (%+.2f ppm) by %.2f +- %.2f ppm over %zu capture%s",
                          link_kind(link->link_clocks), kind == CrystalMatch::Different ? "differs from" : "agrees with",
                          f->ppm, std::abs(f->difference_ppm), f->sigma_ppm, f->captures, f->captures == 1 ? "" : "s");
            return {kind, text};
        };
        const auto fit = fit_link(link->link_grids, packet->droneid_offsets);
        const auto clock = by_clock();
        if (!fit) return clock;
        const double hz = fit->median_residual_hz;
        // An unrelated link's grid lands anywhere within +-2.5 kHz, so the chance it fits by
        // accident is the threshold over 2.5 kHz.
        const auto describe = [&](const char* verb, double limit) {
            std::snprintf(text, sizeof text,
                          "link grid %s the DroneID crystal (%+.2f ppm) by %.0f Hz over %zu capture%s; "
                          "about 1 in %.0f unrelated links would fit as well",
                          verb, fit->ppm, hz, fit->captures, fit->captures == 1 ? "" : "s",
                          link_raster_hz / 2 / limit);
            return std::string(text);
        };
        CrystalMatch grid;
        if (hz <= link_likely_hz && fit->captures >= link_likely_captures)
            grid = {CrystalMatch::Likely, describe("fits", link_likely_hz)};
        else if (hz <= link_possible_hz)
            grid = {CrystalMatch::Possible, describe("fits", link_possible_hz)};
        else if (hz >= link_different_hz && fit->captures >= link_different_captures) {
            std::snprintf(text, sizeof text, "link grid misses the DroneID crystal (%+.2f ppm) by %.0f Hz over %zu captures",
                          fit->ppm, hz, fit->captures);
            grid = {CrystalMatch::Different, text};
        }
        // Grid and clock together. The clock measures the crystal directly; the grid only modulo
        // 5 kHz. Live 21 Nov 2026: one Mini 2 capture's clock agreed with its DroneID within
        // 0.05 ppm while its grid alone allowed only "possibly". The stronger agreement wins;
        // when one says the same and the other different, neither is shown.
        const auto agrees = [](const CrystalMatch& m) {
            return m.kind == CrystalMatch::Likely || m.kind == CrystalMatch::Possible;
        };
        if ((agrees(grid) && clock.kind == CrystalMatch::Different) ||
            (grid.kind == CrystalMatch::Different && agrees(clock)))
            return {};
        if (clock.kind == CrystalMatch::None) return grid;
        if (grid.kind == CrystalMatch::None) return clock;
        if (clock.kind == grid.kind) return {clock.kind, clock.reason + "; " + grid.reason};
        return clock.kind == CrystalMatch::Likely ? clock : grid;
    }
    // Two link-only rows: their frame timing, else their link clocks.
    if (a.droneid_offsets.empty() && b.droneid_offsets.empty()) {
        if (auto f = compare_rows_frame_timing(a, b); f.kind != CrystalMatch::None) return f;
        auto c = compare_rows_link_clock(a, b);
        if (c.kind != CrystalMatch::None) return c;
    }
    return {};
}
}
