#include "detector.hpp"

#include <algorithm>
#include <cmath>

namespace rfmon {

namespace {

// Matches numpy.percentile's default ('linear') interpolation.
double percentile(std::vector<double> values, double pct) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    double idx = (pct / 100.0) * (values.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return values[lo];
    double frac = idx - lo;
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

}  // namespace

std::vector<Segment> find_segments(const std::vector<double>& freqs_offset_hz,
                                    const std::vector<double>& psd_db, double tuned_center_hz,
                                    const std::vector<bool>& edge_mask,
                                    const std::vector<bool>& dc_mask, double noise_percentile,
                                    double threshold_db, int min_segment_bins,
                                    int merge_gap_bins) {
    size_t n = psd_db.size();
    std::vector<Segment> segments;
    if (n == 0) return segments;

    std::vector<double> trusted_psd;
    trusted_psd.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (edge_mask[i] && dc_mask[i]) trusted_psd.push_back(psd_db[i]);
    }
    if (trusted_psd.empty()) return segments;

    double noise_floor = percentile(trusted_psd, noise_percentile);
    double thresh = noise_floor + threshold_db;

    std::vector<bool> above_valid(n);
    bool any_above = false;
    for (size_t i = 0; i < n; ++i) {
        above_valid[i] = (psd_db[i] >= thresh) && edge_mask[i] && dc_mask[i];
        any_above = any_above || above_valid[i];
    }
    if (!any_above) return segments;

    std::vector<bool> passable = above_valid;

    // Bridge the DC-guard gap only if real evidence flanks both sides.
    // (Edge-excluded bins are never bridged - passable simply stays
    // false there, same as for any other untrusted/absent evidence.)
    long dc_start = -1, dc_end = -1;
    for (size_t i = 0; i < n; ++i) {
        if (!dc_mask[i]) {
            if (dc_start < 0) dc_start = static_cast<long>(i);
            dc_end = static_cast<long>(i) + 1;
        }
    }
    if (dc_start >= 0) {
        bool left_ok = dc_start > 0 && above_valid[dc_start - 1];
        bool right_ok = static_cast<size_t>(dc_end) < n && above_valid[dc_end];
        if (left_ok && right_ok) {
            for (long i = dc_start; i < dc_end; ++i) passable[i] = true;
        }
    }

    // Merge small below-threshold (but trusted) gaps, e.g. spectral notches.
    std::vector<size_t> above_idx;
    for (size_t i = 0; i < n; ++i)
        if (above_valid[i]) above_idx.push_back(i);
    for (size_t k = 0; k + 1 < above_idx.size(); ++k) {
        size_t gap = above_idx[k + 1] - above_idx[k];
        if (gap > 1 && gap <= static_cast<size_t>(merge_gap_bins) + 1) {
            for (size_t i = above_idx[k] + 1; i < above_idx[k + 1]; ++i) passable[i] = true;
        }
    }

    // Find contiguous runs of passable bins.
    size_t i = 0;
    while (i < n) {
        if (!passable[i]) {
            ++i;
            continue;
        }
        size_t s = i;
        while (i < n && passable[i]) ++i;
        size_t e = i;  // run spans [s, e)

        int above_count = 0;
        for (size_t k = s; k < e; ++k)
            if (above_valid[k]) ++above_count;
        if (above_count < min_segment_bins) continue;

        double weight_sum = 0.0, weighted_freq_sum = 0.0, peak_db = -1e18;
        for (size_t k = s; k < e; ++k) {
            if (!above_valid[k]) continue;
            double w = std::pow(10.0, psd_db[k] / 10.0);
            weight_sum += w;
            weighted_freq_sum += freqs_offset_hz[k] * w;
            peak_db = std::max(peak_db, psd_db[k]);
        }
        double center_offset_hz = weighted_freq_sum / weight_sum;
        // Bandwidth spans the full run (bridge included where
        // applicable) - the real occupied extent, even though the
        // bridged bins' own power readings aren't used above.
        double bandwidth_hz = freqs_offset_hz[e - 1] - freqs_offset_hz[s];

        segments.push_back(Segment{tuned_center_hz + center_offset_hz,
                                    std::max(bandwidth_hz, 1.0), peak_db});
    }
    return segments;
}

}  // namespace rfmon
