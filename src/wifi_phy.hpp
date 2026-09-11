// Wi-Fi modulation classification: DSSS/CCK (802.11b-style) vs OFDM
// (802.11a/g/n/ac/ax-style).
//
// This deliberately never decodes anything. It runs two independent
// correlators directly against each WiFi channel's own already-
// captured per-channel IQ buffer (see config.hpp's scan_plan_for_band()
// - this project's existing finite-capture-per-scan-step model is kept
// as-is here; no new streaming/hopping architecture):
//   - a Barker-11 matched filter, looking for the periodic peak train
//     a real 802.11b DSSS/CCK preamble produces (one peak per chip-
//     sequence-length regardless of the preamble's DBPSK bit values,
//     since matched-filter magnitude doesn't care about a data-bit
//     sign flip).
//   - a Schmidl-Cox delayed-conjugate autocorrelator, looking for the
//     sustained near-1.0 plateau a repeated OFDM short training symbol
//     (L-STF) produces.
// Both always run on every WiFi channel capture, unconditionally - a
// correlator hit IS the detection signal here, not a downstream
// bandwidth-based gate (see PROJECT_STATUS.md / session notes on why:
// this project's own noise-floor-relative, contiguous-run energy
// detector could never reliably measure >=8MHz on real, moderate-SNR
// 2.4GHz traffic, so requiring that before even trying modulation
// classification meant the correlators never got a real chance to run
// at all). Occupied bandwidth is now purely descriptive, computed via
// estimate_occupied_bandwidth_hz() below only once a correlator has
// already confirmed a hit - never used to gate detection.

#pragma once

#include <complex>
#include <vector>

namespace rfmon::wifi {

enum class ModClass { Unknown, DSSS, OFDM };

struct ModClassification {
    ModClass mod = ModClass::Unknown;
    // Whichever correlator won, normalized so >= 1.0 means "fired" -
    // not a probability, just a consistent way for the caller to pick
    // the best result across several sub-captures of the same segment.
    double confidence = 0.0;
};

// Mixes `iq` (captured at `sample_rate_hz`, centered on the tuned
// frequency) down so the candidate at `freq_offset_hz` away from that
// tuned center lands at 0 Hz, decimates for correlator cost, and runs
// both correlators against the result. `try_dsss` should be false for
// 5GHz captures - Barker/CCK/DSSS does not exist in that band, so
// skipping it there both saves time and rules out a whole class of
// impossible false positives.
ModClassification classify_modulation(const std::vector<std::complex<float>>& iq,
                                       double sample_rate_hz, double capture_center_hz,
                                       double segment_center_hz, bool try_dsss);

// Peak-relative, non-contiguous occupied-bandwidth estimate, computed
// directly from raw time-domain IQ - no baseband mixing needed, since
// it's purely self-referential to wherever the signal's own peak sits.
// Adopted from a separate working prototype that measured real Wi-Fi
// channel widths this project's own detector.cpp::find_segments()
// (noise-floor-relative threshold, contiguous-run requirement, shared
// with the LoRa/sub-GHz path and NOT touched by this) couldn't reliably
// recover: a small FFT (coarser bins smooth over per-subcarrier fades)
// and a threshold relative to the signal's OWN peak - not an
// independently-estimated noise floor, which can itself be biased
// upward once a wide, weak signal already occupies much of the capture
// - report the full span between the outermost bins that clear it,
// tolerant of interior dips rather than requiring one contiguous run.
//
// `threshold_db` is relative to the peak and therefore negative (e.g.
// the default -6.0 means "the -6dB points", matching the prototype this
// was adopted from) - a positive value can never be crossed, since
// nothing exceeds the peak itself.
double estimate_occupied_bandwidth_hz(const std::complex<float>* x, size_t n,
                                       double sample_rate_hz, double threshold_db = -6.0);

// Mean power of raw IQ samples, same "10*log10(power)" family as
// spectrum.cpp's PSD - lets a correlator-confirmed detection compete on
// roughly comparable terms in DeviceRegistry's per-cycle "highest
// peak_db wins" bucket resolution (registry.cpp's update_cycle()).
// Not calibrated to numerically match PSD-per-bin power exactly (a
// different measurement - per-FFT-bin density vs. a plain time-domain
// mean) - good enough for a same-cycle ranking heuristic; revisit if
// that assumption doesn't hold up in practice.
double estimate_mean_power_db(const std::complex<float>* x, size_t n);

}  // namespace rfmon::wifi
