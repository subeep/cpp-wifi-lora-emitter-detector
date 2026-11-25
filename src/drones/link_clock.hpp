#pragma once

#include <complex>
#include <cstddef>
#include <optional>

// A link's own clock error (ppm), from how fast its OFDM symbols arrive in one capture.
//
// A drone derives its baseband sample clock and its carrier from one crystal, so its symbols
// arrive off by the same ppm as its carrier, measured against the X310's own clock: the same
// quantity the DroneID decoder measures from DroneID packets. This gives it for links without
// DroneID (Lightbridge) and for OcuSync link-only rows.
//
// It uses only the timing probe's blocks (24 x 131072 samples spread over the capture). Port of
// air3s_iq_recording/analysis/app_blocks_ppm.py; both start from each block's cyclic-prefix
// metric, decimated by 10 and timed from the block's position in the capture:
//   15 kHz OcuSync: phase of the metric's harmonics of the 14 kHz symbol rate against block time.
//   30 kHz OcuSync (O3+/O4 links: Mini 4 Pro, Avata 2): the same at 28 kHz.
//   Lightbridge: its symbols sit several us off a uniform grid depending on where they are in
//     the 7 ms frame, so each block is lined up against the capture's other blocks folded at the
//     frame period, and the block lags are fitted against block time.
// Measured offline (21 Nov 2026): KU Leuven Mini 2, Mavic, M300 within 0.05 ppm of their DroneID
// ppm (scatter 0.15-0.18 per 0.2 s capture); our Phantom 4, 0.512 s visits, scatter 1.0-1.5 ppm,
// mean within 0.35 of a 3 s recording. sigma_ppm is the fit's error halved (sigma_scale): on both
// links the fit's own error was about twice the scatter measured between captures.
namespace rfmon::link_clock {
inline constexpr char version[] = "link-clock-v1";

enum class Link { ocusync_15k, lightbridge, ocusync_30k };

struct Estimate {
    Link link{};
    double ppm{}, sigma_ppm{};
    std::size_t blocks{};
    // Where the link's symbol grid sits, as the time from the capture's first sample to a symbol,
    // modulo the link's nominal symbol period (symbol_period_s): the phase of the blocks'
    // cyclic-prefix metric at the nominal symbol rate. Adding the capture's device time gives the
    // grid's position on the receiver clock, which frame_timing.hpp follows across captures.
    // Offline 21 Nov 2026 (Phantom 4, sweep sessions with X310 device time): positions from
    // visits on five centres 0.7-20 s apart lay on one line to 2.4-2.6 us rms over 33-120 s.
    // OcuSync (23 Nov 2026, training corpus, tools/drone_corpus_check.cpp): 0.06-0.4 us rms over
    // 1-13 s on 51 of 52 recordings of 15 kHz links (Mavic Pro, Mavic 2, Air 2, Air 2S, Mavic 3,
    // FPV, Mini 3, Mini 3 Pro) and 16 of 19 of 30 kHz links (Mini 4 Pro, Mini 5 Pro, Avata 2);
    // the line's slope matched DroneID to about 0.1 ppm.
    bool has_symbol_phase{};
    double symbol_phase_s{};
};
inline constexpr double lightbridge_symbol_s = 7e-3 / 68;
// OcuSync nominal symbol rates, 14 symbols per ms per 15 kHz of spacing (LTE/NR-style slots):
// measured from the cyclic-prefix metric's spectrum on the corpus, 14000 Hz on every 15 kHz link
// and 28000 Hz on every 30 kHz link (no per-model value).
inline constexpr double ocusync_15k_symbol_s = 1 / 14000., ocusync_30k_symbol_s = 1 / 28000.;
inline constexpr double symbol_period_s(Link link) {
    return link == Link::lightbridge ? lightbridge_symbol_s : link == Link::ocusync_30k ? ocusync_30k_symbol_s
                                                                                        : ocusync_15k_symbol_s;
}
inline const char* name(Link link) {
    return link == Link::lightbridge ? "lightbridge" : link == Link::ocusync_30k ? "ocusync_30k" : "ocusync_15k";
}

inline constexpr double sigma_scale = 0.5;
// Floors on sigma_ppm for blocks spanning 0.5 s, scaled by 0.5 s / span (the timing jitter does
// not accumulate, so the slope improves with the span). Lightbridge frame timing itself jitters
// (about 0.4 us rms per 50 ms): 0.512 s visits scattered 1.0-1.5 ppm, and weak Phantom captures
// reported 0.7 while sitting 2 ppm off; the same 24 blocks over 3 s recordings gave -3.8 to -4.1.
// 15 kHz OcuSync scattered 0.15-0.2 ppm per 0.2 s KU Leuven capture.
inline constexpr double lightbridge_min_sigma_ppm = 1.0, ocusync_min_sigma_ppm = 0.1, min_sigma_span_s = 0.5;
// Readings looser than this are not used to compare rows. Phantom 4 visits gave 0.7-1.7, a weak
// one 4.3; noise alone about 3.
inline constexpr double usable_sigma_ppm = 2.0;
inline bool usable(const Estimate& e) { return e.sigma_ppm > 0 && e.sigma_ppm <= usable_sigma_ppm; }

// `count` blocks of `block_len` samples laid end to end at `blocks`; starts[i] is block i's first
// sample within the capture. Needs at least 4 blocks. nullopt when the link's timing is not
// found or the rate is outside 20-25 MSps.
std::optional<Estimate> measure(Link link, const std::complex<float>* blocks, std::size_t block_len,
                                const std::size_t* starts, std::size_t count, double rate);
// The same, with each block anywhere in memory (a subset of a capture's blocks, starts in order).
std::optional<Estimate> measure(Link link, const std::complex<float>* const* blocks, std::size_t block_len,
                                const std::size_t* starts, std::size_t count, double rate);
}
