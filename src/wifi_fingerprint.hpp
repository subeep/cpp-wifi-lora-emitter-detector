// RF fingerprint extraction for WiFi - a SEPARATE engine from the LoRa
// one (fingerprint.hpp/.cpp), deliberately: different PHY, different
// preamble structures (L-STF/L-LTF for OFDM, Barker SYNC for DSSS),
// and a different dominant corruption source (multipath frequency-
// selective fading is WiFi's biggest problem, barely a factor for
// LoRa's narrowband chirp). The quadrature/mixer group's underlying
// MATH (widely-linear IQ model r' = mu*s + nu*conj(s) + c, solved via
// the same 3x3 normal-equations fit) is the same FORMULA as
// fingerprint.cpp's, reimplemented independently here rather than
// shared - matching this project's established convention for already-
// validated math consumed by a new caller (see lora_phy_std.cpp's file
// header for the same pattern: duplicating ~100 validated lines rather
// than risking a shared refactor).
//
// Scope (first pass, per the WiFi fingerprinting research plan):
//   - cfo_ppm: coarse (Schmidl-Cox phase(P), from wifi_phy.cpp) + fine
//     (L-LTF delay-and-conjugate, Moose-style) for OFDM; despread-
//     symbol phase-difference over the SYNC field for DSSS.
//   - irr_db, iq_eps, iq_phi_deg, dc_dbc, dc_ang_deg: widely-linear fit
//     against a known reference - the standard 802.11 L-LTF frequency-
//     domain training sequence (evaluated as a time-domain waveform at
//     the capture's own sample rate) for OFDM, or a re-integrated BPSK
//     sequence built from the SYNC field's own differential decisions
//     for DSSS (see wifi_dsss_rx.hpp's DsssDecodeResult::sync_symbols).
//   - snr_db, evm_pct, sync_corr, n_samp: quality covariates, same
//     shape and same non-identity role as LoRa's (source spec Rule 2:
//     store the measurement and its conditions, never just a score).
//
// Deliberately NOT implemented yet (see the research plan's
// excluded_features for the full reasoning):
//   - Spectral regrowth on L-STF/L-LTF null subcarriers, DSSS
//     despreading-gain loss: both flagged exploratory - lower
//     confidence, need more new correlator-internals plumbing than
//     what's built here.
//   - PA nonlinearity (needs payload amplitude diversity this project's
//     no-MAC-decode OFDM scope doesn't reach), turn-on transients
//     (needs triggered near-field capture, not this project's passive
//     wideband model).
//
// All numeric gate thresholds below are STARTING values, explicitly
// NOT yet calibrated against real captured WiFi hardware - see
// fingerprint.hpp's LORA_EVM_CEILING_PCT for why that discipline
// matters here too: the source spec's generic LoRa numbers rejected
// nearly every real TarangMini capture until recalibrated from actual
// measured data, and there is no reason to expect a WiFi generic
// number to fare any better. Revisit once real captures exist.

#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "wifi_dsss_rx.hpp"
#include "wifi_phy.hpp"

namespace rfmon::wifi_fingerprint {

// Starting SNR floor - same NUMBER as LoRa's (LORA_SNR_FLOOR_DB) for
// now, in the same "well above where a fit becomes meaningless" spirit
// - not derived from any WiFi-specific measurement yet.
constexpr double WIFI_SNR_FLOOR_DB = 10.0;

// Starting fit-quality gates. Deliberately looser than LoRa's own
// recalibrated numbers (fingerprint.hpp's LORA_EVM_CEILING_PCT=75.0,
// LORA_SYNC_CORR_FLOOR=0.65): WiFi's L-STF/L-LTF (8us+8us) and even
// DSSS's SYNC field (~128us) are one to three orders of magnitude
// shorter than LoRa's tens-of-symbols, often multi-millisecond
// preamble, so materially higher single-burst variance is expected
// even from a genuinely clean signal. NOT calibrated against real WiFi
// hardware - see this file's header.
constexpr double WIFI_EVM_CEILING_PCT = 80.0;
constexpr double WIFI_SYNC_CORR_FLOOR = 0.5;

struct WifiFingerprint {
    // Oscillator group - ppm, never Hz (not comparable across channels
    // otherwise), matching fingerprint.hpp's LoraFingerprint::cfo_ppm.
    double cfo_ppm = 0.0;

    // Quadrature/mixer group - the stable core, same five fields and
    // same units as LoraFingerprint.
    double irr_db = 0.0;
    double iq_eps = 0.0;
    double iq_phi_deg = 0.0;
    double dc_dbc = 0.0;
    double dc_ang_deg = 0.0;

    // Quality covariates - recorded whenever the fit actually ran, even
    // on a gated-out attempt (source spec's own "record rejections,
    // don't silently drop them", carried over from the LoRa side).
    double snr_db = 0.0;
    double evm_pct = 0.0;
    double sync_corr = 0.0;
    int n_samp = 0;

    bool gated_out = false;
    std::string gate_reason;
};

// Extracts a fingerprint from an OFDM burst's L-STF/L-LTF preamble.
//
// `cap`/`cap_len` is the FULL per-channel capture this burst was found
// in (i.e. what wifi::detect_bursts() was called against), NOT the
// already-sliced burst-only window scanner.cpp builds for
// classify_modulation() - this function needs samples immediately
// BEFORE the burst for its own pre-burst noise-power measurement,
// mirroring extract_lora_fingerprint()'s identical requirement.
// `burst_start`/`burst_length` locate the burst within `cap` (straight
// from wifi::BurstWindow). `mc` must be classify_modulation()'s own
// result for this same burst, with mc.mod == ModClass::OFDM and
// mc.has_preamble_range true - `mc.l_stf_start`/`l_ltf_start` etc. are
// indices into the burst-only window (see ModClassification's header
// comment for why that's safe to use directly against `cap` too: add
// `burst_start` to translate). `channel_center_hz` is the real channel
// frequency (for cfo_ppm's Hz->ppm conversion), matching how the
// scanner already recovers it from WIFI_CHANNEL_CAPTURE_OFFSET_HZ.
WifiFingerprint extract_ofdm_fingerprint(const std::complex<float>* cap, size_t cap_len,
                                          size_t burst_start, size_t burst_length,
                                          double sample_rate_hz, double capture_center_hz,
                                          double segment_center_hz, double channel_center_hz,
                                          const wifi::ModClassification& mc);

// Extracts a fingerprint from a DSSS burst's SYNC field. `dec` must be
// wifi::decode_dsss_burst()'s own result for this burst, with
// dec.preamble_found and a non-empty dec.sync_symbols.
//
// No raw pre-burst window is needed here (unlike the OFDM version):
// dec.sync_symbols are already Barker-despread (~10.4dB processing
// gain baked in via an 11-sample coherent sum), so comparing their
// power directly against a RAW pre-burst noise sample would overstate
// SNR by roughly that processing gain. Instead, snr_db here is a
// self-referential proxy - the despread symbols' own magnitude
// consistency (mean^2/variance): a genuine constant-envelope BPSK lock
// clusters tightly, incoherent noise does not. Distinct in kind from
// extract_ofdm_fingerprint()'s/LoRa's true pre-burst-noise-power SNR -
// flagged as a starting approximation pending refinement, same
// discipline as this file's gate constants.
WifiFingerprint extract_dsss_fingerprint(double channel_center_hz,
                                          const wifi::DsssDecodeResult& dec);

}  // namespace rfmon::wifi_fingerprint
