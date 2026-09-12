// RF fingerprint extraction - Tier-1, preamble-only, LoRa first.
//
// Scope (deliberately narrow - see PROJECT_STATUS.md / session notes
// for the full spec this is a scoped-down first pass of):
//   - cfo_ppm (oscillator group)
//   - irr_db, iq_eps, iq_phi_deg, dc_dbc, dc_ang_deg (quadrature/mixer
//     group - "the stable core" per the source spec)
//   - snr_db, evm_pct, sync_corr, n_samp (quality covariates - always
//     recorded, never used as identity features themselves)
//
// Deliberately NOT implemented yet, and why:
//   - PA nonlinearity (a3/a1, a5/a1, Saleh AM/AM-AM/PM, ACPR): needs
//     the actual payload as a fitting reference (a chirp's envelope is
//     nearly constant - no amplitude variation to characterize a PA
//     against). TarangMini's header/CRC never validates in this
//     project, so there is no decoded payload to use.
//   - chirp_rate_err_ppm / dechirp_resid_deg: the source spec gives
//     this one line, not a fully worked formula, and it's entangled
//     with our own integer-bin CFO estimate's quantization error
//     (+/-0.5 bin) in a way that isn't easy to cleanly separate out.
//     Lower priority than the group above per the source spec's own
//     tiering; deferred rather than shipped half-confident.
//   - clk_ratio, thermal correction, templates, stability weights,
//     conformal scoring: all need multi-session/multi-day data this
//     project hasn't collected yet - the source spec itself warns
//     against computing stability weights from a single session.
//
// This works from the preamble alone - every field here is computable
// whether or not the header/payload ever decodes, which matters a lot
// given TarangMini specifically never does.

#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace rfmon::fingerprint {

constexpr double LORA_SNR_FLOOR_DB = 10.0;  // source spec's LoRa floor (§5)
// Fit-quality gates - a high SNR alone doesn't mean the (SF, BW)
// hypothesis was actually correct; these catch a fit that technically
// ran but doesn't explain the captured signal (see the gate's own
// comment in extract_lora_fingerprint()'s .cpp for why this was added).
//
// These are NOT the source spec's original §5 numbers (10%/0.95) -
// those were tried first and rejected essentially every real TarangMini
// capture, even ones later confirmed correct by directly reading the
// module's own live config (config_read cmd 0x0008) and controlling
// for decimation (BW500, no decimation needed, vs BW125, 4x decimated -
// both showed the same EVM floor). That controlled testing traced the
// remaining error to TarangMini's own transmitted signal not perfectly
// matching the idealized chirp this fit assumes (most likely PA/chirp
// non-linearity or a multipath-heavy test environment) - consistent
// with this same module's header/CRC never validating in this project
// either. These thresholds are instead calibrated directly from that
// controlled data: confirmed-correct real bursts landed at 25-73% EVM
// / 0.68-0.97 sync_corr, while genuine wrong-hypothesis noise (from
// testing all 24 SF/BW combos against one capture) ran markedly worse
// (80-100%+ EVM, sync_corr well under 0.7) - the values below sit
// just outside the confirmed-real cluster, not at the spec's generic
// numbers, which assumed a cleaner transmitter than this one.
constexpr double LORA_EVM_CEILING_PCT = 75.0;
constexpr double LORA_SYNC_CORR_FLOOR = 0.65;

// Real preambles run 8-40+ symbols (see TARANGMINI_LORA_FINDINGS.md).
// Originally added on the theory that a long fit window lets CFO
// drift/phase-noise accumulate into unmodeled phase error - controlled
// testing later showed EVM stayed flat regardless of window size, so
// that theory didn't pan out (the real remaining error is elsewhere,
// see LORA_EVM_CEILING_PCT's comment). Left in place anyway since a
// shorter window is still strictly cheaper and no less accurate for
// the parameters that matter (confirmed via the synthetic tests) - not
// load-bearing for the gate outcome the way it was first thought to be.
constexpr int LORA_FP_MAX_FIT_SYMBOLS = 10;

struct LoraFingerprint {
    // Tier-1 oscillator - ppm, never Hz (a fingerprint stored in Hz
    // isn't comparable across carriers/bands - see source spec Rule 1).
    double cfo_ppm = 0.0;

    // Tier-1 quadrature/mixer group - the stable core.
    double irr_db = 0.0;
    double iq_eps = 0.0;
    double iq_phi_deg = 0.0;
    double dc_dbc = 0.0;
    double dc_ang_deg = 0.0;

    // Quality covariates - recorded every time, never features
    // themselves (source spec §1.6/Rule 2: store the measurement AND
    // its conditions, never just a score).
    double snr_db = 0.0;
    double evm_pct = 0.0;
    double sync_corr = 0.0;
    int n_samp = 0;

    // Gating (source spec §5/§2.2: "record rejections, don't silently
    // drop them" - a gated-out attempt is still written to the log,
    // just with the `p` fields null and a reason in `gate_reason`).
    bool gated_out = false;
    std::string gate_reason;
};

// Extracts a Tier-1 LoRa fingerprint from the preamble alone.
// `iq` must be at sample_rate == bandwidth_hz (this project's existing
// codec convention - see lora_phy.hpp). `start_sample`/`preamble_len`
// locate the locked preamble within `iq` (from BurstDetection or
// Decoded*Packet - both already carry these). `cfo_bins` is the same
// integer-bin CFO estimate the codec itself already produces.
LoraFingerprint extract_lora_fingerprint(const std::vector<std::complex<float>>& iq, int sf,
                                          double bandwidth_hz, double center_hz,
                                          long start_sample, int preamble_len, int cfo_bins);

// Appends one NDJSON record for this fingerprint attempt to `path`
// (created if missing, opened in append mode and closed on every call -
// slightly more I/O overhead than holding an open handle, but the
// source spec's own case for NDJSON is exactly this: append-only writes
// are atomic per line, so a crash mid-session loses nothing but a
// truncated last line). Field names and resolutions match the source
// spec's §1 tables where it defines them, so this stays extensible
// toward the fuller manifest/schema/template system later without a
// rename; `dev`/`algo`/`sess`/`crc` wrapper fields are intentionally
// omitted for now since there's no device-identity or algorithm-
// versioning concept built yet - see this file's header for scope.
void append_fingerprint_record(const std::string& path, const std::string& time_hhmmss,
                                double freq_mhz, int sf, double bandwidth_khz,
                                const LoraFingerprint& fp);

}  // namespace rfmon::fingerprint
