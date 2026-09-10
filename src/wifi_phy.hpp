// Wi-Fi modulation classification: DSSS/CCK (802.11b-style) vs OFDM
// (802.11a/g/n/ac/ax-style), from energy-detected candidates only.
//
// This deliberately never decodes anything. It runs two independent
// correlators against a candidate emitter's own slice of an already-
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
// Both always run on every WiFi-bandwidth candidate (never picking one
// detector first) - cheap enough given these only run on segments the
// existing bandwidth-based detector has already flagged as candidates,
// not on every raw sample.

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

}  // namespace rfmon::wifi
