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
// runs separated by small gaps, and return one Segment per run that's
// at least `min_segment_bins` wide.
//
// Two independent exclusion masks are handled differently:
// - `edge_mask` (false near the Nyquist edges - filter rolloff) is a
//   hard boundary; a segment cannot extend into it.
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
                                    double threshold_db, int min_segment_bins,
                                    int merge_gap_bins);

}  // namespace rfmon
