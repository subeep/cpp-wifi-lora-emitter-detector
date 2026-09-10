// Threshold-based segmentation of a spectrum into candidate emitter segments.

#pragma once

#include <vector>

namespace rfmon {

struct Segment {
    double center_hz;      // absolute center frequency
    double bandwidth_hz;
    double peak_db;
};

// Find contiguous runs of bins that sit `threshold_db` above the noise
// floor (estimated as a low percentile of the trusted spectrum), merge
// runs separated by small gaps, grow each qualifying run outward through
// a lower-confidence `hysteresis_low_db` skirt, and return one Segment
// per run that's at least `min_segment_bins` wide at the high threshold.
//
// The two thresholds solve different problems. `threshold_db` alone
// (the old behavior) only merges tiny below-any-threshold notches
// within a run via `merge_gap_bins` - it can't recover a real but only
// moderately strong signal (a distant Wi-Fi AP's OFDM subcarriers, say)
// that clears the noise floor across its whole occupied width but only
// clears the *high* threshold in scattered peaks, since bridging that
// with `merge_gap_bins` alone would mean blindly bridging gaps of
// dozens to hundreds of bins - which would just as readily splice
// together unrelated nearby emitters. Growing outward through bins that
// still clear a *lower* threshold only extends a segment through
// genuinely elevated, measured power, not blind bridging - a
// hysteresis/Canny-style dual threshold, seeded by the strong core and
// grown by the weaker skirt.
//
// Two independent exclusion masks are handled differently:
// - `edge_mask` (false near the Nyquist edges - filter rolloff) is a
//   hard boundary; a segment cannot extend into it (growth stops there
//   too, since neither above-threshold array is ever true there).
// - `dc_mask` (false around the tuned center - LO leakage) can be
//   bridged: a real emitter wide enough to span the tuned center would
//   otherwise get sliced into two segments by that gap, so it's merged
//   into one, but ONLY when there's an above-threshold reading
//   immediately on both sides of it. A signal merely adjacent to one
//   side of the gap is left bounded at the gap's edge.
std::vector<Segment> find_segments(const std::vector<double>& freqs_offset_hz,
                                    const std::vector<double>& psd_db, double tuned_center_hz,
                                    const std::vector<bool>& edge_mask,
                                    const std::vector<bool>& dc_mask, double noise_percentile,
                                    double threshold_db, int min_segment_bins, int merge_gap_bins,
                                    double hysteresis_low_db);

}  // namespace rfmon
