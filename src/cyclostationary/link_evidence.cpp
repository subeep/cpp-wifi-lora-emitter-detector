#include "link_evidence.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
void finite(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("nonfinite evidence input");
}
void check(std::vector<EvidenceCheck>& out, const char* name, double value, const char* op,
           double threshold, bool known = true) {
    finite(value); finite(threshold);
    out.push_back({name, op, value, threshold, known,
        known && (op[0] == '>' ? value >= threshold : value <= threshold)});
}
bool passes(const std::vector<EvidenceCheck>& checks) {
    return std::all_of(checks.begin(), checks.end(), [](const auto& c) { return !c.known || c.passed; });
}
void finish(WaveformEvidence& m, const LinkEvidence& evidence, bool measured, double edge_hz,
            bool extent_known) {
    m.pattern_consistent = measured && passes(m.checks);
    m.observed_quality_passed = evidence.observed_quality_passed;
    const auto& context = evidence.context;
    const bool known = context.usable_bandwidth_hz > 0 && extent_known;
    check(m.checks, "extent_plus_one_bin_hz", edge_hz, "<=", context.usable_bandwidth_hz / 2, known);
    m.passband_status = !known ? "unknown" : m.checks.back().passed ? "fits_declared_band" : "outside_declared_band";
    m.status = !measured ? "unavailable" : !m.pattern_consistent ? "no_match" :
        !m.observed_quality_passed ? "quality_rejected" : !known ? "passband_unverified" :
        !m.checks.back().passed ? "passband_rejected" : "experimental_match";
}
}
LinkEvidence assess_link_evidence(const SpectralFeatures& s, const std::vector<OfdmMeasurement>& ofdm,
                                 const std::vector<ChirpMeasurement>& chirps, const EvidenceContext& c) {
    finite(c.sample_rate_hz); finite(c.usable_bandwidth_hz); finite(c.full_scale_component_fraction);
    if (c.sample_rate_hz <= 0 || c.sample_rate_hz > 1e9 || c.usable_bandwidth_hz < 0 ||
        c.usable_bandwidth_hz > c.sample_rate_hz ||
        (c.full_scale_component_fraction != -1 && (c.full_scale_component_fraction < 0 || c.full_scale_component_fraction > 1)) ||
        ofdm.size() > 32 || chirps.size() > 3 || s.power_fraction.size() > 4096)
        throw std::invalid_argument("invalid evidence context or measurement count");
    LinkEvidence r; r.context = c;
    check(r.quality_checks, "spectral_status_measured", s.quality == "measured", ">=", 1);
    check(r.quality_checks, "spectral_frames", double(s.frames), ">=", 16);
    finite(s.variance_power);
    check(r.quality_checks, "positive_variance", s.variance_power > 0, ">=", 1);
    check(r.quality_checks, "dc_fraction", s.dc_fraction, "<=", .10);
    check(r.quality_checks, "strongest_bin_fraction", s.strongest_bin_fraction, "<=", .25);
    double edges = 0;
    for (std::size_t i = 0; i < s.power_fraction.size(); ++i) {
        finite(s.power_fraction[i]);
        if (s.power_fraction[i] < 0 || s.power_fraction[i] > 1) throw std::invalid_argument("invalid spectrum fraction");
        if (i < 4 || i + 4 >= s.power_fraction.size()) edges += s.power_fraction[i];
    }
    check(r.quality_checks, "spectrum_present", !s.power_fraction.empty(), ">=", 1);
    check(r.quality_checks, "outer_four_bins_power_fraction", edges, "<=", .05);
    check(r.quality_checks, "source_int16_full_scale_component_fraction", std::max(0.0, c.full_scale_component_fraction),
          "<=", .01, c.full_scale_component_fraction >= 0);
    r.observed_quality_passed = passes(r.quality_checks);
    finite(s.occupied_low_hz); finite(s.occupied_high_hz); finite(s.bin_hz);
    if (s.bin_hz < 0) throw std::invalid_argument("invalid spectral bin width");
    for (const auto& v : ofdm) {
        WaveformEvidence m; m.kind = "ofdm_cp_structure"; m.hypothesis = v.hypothesis.label;
        if (m.hypothesis.size() > 128) throw std::invalid_argument("oversized hypothesis label");
        check(m.checks, "train_symbols", double(v.train_symbols), ">=", 16);
        check(m.checks, "holdout_symbols", double(v.holdout_symbols), ">=", 16);
        check(m.checks, "train_cp_squared_coherence", v.train_prefix_coherence_squared, ">=", .5);
        check(m.checks, "holdout_cp_squared_coherence", v.holdout_prefix_coherence_squared, ">=", .5);
        check(m.checks, "holdout_outside_squared_coherence", v.holdout_outside_coherence_squared, "<=", .1);
        check(m.checks, "holdout_contrast", v.holdout_contrast, ">=", .4);
        check(m.checks, "holdout_symbol_squared_coherence", v.holdout_symbol_cyclic_coherence_squared, ">=", .005);
        const double edge = std::max(std::abs(s.occupied_low_hz), std::abs(s.occupied_high_hz)) + s.bin_hz;
        // Whole-window 99% PSD extent is a necessary geometry check only; CP
        // timing does not establish the channel width or separate emitters.
        finish(m, r, v.status == "measured", edge, !s.power_fraction.empty());
        r.candidates.push_back(std::move(m));
    }
    for (const auto& v : chirps) {
        WaveformEvidence m; m.kind = "linear_chirp_reference"; m.hypothesis = v.label;
        if (m.hypothesis.size() > 128) throw std::invalid_argument("oversized hypothesis label");
        finite(v.slope_hz_per_second); finite(v.nominal_sweep_hz); finite(v.frequency_mean_hz);
        finite(v.frequency_slope_hz_per_second); finite(v.frequency_rmse_hz);
        check(m.checks, "peak_squared_coherence", v.peak_coherence_squared, ">=", .8);
        check(m.checks, "first_half_squared_coherence", v.first_half_coherence_squared, ">=", .8);
        check(m.checks, "second_half_squared_coherence", v.second_half_coherence_squared, ">=", .8);
        check(m.checks, "half_energy_balance", v.half_energy_balance, ">=", .5);
        check(m.checks, "frequency_fit_measured", v.frequency_status == "measured", ">=", 1);
        check(m.checks, "phase_unwraps", double(v.phase_unwraps), "<=", 0);
        check(m.checks, "relative_slope_error", v.slope_hz_per_second != 0 ?
              std::abs(v.frequency_slope_hz_per_second / v.slope_hz_per_second - 1) : 1, "<=", .02);
        check(m.checks, "frequency_rmse_over_nominal_sweep", v.nominal_sweep_hz > 0 ?
              v.frequency_rmse_hz / v.nominal_sweep_hz : 1, "<=", .05);
        const double edge = std::abs(v.frequency_mean_hz) + std::max(0.0, v.nominal_sweep_hz) / 2 + s.bin_hz;
        finish(m, r, v.status == "measured", edge, v.frequency_status == "measured" && v.nominal_sweep_hz > 0);
        r.candidates.push_back(std::move(m));
    }
    return r;
}
} // namespace rfmon::cyclo
