#include "cyclostationary/iq_file.hpp"
#include "cyclostationary/analysis_samples.hpp"
#include "cyclostationary/spectral_correlation.hpp"
#include "cyclostationary/ofdm_structure.hpp"
#include "cyclostationary/process_client.hpp"
#include "cyclostationary/burst_selection.hpp"
#include "cyclostationary/chirp_refinement.hpp"
#include "cyclostationary/candidate_bands.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace rfmon::cyclo;
namespace {
using Options = std::map<std::string, std::string>;
constexpr const char* usage =
    "Offline measurements only; no radio, GUI, payload decoder or trained classifier.\n"
    "wifi_cyclo_replay analyze --input FILE --format cf32_le|ci16_le\n"
    "  --sample-rate HZ [--center HZ] [--offset SAMPLES] [--samples N]\n"
    "  [--fft N] [--hop N] [--frames N] [--alpha-bins N] [--include-spectrum] [--measure-rois] [--measure-chirps]\n"
    "  [--roi-offset HZ --roi-bandwidth HZ --source-bandwidth HZ --decimate N]\n"
    "  [--remove-source-dc] (requires all band-selection options)\n"
    "  [--refine-chirps] (offline-only dechirp research; <=65536 analysis samples)\n"
    "  [--refine-clock] [--analyze-bursts] [--expand-waveforms] [--check-cyclic-background] [--discover-sweeps] [--discover-structure] [--review-waveforms] [--prepare-candidates] (<=65536 samples; candidate filtering needs declared usable bandwidth)\n"
    "  [--assess-evidence] [--usable-bandwidth HZ] (declared profile, not hardware verification)\n"
    "  [--qualify-candidates] (requires --prepare-candidates; separate coverage/support audit)\n"
    "  [--ofdm-useful N --ofdm-prefix N] (explicit timing replaces WLAN timing bank)\n"
    "  --measure-rois/--measure-chirps require <=65536 analysis samples after optional filtering.\n"
    "wifi_cyclo_replay inventory --root DIRECTORY --format cf32_le|ci16_le\n"
    "  --sample-rate HZ [--center HZ] [--expected-samples N] [--probe-samples N]\n"
    "wifi_cyclo_replay shadow-replay --input FILE --format cf32_le|ci16_le\n"
    "  --sample-rate HZ --capture-samples N [--offset N] [--center HZ]\n"
    "  [--continuous-samples N] [--burst-hints FILE.json] [--include-spectrum] [--usable-bandwidth HZ]\n"
    "  Uses the isolated worker and distributed live copy budget; native rate only.\n"
    "All output is JSON on stdout; errors go to stderr. Originals remain read-only.\n";

std::uint64_t uint_value(const std::string& text, const std::string& name) {
    std::uint64_t v = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), v);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid integer for " + name);
    return v;
}
double real_value(const std::string& text, const std::string& name) {
    std::size_t used = 0;
    const double v = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(v))
        throw std::invalid_argument("invalid finite number for " + name);
    return v;
}
std::string required(const Options& o, const std::string& key) {
    auto i = o.find(key);
    if (i == o.end()) throw std::invalid_argument("missing " + key);
    return i->second;
}
std::uint64_t integer(const Options& o, const std::string& key, std::uint64_t fallback) {
    auto i = o.find(key);
    return i == o.end() ? fallback : uint_value(i->second, key);
}
json metadata(const Options& o, IqFormat format, double rate) {
    json j = {{"format", format_name(format)}, {"sample_rate_hz", rate},
              {"parameters_source", "caller_declared_not_inferred_from_file"},
              {"timing", "relative_sample_coordinates_only"},
              {"continuity", "not_verified"}};
    j["capture_center_hz"] = nullptr;
    if (o.count("--center")) {
        auto f = real_value(o.at("--center"), "--center");
        if (f < 0 || f > 1e11) throw std::invalid_argument("invalid RF center");
        j["capture_center_hz"] = f;
    }
    return j;
}

double declared_bandwidth(const Options& o, double rate) {
    if (!o.count("--usable-bandwidth")) return 0;
    const double width = real_value(o.at("--usable-bandwidth"), "--usable-bandwidth");
    if (width <= 0 || width > rate) throw std::invalid_argument("usable bandwidth must be in (0, sample rate]");
    return width;
}
double source_rails(const std::vector<std::complex<float>>& iq, IqFormat format,
                    std::size_t first, std::size_t count) {
    if (format != IqFormat::ci16_le) return -1;
    if (!count || first > iq.size() || count > iq.size() - first) return -1;
    std::size_t rails = 0;
    for (std::size_t i = first; i < first + count; ++i) {
        rails += std::abs(iq[i].real()) >= 32767.0f / 32768;
        rails += std::abs(iq[i].imag()) >= 32767.0f / 32768;
    }
    return double(rails) / (2 * count);
}
json evidence_json(const LinkEvidence& e) {
    auto checks = [](const std::vector<EvidenceCheck>& cs) {
        json j = json::array();
        for (const auto& c : cs) j.push_back({{"name", c.name}, {"comparison", c.comparison},
            {"value", c.known ? json(c.value) : json(nullptr)}, {"threshold", c.threshold},
            {"known", c.known}, {"passed", c.known ? json(c.passed) : json(nullptr)}});
        return j;
    };
    json j = {{"policy_version", evidence_policy_version}, {"thresholds_calibrated", false},
        {"threshold_origin", "development_informed_heuristics_frozen_before_v1_benchmark"},
        {"named_family_acceptance_enabled", false}, {"drone_identity_validated", false},
        {"receiver_quality_verified", false}, {"mixture_separation_established", false},
        {"repetition_confirmed", false}, {"independent_unit_session_validation", false},
        {"context", {{"sample_rate_hz", e.context.sample_rate_hz},
            {"usable_bandwidth_hz", e.context.usable_bandwidth_hz > 0 ? json(e.context.usable_bandwidth_hz) : json(nullptr)},
            {"passband_source", e.context.usable_bandwidth_hz > 0 ? "caller_declared_or_private_filter" : "unknown"},
            {"int16_rails_known", e.context.full_scale_component_fraction >= 0},
            {"clipping_from_float_amplitude_inferred", false}}},
        {"observed_quality_passed", e.observed_quality_passed},
        {"quality_checks", checks(e.quality_checks)}, {"candidates", json::array()}};
    for (const auto& m : e.candidates) j["candidates"].push_back({{"kind", m.kind}, {"hypothesis", m.hypothesis},
        {"status", m.status}, {"pattern_consistent", m.pattern_consistent},
        {"observed_quality_passed", m.observed_quality_passed}, {"passband_status", m.passband_status},
        {"checks", checks(m.checks)}});
    return j;
}


json review_json(const WaveformReview& r,const char* method=waveform_review_version) {
    auto checks=[](const std::vector<EvidenceCheck>& cs){json list=json::array();
        for(const auto& c:cs)list.push_back({{"name",c.name},{"value",c.known?json(c.value):json(nullptr)},
            {"comparison",c.comparison},{"threshold",c.threshold},{"known",c.known},{"passed",c.known?json(c.passed):json(nullptr)}});
        return list;};
    json j={{"method",method},{"status",r.status},{"classification_used",false},
        {"thresholds_calibrated",false},{"coherence_is_probability",false},{"named_family_acceptance_enabled",false},
        {"receiver_passband_verified",false},{"cp_code_ambiguous",r.cp_code_ambiguous},
        {"observed_quality_passed",r.observed_quality_passed},{"quality_checks",checks(r.quality_checks)},
        {"context",{{"sample_rate_hz",r.context.sample_rate_hz},
            {"usable_bandwidth_hz",r.context.usable_bandwidth_hz>0?json(r.context.usable_bandwidth_hz):json(nullptr)},
            {"source_int16_rails_known",r.context.full_scale_component_fraction>=0},
            {"passband_status",r.passband_status},{"extent_plus_bin_hz",r.extent_plus_bin_hz}}},
        {"candidates",json::array()}};
    for(const auto& p:r.candidates)j["candidates"].push_back({{"kind",p.kind},{"label",p.label},
        {"measurement_index",p.measurement_index},{"status",p.status},{"pattern_consistent",p.pattern_consistent},
        {"specificity",p.kind=="cp_timing" || p.kind=="barker_11"?"experimental_structure":
            p.kind=="two_frequency" || p.kind=="two_level_envelope" || p.kind=="linear_sweep"?"shape_diagnostic":"cyclic_diagnostic"},
        {"checks",checks(p.checks)},{"reasons",p.reasons}});
    return j;
}

json chirp_json(const std::vector<ChirpMeasurement>& measurements, double rate, std::uint64_t first_source, std::size_t step) {
    json j = {{"method", "frequency_shifted_lag_correlation_v1"},
              {"hypothesis_source", "https://doi.org/10.3390/s25154552"},
              {"analysis_sample_rate_hz", rate}, {"source_sample_step", step},
              {"mean_removed", true}, {"normalization", "peak_magnitude_after_complex_mean_removal"},
              {"relative_half_energy_floor", 1e-10},
              {"peak_selection", "maximum_over_all_full_window_positions_per_hypothesis"},
              {"split_checks_independent_of_peak_selection", false},
              {"multiple_testing_calibrated", false}, {"coherence_is_probability", false},
              {"whole_signal_passband_verified", false}, {"vendor_or_drone_identity", false},
              {"periodicity_measured", false}, {"calibrated_detection_threshold", nullptr},
              {"frequency_check", "phase_increment_linear_fit_over_selected_peak_span"},
              {"frequency_check_independent_data", false}, {"frequency_unwrapping_proves_no_aliasing", false},
              {"candidates", json::array()}};
    for (const auto& m : measurements) {
        json c = {{"label", m.label}, {"status", m.status}, {"slope_hz_per_second", m.slope_hz_per_second},
                  {"nominal_sweep_hz", m.nominal_sweep_hz}, {"lag_samples", m.lag_samples},
                  {"gate_samples", m.gate_samples}, {"lag_seconds", m.lag_samples / rate},
                  {"positions_examined", m.positions_examined}, {"positions_eligible", m.positions_eligible},
                  {"peak_window_offset_samples", nullptr}, {"first_original_source_sample_center", nullptr},
                  {"last_original_source_sample_center", nullptr}, {"peak_coherence_squared", nullptr},
                  {"first_half_coherence_squared", nullptr}, {"second_half_coherence_squared", nullptr},
                  {"half_energy_balance", nullptr}};
        c.update({{"frequency_status", m.frequency_status}, {"frequency_pairs", m.frequency_pairs},
                  {"phase_unwraps", m.phase_unwraps}, {"frequency_slope_hz_per_second", nullptr}, {"frequency_rmse_hz", nullptr}, {"frequency_mean_offset_hz", nullptr}});
        if (m.frequency_status == "measured") c.update({{"frequency_slope_hz_per_second", m.frequency_slope_hz_per_second},
                                                       {"frequency_rmse_hz", m.frequency_rmse_hz}, {"frequency_mean_offset_hz", m.frequency_mean_hz}});
        if (m.status == "measured") c.update({{"peak_window_offset_samples", m.peak_offset},
            {"first_original_source_sample_center", first_source + m.peak_offset * step},
            {"last_original_source_sample_center", first_source + (m.peak_offset + m.lag_samples + m.gate_samples - 1) * step},
            {"peak_coherence_squared", m.peak_coherence_squared}, {"first_half_coherence_squared", m.first_half_coherence_squared},
            {"second_half_coherence_squared", m.second_half_coherence_squared}, {"half_energy_balance", m.half_energy_balance}});
        j["candidates"].push_back(c);
    }
    return j;
}
json refinement_json(const std::vector<ChirpRefinement>& measurements, double rate,
                     std::uint64_t first_source, std::size_t step, double width, double spectral_bin_hz) {
    json j = {{"method", chirp_refinement_version}, {"analysis_sample_rate_hz", rate}, {"offline_only", true}, {"live_worker_uses_refinement", false},
        {"classification_used", false}, {"named_family_acceptance_enabled", false}, {"thresholds_calibrated", false},
        {"selected_max_is_significance", false}, {"source_separation_established", false},
        {"analogue_passband_verified", false}, {"repetition_confirmed", false},
        {"selection_and_checks_share_data", true}, {"slope_factors", {.998,.999,1.0,1.001,1.002}},
        {"coarse_lag_seconds", 8e-6}, {"span_seconds_nominal", 64e-6},
        {"echo_band_allowance_seconds", 2e-6}, {"extra_band_bins", 4},
        {"shape_thresholds", {{"full_band_fraction_min", .8}, {"half_band_fraction_min", .7}, {"half_energy_balance_min", .25}}},
        {"source_sample_step", step}, {"candidates", json::array()}};
    for (std::size_t i=0;i<measurements.size();++i) {
        const auto& r=measurements[i];
        const double sweep = i==1 ? 18e6 : 9e6;
        const double edge = std::abs(r.estimated_span_mid_frequency_hz)+sweep/2+spectral_bin_hz;
        j["candidates"].push_back({{"label", r.label}, {"status", r.status}, {"shape_consistent", r.shape_consistent},
            {"nominal_slope_hz_per_second", r.nominal_slope_hz_per_second},
            {"selected_slope_hz_per_second", r.selected_slope_hz_per_second}, {"span_samples", r.span_samples},
            {"positions_examined", r.positions_examined}, {"spectra_examined", r.spectra_examined},
            {"first_original_source_sample_center", r.status=="measured" ? json(first_source+r.peak_offset*step) : json(nullptr)},
            {"last_original_source_sample_center", r.status=="measured" ? json(first_source+(r.peak_offset+r.span_samples-1)*step) : json(nullptr)},
            {"coarse_coherence_squared", r.coarse_coherence_squared},
            {"dechirped_band_power_fraction", r.dechirped_band_power_fraction},
            {"first_half_band_power_fraction", r.first_half_band_power_fraction},
            {"second_half_band_power_fraction", r.second_half_band_power_fraction},
            {"half_energy_balance", r.half_energy_balance}, {"dechirped_band_center_hz", r.dechirped_band_center_hz},
            {"dechirped_band_width_hz", r.dechirped_band_width_hz},
            {"estimated_span_mid_frequency_hz", r.estimated_span_mid_frequency_hz},
            {"extent_plus_margin_hz", edge}, {"passband_check_margin_hz", spectral_bin_hz},
            {"passband_geometry_status", width<=0 || r.status!="measured" ? "unknown" :
                edge<=width/2 ? "fits_declared_band" : "outside_declared_band"}});
        if (r.status!="measured") for (const char* field : {"coarse_coherence_squared", "dechirped_band_power_fraction",
            "first_half_band_power_fraction", "second_half_band_power_fraction", "half_energy_balance",
            "dechirped_band_center_hz", "dechirped_band_width_hz", "estimated_span_mid_frequency_hz",
            "selected_slope_hz_per_second", "extent_plus_margin_hz"}) j["candidates"].back()[field]=nullptr;
    }
    return j;
}

json background_json(const CyclicBackground& r,const WaveformFeatures& w,double rate,std::uint64_t first,std::size_t step) {
    auto part=[](const BackgroundPartition& p){return json{{"reference_bins",p.reference_bins},
        {"local_median_coherence_squared",p.median_coherence_squared},{"local_upper_coherence_squared",p.upper_coherence_squared},
        {"line_to_local_median",p.line_to_median},{"line_to_local_upper",p.line_to_upper},
        {"block_phase_coherence_squared",p.block_phase_coherence_squared},{"min_block_product_energy_fraction",p.min_block_energy_fraction}};};
    json j={{"method",cyclic_background_version},{"status",r.status},{"samples_examined",r.samples_examined},
        {"partition_samples",r.partition_samples},{"holdout_offset",r.holdout_offset},{"fft_calls",r.fft_calls},
        {"analysis_sample_rate_hz",rate},{"source_sample_step",step},{"classification_used",false},
        {"thresholds_calibrated",false},{"receiver_background_calibrated",false},{"multiple_searches_corrected",false},
        {"coherence_or_line_ratio_is_probability",false},{"selected_rates_or_lags_refitted",false},
        {"partition_means","separate"},{"reference_scope","same_sign_coarse_grid_4_to_20_bins_from_selected_coarse_peak"},
        {"block_scope","four_contiguous_portions_of_the_same_Hann_weighted_partition_not_independent_repetitions"},
        {"search_bounds",{{"max_extra_fft_calls",16},{"reference_bins_max",34},{"reference_bins_min",16},
            {"blocks_per_partition",4},{"line_ratio_floor",1e-12}}},
        {"heuristics",{{"raw_coherence_squared_min",.05},{"line_to_median_min",16},{"line_to_upper_min",8},
            {"block_phase_coherence_squared_min",.8},{"min_block_product_energy_fraction",.02}}},
        {"discovery_first_original_sample",nullptr},{"discovery_last_original_sample",nullptr},
        {"holdout_first_original_sample",nullptr},{"holdout_last_original_sample",nullptr},{"peaks",json::array()}};
    if(r.partition_samples) {
        j["discovery_first_original_sample"]=first;j["discovery_last_original_sample"]=first+(r.partition_samples-1)*step;
        j["holdout_first_original_sample"]=first+r.holdout_offset*step;j["holdout_last_original_sample"]=first+(r.holdout_offset+r.partition_samples-1)*step;
    }
    for(std::size_t i=0;i<r.peaks.size();++i)j["peaks"].push_back({{"measurement_index",i},{"conjugate",w.peaks[i].conjugate},
        {"lag_samples",w.peaks[i].lag_samples},{"alpha_hz",w.peaks[i].alpha_hz},
        {"raw_persistent_pattern",w.peaks[i].discovery_coherence_squared>=.05 && w.peaks[i].holdout_coherence_squared>=.05},
        {"discovery",part(r.peaks[i].discovery)},{"holdout",part(r.peaks[i].holdout)},
        {"background_supported",r.peaks[i].background_supported}});
    return j;
}

json waveform_json(const WaveformFeatures& r,double rate,std::uint64_t first_source,std::size_t step) {
    json j={{"method",waveform_features_version},{"samples_examined",r.samples_examined},{"analysis_sample_rate_hz",rate},
        {"source_sample_step",step},{"classification_used",false},{"vendor_or_drone_identity",false},{"thresholds_calibrated",false},
        {"frequency_reference","current_analysis_center"},
        {"mixture_status","unresolved"},{"cyclic_status",r.cyclic_status},{"morphology_status",r.morphology_status},
        {"partition_samples",r.partition_samples},{"partition_limit",cyclic_partition_limit},{"holdout_offset_samples",r.holdout_offset},
        {"alpha_bin_hz",r.partition_samples?json(r.alpha_bin_hz):json(nullptr)},{"minimum_coarse_cycles",9},{"lags_samples",{0,1,4,16}},
        {"partition_means","separate"},{"window","periodic_hann_on_lag_product"},{"fft_calls",r.fft_calls},
        {"refinement_evaluations",r.refinement_evaluations},{"refinement_offsets_bins",{-.5,-.25,0,.25,.5}},
        {"selection","discovery_only_holdout_does_not_refit"},{"coherence_is_probability",false},
        {"multiple_searches_corrected",false},{"persistence_threshold_squared",.05},{"cyclic_peaks",json::array()},
        {"discovery_first_original_sample",nullptr},{"discovery_last_original_sample",nullptr},
        {"holdout_first_original_sample",nullptr},{"holdout_last_original_sample",nullptr}};
    if(r.partition_samples) {
        j["discovery_first_original_sample"]=first_source; j["discovery_last_original_sample"]=first_source+(r.partition_samples-1)*step;
        j["holdout_first_original_sample"]=first_source+r.holdout_offset*step;
        j["holdout_last_original_sample"]=first_source+(r.holdout_offset+r.partition_samples-1)*step;
    }
    for(const auto& p:r.peaks) j["cyclic_peaks"].push_back({{"kind",p.conjugate?"conjugate":"ordinary"},{"lag_samples",p.lag_samples},
        {"coarse_alpha_hz",p.coarse_alpha_hz},{"alpha_hz",p.alpha_hz},{"discovery_coherence_squared",p.discovery_coherence_squared},
        {"holdout_coherence_squared",p.holdout_coherence_squared},{"persistent_pattern",p.persistent_pattern},
        {"pairs_per_partition",r.partition_samples-p.lag_samples}});
    j["morphology"]={{"power_scope","raw_analysis_copy_no_mean_removal"},{"phase_pairs",r.phase_pairs},
        {"amplitude_cv",r.amplitude_cv},{"envelope_low_relative_to_mean_power",r.envelope_low},{"envelope_high_relative_to_mean_power",r.envelope_high},
        {"envelope_high_fraction",r.envelope_high_fraction},{"envelope_fit_residual",r.envelope_fit_residual},
        {"envelope_contrast",r.envelope_contrast},{"envelope_transitions",r.envelope_transitions},
        {"two_level_envelope_pattern",r.two_level_envelope_pattern},{"frequency_low_hz",r.frequency_low_hz},
        {"frequency_high_hz",r.frequency_high_hz},{"frequency_first_fraction",r.frequency_first_fraction},
        {"frequency_second_fraction",r.frequency_second_fraction},{"frequency_concentration",r.frequency_concentration},
        {"frequency_transitions",r.frequency_transitions},{"two_frequency_pattern",r.two_frequency_pattern},
        {"frequency_bins",256},{"frequency_neighborhood_bins",2},{"phase_power_floor_relative",1e-4},
        {"frequency_aliasing","phase_increments_modulo_sample_rate"},
        {"pattern_meaning","experimental_morphology_not_exact_modulation_or_drone"}};
    j["morphology"]["heuristics"]={{"envelope_quantiles",{.2,.8}},{"envelope_min_contrast",.7},{"envelope_max_fit_residual",.05},
        {"envelope_high_fraction_min",.1},{"envelope_high_fraction_max",.9},{"min_transitions",8},
        {"frequency_min_phase_support_fraction",.8},{"frequency_max_amplitude_cv",.3},{"frequency_min_concentration",.8},
        {"frequency_min_each_state_fraction",.1}};
    if(r.morphology_status!="measured") for(const char* key:{"amplitude_cv","envelope_low_relative_to_mean_power",
        "envelope_high_relative_to_mean_power","envelope_high_fraction","envelope_fit_residual","envelope_contrast",
        "frequency_low_hz","frequency_high_hz","frequency_first_fraction","frequency_second_fraction","frequency_concentration"}) j["morphology"][key]=nullptr;
    else if(!r.phase_pairs) for(const char* key:{"frequency_low_hz","frequency_high_hz","frequency_first_fraction","frequency_second_fraction","frequency_concentration"}) j["morphology"][key]=nullptr;
    const bool centres_available=r.morphology_status=="measured" && r.phase_pairs && r.frequency_second_fraction>0;
    j["morphology"]["frequency_state_centres_available"]=centres_available;
    if(!centres_available) {j["morphology"]["frequency_low_hz"]=nullptr;j["morphology"]["frequency_high_hz"]=nullptr;}
    return j;
}

json sweeps_json(const ChirpDiscovery& r,double rate,std::uint64_t first,std::size_t step) {
    json j={{"method",chirp_discovery_version},{"status",r.status},{"samples_examined",r.samples_examined},
        {"analysis_sample_rate_hz",rate},{"source_sample_step",step},{"partition_samples",r.partition_samples},
        {"holdout_partition_offset",r.holdout_partition_offset},{"discovery_trials",r.discovery_trials},{"discovery_continuation_trials",r.discovery_continuation_trials},
        {"selection","discovery_slope_and_span_frozen_held_position_search_carrier_nuisance_refit"},
        {"classification_used",false},{"vendor_or_drone_identity",false},{"thresholds_calibrated",false},
        {"multiple_searches_corrected",false},{"alias_recovery",false},{"complete_chirp_duration_estimated",false},
        {"search_bounds",{{"partition_limit",8192},{"span_samples",{64,128,256,512,1024,2048}},
            {"stride_fraction",.5},{"candidate_limit",3},{"phase_frequency_guard_fraction",.45},{"phase_frequency_average_samples",8},
            {"sweep_fraction_min",.04},{"sweep_fraction_max",.7},{"rmse_fraction_max",.003},
            {"coherence_squared_min",.65},{"amplitude_cv_max",.5}}},{"candidates",json::array()}};
    for(const auto& p:r.candidates) {
        json c={{"direction",p.slope_hz_per_second>0?"up":"down"},{"span_samples",p.span_samples},
            {"observed_span_us",1e6*p.span_samples/rate},{"slope_hz_per_second",p.slope_hz_per_second},
            {"observed_sweep_hz",p.sweep_hz},{"discovery_offset",p.discovery_offset},
            {"discovery_first_original_sample",first+p.discovery_offset*step},
            {"discovery_last_original_sample",first+(p.discovery_offset+p.span_samples-1)*step},
            {"discovery_center_hz",p.discovery_center_hz},{"discovery_rmse_fraction",p.discovery_rmse_fraction},
            {"discovery_coherence_squared",p.discovery_coherence_squared},{"discovery_amplitude_cv",p.discovery_amplitude_cv},
            {"holdout_supported",p.holdout_supported},{"holdout_trials",p.holdout_trials},{"pattern_consistent",p.pattern_consistent},
            {"holdout_offset",nullptr},{"holdout_first_original_sample",nullptr},{"holdout_last_original_sample",nullptr},
            {"holdout_center_hz",nullptr},{"holdout_rmse_fraction",nullptr},{"holdout_coherence_squared",nullptr},{"holdout_amplitude_cv",nullptr}};
        if(p.holdout_supported) {
            c["holdout_offset"]=p.holdout_offset;c["holdout_first_original_sample"]=first+p.holdout_offset*step;
            c["holdout_last_original_sample"]=first+(p.holdout_offset+p.span_samples-1)*step;
            c["holdout_center_hz"]=p.holdout_center_hz;c["holdout_rmse_fraction"]=p.holdout_rmse_fraction;
            c["holdout_coherence_squared"]=p.holdout_coherence_squared;c["holdout_amplitude_cv"]=p.holdout_amplitude_cv;
        }
        j["candidates"].push_back(std::move(c));
    }
    return j;
}

json structure_json(const StructureDiscovery& r,double rate,std::uint64_t first,std::size_t step) {
    json j={{"method",structure_discovery_version},{"status",r.status},{"samples_examined",r.samples_examined},
        {"analysis_sample_rate_hz",rate},{"source_sample_step",step},{"partition_samples",r.partition_samples},
        {"holdout_offset_samples",r.holdout_offset},{"partition_means","separate"},
        {"selection","discovery_only_holdout_does_not_refit"},{"classification_used",false},{"vendor_or_drone_identity",false},
        {"coherence_is_probability",false},{"thresholds_calibrated",false},{"multiple_searches_corrected",false},
        {"mixture_status","unresolved"},{"receiver_passband_verified",false},
        {"discovery_first_original_sample",nullptr},{"discovery_last_original_sample",nullptr},
        {"holdout_first_original_sample",nullptr},{"holdout_last_original_sample",nullptr},
        {"max_useful_lag_samples",r.max_lag},{"ofdm_fft_calls",r.ofdm_fft_calls},{"timing_hypotheses",r.timing_hypotheses},
        {"spread_hypotheses",r.spread_hypotheses},{"ofdm_candidates",json::array()},{"short_code_candidates",json::array()},
        {"search_bounds",{{"partition_limit",8192},{"ofdm_min_lag_samples",16},{"ofdm_max_lag_samples",1024},
            {"ofdm_max_lags",4},{"ofdm_max_prefixes_per_lag",8},{"ofdm_min_prefix_samples",4},
            {"ofdm_prefix_max_fraction",.5},{"min_complete_symbols_or_words",8},
            {"code_chip_samples_min",1},{"code_chip_samples_max",16},{"carrier_aliases",2},{"max_retained_per_kind",4}}},
        {"short_code_scope","fixed_Barker_11_compatibility_not_generic_DSSS_or_WiFi_identity"},
        {"spreading_code",{1,1,1,-1,-1,-1,1,-1,-1,1,-1}},
        {"carrier_search","discovery_squared_signal_lag1_moment_and_sample_rate_over_2_alias"},
        {"carrier_estimate_hz",r.status=="measured"?json(r.carrier_estimate_hz):json(nullptr)},
        {"carrier_coherence_squared",r.status=="measured"?json(r.carrier_coherence_squared):json(nullptr)},
        {"heuristics",{{"ofdm_min_prefix_coherence_squared",.5},{"ofdm_max_outside_coherence_squared",.1},
            {"ofdm_min_held_contrast",.4},{"ofdm_min_held_symbol_coherence_squared",.005},
            {"short_code_min_coherence_squared",.8},{"short_code_min_phase_contrast",.5}}}};
    if(r.partition_samples) {
        j["discovery_first_original_sample"]=first;j["discovery_last_original_sample"]=first+(r.partition_samples-1)*step;
        j["holdout_first_original_sample"]=first+r.holdout_offset*step;
        j["holdout_last_original_sample"]=first+(r.holdout_offset+r.partition_samples-1)*step;
    }
    for(const auto& p:r.ofdm)j["ofdm_candidates"].push_back({{"useful_samples",p.useful_samples},{"prefix_samples",p.prefix_samples},
        {"prefix_phase_samples",p.phase_samples},{"symbols_per_partition",p.symbols_per_partition},
        {"useful_time_us",1e6*p.useful_samples/rate},{"prefix_time_us",1e6*p.prefix_samples/rate},
        {"symbol_rate_hz",rate/(p.useful_samples+p.prefix_samples)},{"lag_coherence_squared",p.lag_coherence_squared},
        {"train_prefix_coherence_squared",p.train_prefix},{"train_outside_coherence_squared",p.train_outside},
        {"holdout_prefix_coherence_squared",p.holdout_prefix},{"holdout_outside_coherence_squared",p.holdout_outside},
        {"holdout_symbol_coherence_squared",p.holdout_cyclic},{"pattern_consistent",p.pattern_consistent}});
    for(const auto& p:r.spread)j["short_code_candidates"].push_back({{"chip_samples",p.chip_samples},{"chip_rate_hz",rate/p.chip_samples},
        {"code_phase_samples",p.code_phase_samples},{"carrier_hz",p.carrier_hz},{"train_words",p.train_words},{"holdout_words",p.holdout_words},
        {"train_code_coherence_squared",p.train_code_coherence_squared},{"holdout_code_coherence_squared",p.holdout_code_coherence_squared},
        {"train_other_phase_coherence_squared",p.train_other_phase},{"holdout_other_phase_coherence_squared",p.holdout_other_phase},
        {"pattern_consistent",p.pattern_consistent}});
    return j;
}

json clock_json(ClockRefinement r,const SpectralFeatures& quality,double rate,std::uint64_t first,
    std::size_t step,const EvidenceContext& context,const char* scope) {
    review_clock_refinement(r,quality,context);
    json j={{"method",clock_refinement_version},{"status",r.status},{"scope",scope},
        {"input_origin_original_sample",first},{"input_original_sample_step",step},{"samples_examined",r.samples_examined},
        {"partition_samples",r.partition_samples},{"canonical_samples",r.target_samples},{"canonical_holdout_offset",r.holdout_offset},
        {"source_margin_samples",timing_source_margin},{"grid_ppm",timing_grid_ppm},{"grids_tested",r.grids_tested},
        {"cp_trials",r.cp_trials},{"code_trials",r.code_trials},{"interpolated_samples",r.interpolated_samples},
        {"max_seeds_per_kind",timing_seed_limit},{"max_retained_per_kind",1},{"holdout_selects_grid_or_phase",false},
        {"interpolation","linear_two_source_samples_no_padding_no_antialias_filter"},
        {"quality_scope","raw_selected_scope_PSD_before_timing_interpolation"},
        {"clock_grid_is_calibrated_clock_estimate",false},{"identity_accepted",false},
        {"cp_candidates",json::array()},{"code_candidates",json::array()},{"review",review_json(r.review)}};
    auto mapping=[&](std::size_t seed,std::size_t grid) {
        const auto scale=timing_grid_scale(grid);
        return json{{"seed_index",seed},{"grid_index",grid},{"selected_grid_ppm",timing_grid_ppm[grid]},
            {"source_samples_per_canonical_sample",scale},{"discovery_first_input_coordinate",double(timing_source_margin)},
            {"discovery_last_input_coordinate",timing_source_margin+scale*(r.partition_samples-1)},
            {"holdout_first_input_coordinate",timing_source_margin+scale*r.holdout_offset},
            {"holdout_last_input_coordinate",timing_source_margin+scale*(r.holdout_offset+r.partition_samples-1)}};
    };
    for(const auto& p:r.cp) {
        const auto& m=p.measurement;auto v=mapping(p.seed_index,p.grid_index);const auto factor=timing_grid_scale(p.grid_index);
        v.update({{"canonical_useful_samples",m.useful_samples},{"canonical_prefix_samples",m.prefix_samples},
            {"canonical_phase_samples",m.phase_samples},{"symbols_per_partition",m.symbols_per_partition},
            {"source_useful_samples",m.useful_samples*factor},{"source_prefix_samples",m.prefix_samples*factor},
            {"source_symbol_rate_hz",rate/(factor*(m.useful_samples+m.prefix_samples))},
            {"train_prefix_coherence_squared",m.train_prefix},{"train_outside_coherence_squared",m.train_outside},
            {"holdout_prefix_coherence_squared",m.holdout_prefix},{"holdout_outside_coherence_squared",m.holdout_outside},
            {"holdout_symbol_coherence_squared",m.holdout_cyclic},{"pattern_consistent",m.pattern_consistent}});
        j["cp_candidates"].push_back(v);
    }
    for(const auto& p:r.code) {
        const auto& m=p.measurement;auto v=mapping(p.seed_index,p.grid_index);
        v.update({{"canonical_chip_samples",m.chip_samples},{"source_chip_samples",m.chip_samples*timing_grid_scale(p.grid_index)},
            {"canonical_code_phase_samples",m.code_phase_samples},{"retimed_carrier_hz",m.carrier_hz},
            {"train_words",m.train_words},{"holdout_words",m.holdout_words},
            {"train_code_coherence_squared",m.train_code_coherence_squared},{"holdout_code_coherence_squared",m.holdout_code_coherence_squared},
            {"train_other_phase_coherence_squared",m.train_other_phase},{"holdout_other_phase_coherence_squared",m.holdout_other_phase},
            {"pattern_consistent",m.pattern_consistent}});
        j["code_candidates"].push_back(v);
    }
    return j;
}

json burst_json(BurstAnalysis b,double rate,std::uint64_t first,std::size_t step,const EvidenceContext& context) {
    const auto& s=b.selection;review_burst(b,context);
    json j={{"method",burst_analysis_version},{"status",s.status},{"eligible_retained_regions",s.eligible_regions},
        {"short_retained_regions",s.short_regions},{"budget_skipped_retained_regions",s.budget_skipped_regions},
        {"max_regions_per_tile",1},{"sample_limit_per_tile",burst_sample_limit},{"minimum_region_samples",burst_min_samples},
        {"selection","strongest_retained_eligible_energy_region_earliest_tie_central_crop"},
        {"selection_bias_calibrated",false},{"phase_concatenation",false},{"source_separation",false},
        {"identity_accepted",false},{"samples_examined",s.samples},{"crop_applied",s.cropped},
        {"source_first_original_sample",nullptr},{"source_last_original_sample",nullptr}};
    if(!s.samples)return j;
    first+=s.offset*step;j["region_index"]=s.region_index;j["source_first_original_sample"]=first;
    j["source_last_original_sample"]=first+(s.samples-1)*step;j["original_sample_step"]=step;
    j["local_quality"]={{"status",b.spectral.quality},{"frames",b.spectral.frames},{"samples_used",b.spectral.samples_used},
        {"bin_hz",b.spectral.bin_hz},{"dc_fraction",b.spectral.dc_fraction},{"strongest_bin_fraction",b.spectral.strongest_bin_fraction},
        {"occupied_low_hz",b.spectral.occupied_low_hz},{"occupied_high_hz",b.spectral.occupied_high_hz},
        {"warnings",b.spectral.warnings},{"scope","local_crop_PSD_and_quality_not_whole_tile"}};
    j["waveform_features"]=waveform_json(b.waveform,rate,first,step);
    j["cyclic_background"]=background_json(b.background,b.waveform,rate,first,step);
    j["structure_discovery"]=structure_json(b.structure,rate,first,step);
    j["linear_sweep_discovery"]=sweeps_json(b.sweeps,rate,first,step);
    j["waveform_review"]=review_json(b.review);
    j["cyclic_background_review"]=review_json(b.background_review,background_review_version);
    j["linear_sweep_review"]=review_json(b.sweep_review,sweep_review_version);
    return j;
}

json candidate_json(const CandidateBands& r,double rate,std::uint64_t first,std::size_t step,double width) {
    json j={{"offline_only",true},{"live_worker_prepares_candidates",false},{"source_usable_width_hz",width>0?json(width):json(nullptr)},
        {"passband_source","caller_declared_not_receiver_calibrated"},{"context_retained",true},{"source_mean_removed",false},
        {"source_separation",false},{"proposals_seen",r.proposals_seen},{"proposals_not_returned",r.proposals_seen-r.candidates.size()},
        {"selection","declared_geometry_eligible_then_energy_priority"},{"candidate_limit",prepared_candidate_limit},
        {"fir_operations",r.fir_operations},{"fir_operation_limit",64000000},{"candidates",json::array()}};
    for(const auto& c:r.candidates) {
        json p={{"status",c.status},{"source_region_first_sample",first+c.source_offset*step},
            {"source_region_samples",c.source_samples},{"source_region_samples_unit","input_analysis_samples"},
            {"source_region_original_sample_step",step},{"source_region_last_original_sample",first+(c.source_offset+c.source_samples-1)*step},
            {"frequency_offset_hz",c.offset_hz},{"passband_width_hz",c.width_hz},
            {"analysis_sample_rate_hz",nullptr},{"filter_taps",c.filter_taps},{"output_samples",c.output_samples}};
        if(c.status=="prepared_declared_passband") {
            p["analysis_sample_rate_hz"]=c.output_rate_hz;
            p["first_original_source_sample_center"]=first+c.first_input_center*step;
            p["last_original_source_sample_center"]=first+(c.first_input_center+(c.output_samples-1)*c.input_step)*step;
            p["original_source_sample_step"]=c.input_step*step;
            p["waveform_features"]=waveform_json(c.waveform,c.output_rate_hz,first+c.first_input_center*step,c.input_step*step);
        }
        j["candidates"].push_back(p);
    }
    j["source_analysis_sample_rate_hz"]=rate; return j;
}
json candidate_qualification_json(const CandidateBands& r,const RoiMeasurements& roi,
                                  std::uint64_t first,std::size_t step,std::size_t upstream_half) {
    // Time extents are unions, not sums over potentially overlapping bands.
    using Span=std::pair<std::size_t,std::size_t>; // half-open analysis coordinates
    auto extent=[](std::vector<Span> spans) {
        std::sort(spans.begin(),spans.end());std::size_t count=0,end=0;
        for(auto [a,b]:spans) {if(b>end) count+=b-std::max(a,end);end=std::max(end,b);}
        return count;
    };
    std::vector<Span> retained,prepared,filter_inputs,centers;
    for(const auto& region:roi.regions) retained.emplace_back(region.offset,region.offset+region.samples);
    json j={{"method","candidate_coverage_support_v1"},{"offline_only",true},{"classification_used",false},
        {"receiver_passband_calibrated",false},{"source_separation_established",false},
        {"signal_detection_recall",nullptr},{"coverage_meaning","time_extent_only_not_signal_or_frequency_coverage"},
        {"analysis_samples",roi.samples_examined},{"original_sample_step",step},
        {"filter_input_coordinates","immediate_input_sample_centers_mapped_to_original"},
        {"upstream_filter_half_length_original_samples",upstream_half},
        {"retained_regions",r.regions_seen},{"regions_without_spectrum",r.regions_without_spectrum},
        {"short_regions_with_spectrum",r.short_regions},{"proposals_seen",r.proposals_seen},
        {"geometry_eligible_proposals",r.geometry_eligible_proposals},
        {"returned_proposals",r.candidates.size()},{"duplicates_of_returned",r.duplicates_of_returned},
        {"budget_omitted_proposals",r.budget_omitted},
        {"proposal_accounting_exact",r.proposals_seen==r.candidates.size()+r.duplicates_of_returned+r.budget_omitted},
        {"raw_context_retained",true},{"candidates",json::array()}};
    std::size_t prepared_count=0,cyclic_supported=0;
    for(const auto& c:r.candidates) {
        json p={{"region_index",c.region_index},{"proposal_kind",c.proposal_kind},{"preparation_status",c.status},
            {"cyclic_support_available",false},{"first_filter_input_original_sample",nullptr},
            {"last_filter_input_original_sample",nullptr},{"first_output_original_center",nullptr},
            {"first_raw_dependency_original_sample",nullptr},{"last_raw_dependency_original_sample",nullptr},
            {"last_output_original_center",nullptr},{"left_center_margin_analysis_samples",nullptr},
            {"right_center_margin_analysis_samples",nullptr},{"unconsumed_tail_analysis_samples",nullptr},
            {"blocking_reasons",json::array()}};
        if(c.status=="prepared_declared_passband") {
            ++prepared_count;
            const auto half=c.filter_taps/2,last=c.first_input_center+(c.output_samples-1)*c.input_step;
            const auto support_first=c.first_input_center-half,support_end=last+half+1;
            prepared.emplace_back(c.source_offset,c.source_offset+c.source_samples);
            filter_inputs.emplace_back(support_first,support_end);centers.emplace_back(c.first_input_center,last+1);
            p["first_filter_input_original_sample"]=first+support_first*step;
            p["last_filter_input_original_sample"]=first+(support_end-1)*step;
            p["first_raw_dependency_original_sample"]=first+support_first*step-upstream_half;
            p["last_raw_dependency_original_sample"]=first+(support_end-1)*step+upstream_half;
            p["first_output_original_center"]=first+c.first_input_center*step;
            p["last_output_original_center"]=first+last*step;
            p["left_center_margin_analysis_samples"]=c.first_input_center-c.source_offset;
            p["right_center_margin_analysis_samples"]=c.source_offset+c.source_samples-1-last;
            p["unconsumed_tail_analysis_samples"]=c.source_offset+c.source_samples-support_end;
            p["output_sample_step_original"]=c.input_step*step;
            p["cyclic_support_available"]=c.waveform.cyclic_status=="measured";
            p["cyclic_status"]=c.waveform.cyclic_status;
            if(c.waveform.cyclic_status=="measured") ++cyclic_supported;
            else p["blocking_reasons"].push_back("prepared_cyclic_"+c.waveform.cyclic_status);
        } else p["blocking_reasons"].push_back(c.status);
        p["blocking_reasons"].push_back("receiver_passband_unqualified");
        p["blocking_reasons"].push_back("link_signature_unvalidated");
        j["candidates"].push_back(std::move(p));
    }
    j["prepared_candidates"]=prepared_count;j["cyclic_supported_candidates"]=cyclic_supported;
    j["retained_region_time_union_samples"]=extent(retained);
    j["prepared_region_time_union_samples"]=extent(prepared);
    j["filter_input_time_union_samples"]=extent(filter_inputs);
    j["output_center_extent_union_samples"]=extent(centers);
    j["samples_outside_prepared_region_union"]=roi.samples_examined-extent(prepared);
    return j;
}
json roi_json(const RoiMeasurements& r, double rate, std::uint64_t first_source, std::size_t step, const json& center) {
    json j = {{"method", "block_energy_and_distributed_hann_psd_v1"}, {"status", r.status},
              {"samples_examined", r.samples_examined}, {"analysis_sample_rate_hz", rate},
              {"block_samples", roi_block_samples}, {"block_count", r.blocks},
              {"block_time_resolution_seconds", roi_block_samples / rate},
              {"regions_seen", r.regions_seen}, {"regions_omitted", r.regions_seen > roi_region_limit ? r.regions_seen - roi_region_limit : 0},
              {"contrast_samples", r.contrast_samples}, {"contrast_mask_seconds", r.contrast_samples / rate},
              {"dc_fraction_removed", r.dc_fraction}, {"background_to_mean", r.background_to_mean},
              {"high_threshold_to_mean", r.high_threshold_to_mean},
              {"background_proxy", "20th_percentile_block_energy_not_calibrated_noise"},
              {"mean_removal", "whole_window_envelope_per_region_psd"},
              {"envelope_low_high_multipliers", {2, 4}}, {"spectral_low_high_multipliers", {4, 8}},
              {"spectral_fft_size", roi_fft_size}, {"spectral_frame_limit", roi_frame_limit},
              {"spectral_frame_positions", "evenly_distributed_nonoverlapping"},
              {"frequency_bin_hz", rate / roi_fft_size}, {"detection_or_drone_threshold", false},
              {"emitter_separation", "not_established"}, {"source_sample_step", step}, {"regions", json::array()}};
    for (const auto& region : r.regions) {
        const bool psd = region.spectral_status == "measured" || region.spectral_status == "few_frames";
        json item = {{"window_offset_samples", region.offset}, {"samples", region.samples},
                     {"first_original_source_sample_center", first_source + region.offset * step},
                     {"last_original_source_sample_center", first_source + (region.offset + region.samples - 1) * step},
                     {"duration_seconds", region.samples / rate}, {"energy_fraction", region.energy_fraction},
                     {"contrast_selected", region.contrast_selected}, {"touches_window_edge", region.touches_window_edge},
                     {"spectral_status", region.spectral_status}, {"spectral_frames", region.spectral_frames},
                     {"spectral_samples", region.spectral_samples},
                     {"spectral_sample_fraction", double(region.spectral_samples) / region.samples},
                     {"occupied_99pct_low_hz", psd ? json(region.occupied_low_hz) : json(nullptr)},
                     {"occupied_99pct_high_hz", psd ? json(region.occupied_high_hz) : json(nullptr)},
                     {"spectral_intervals_seen", region.bands_seen},
                     {"spectral_intervals_omitted", region.bands_seen > roi_band_limit ? region.bands_seen - roi_band_limit : 0},
                     {"spectral_intervals", json::array()}};
        for (const auto& b : region.bands) item["spectral_intervals"].push_back({
            {"low_offset_hz", b.low_hz}, {"high_offset_hz", b.high_hz}, {"power_fraction", b.power_fraction},
            {"rf_low_hz", center.is_null() ? json(nullptr) : json(center.get<double>() + b.low_hz)},
            {"rf_high_hz", center.is_null() ? json(nullptr) : json(center.get<double>() + b.high_hz)},
            {"touches_nyquist_bin", b.edge_bin}, {"contains_removed_dc_bin", b.contains_dc}});
        j["regions"].push_back(std::move(item));
    }
    return j;
}

BurstHints declared_hints(const Options& o, std::size_t prefix) {
    if (!o.count("--burst-hints")) return {};
    const auto path = fs::canonical(o.at("--burst-hints"));
    if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
        throw std::invalid_argument("burst hints must be a regular JSON file of at most 64 KiB");
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(65537, '\0'); stream.read(bytes.data(), bytes.size());
    const auto count = stream.gcount();
    if (stream.bad() || count > 65536) throw std::invalid_argument("burst hints read failed or exceeded limit");
    bytes.resize(std::size_t(count));
    const auto doc = json::parse(bytes);
    if (!doc.is_object() || !doc.contains("ranges") || !doc.at("ranges").is_array() ||
        doc.at("ranges").size() > burst_hint_limit ||
        (doc.contains("detector_capped") && !doc.at("detector_capped").is_boolean()))
        throw std::invalid_argument("expected ranges array (max 512) and optional boolean detector_capped");
    std::array<SampleTile, burst_hint_limit> ranges{};
    for (std::size_t i = 0; i < doc.at("ranges").size(); ++i) {
        const auto& b = doc.at("ranges")[i];
        if (!b.is_object() || !b.contains("start") || !b.contains("length") ||
            !b.at("start").is_number_unsigned() || !b.at("length").is_number_unsigned() ||
            b.at("start").get<std::uint64_t>() > std::numeric_limits<std::size_t>::max() ||
            b.at("length").get<std::uint64_t>() > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("hint start and length must be nonnegative sample integers");
        ranges[i] = {b.at("start").get<std::size_t>(), b.at("length").get<std::size_t>()};
    }
    return collect_burst_hints(doc.at("ranges").size(), prefix, doc.value("detector_capped", false),
                               [&](std::size_t i) { return ranges[i]; });
}

json shadow_replay(const Options& o, IqFormat format, double rate) {
    const double width = declared_bandwidth(o, rate);
    const auto first = read_iq_window(required(o, "--input"), format, integer(o, "--offset", 0), 1);
    const auto requested = uint_value(required(o, "--capture-samples"), "--capture-samples");
    if (!requested) throw std::invalid_argument("capture sample count must be positive");
    const auto capture_count = std::min(requested, first.file_samples - first.offset_samples);
    const auto continuous = integer(o, "--continuous-samples", capture_count);
    if (continuous > capture_count) throw std::invalid_argument("continuous sample count exceeds capture");
    const auto selection = select_sample_tiles(continuous, declared_hints(o, continuous));
    const auto& plan = selection.plan;
    if (!plan.count) throw std::invalid_argument("at least 2048 declared continuous samples required");
    std::vector<std::complex<float>> packed;
    packed.reserve(plan.samples);
    for (std::size_t t = 0; t < plan.count; ++t) {
        const auto& tile = plan.tiles[t];
        auto window = read_iq_window(first.path, format, first.offset_samples + tile.source_offset, tile.samples);
        if (window.samples.size() != tile.samples) throw std::runtime_error("file changed during distributed read");
        packed.insert(packed.end(), window.samples.begin(), window.samples.end());
    }
    ProcessClient client({}); client.start([] { return false; });
    const auto start = std::chrono::steady_clock::now();
    const auto measured = client.analyze(1, rate, plan, packed, [] { return false; });
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    json j = {{"schema", "rfmon.cyclo.shadow_replay.v1"}, {"input", metadata(o, format, rate)},
              {"classification", {{"waveform", "unknown"}, {"link_family", "unknown"},
                                    {"drone_assessment", "insufficient_evidence"}, {"model_status", "not_trained"}}},
              {"selection", {{"method", selection.burst_windows ? "raw_burst_and_context_tiles_v1" : "distributed_continuous_prefix_tiles_v1"},
                              {"hints_source", o.count("--burst-hints") ? "caller_declared_not_verified" : "none"},
                              {"hint_coordinates", "relative_to_capture_offset"},
                              {"hints_reported", selection.hints_reported}, {"hints_examined", selection.hints_examined},
                              {"hints_eligible", selection.hints_eligible}, {"hints_rejected", selection.hints_rejected},
                              {"detector_capped", selection.detector_capped}, {"burst_guided_windows", selection.burst_windows},
                              {"coverage_complete", false},
                              {"capture_samples_requested", requested}, {"capture_samples_available", capture_count},
                              {"continuous_samples_declared", continuous}, {"copied_samples", plan.samples},
                              {"tile_count", plan.count}, {"sample_budget", capture_sample_budget},
                              {"phase_concatenation", false}, {"total_observed_seconds", plan.samples / rate},
                              {"copied_fraction_of_capture", double(plan.samples) / capture_count}}},
              {"worker", {{"process_isolated", true}, {"pid", client.pid()}, {"processing_roundtrip_ms", elapsed},
                           {"deadline_ms", 2000}, {"address_space_limit_bytes", 128 * 1024 * 1024},
                           {"cpu_lifetime_limit_seconds", 30}, {"wire_version", wire_version}}},
              {"measurement", {{"fft_size", 512}, {"max_frames_per_tile", 256}, {"max_alpha_bins", 32},
                                {"coherence_is_probability", false}, {"statistical_detection_threshold", nullptr},
                                {"mixture_status", "not_estimated"}, {"native_rate_only", true}}},
              {"tiles", json::array()}};
    j["input"].update({{"path", first.path.string()}, {"file_bytes", first.file_bytes},
                        {"file_samples", first.file_samples}, {"capture_offset_samples", first.offset_samples},
                        {"file_integrity", "alignment_and_selected_windows_only"}});
    std::size_t packed_first = 0;
    for (std::size_t t = 0; t < measured.size(); ++t) {
        const auto& tile = measured[t];
        const auto& r = tile.spectral;
        json item = {{"source_offset_samples", tile.source.source_offset},
                     {"selection_origin", selection.burst_guided[t] ? "raw_burst_hint" : "capture_context"},
                     {"file_offset_samples", first.offset_samples + tile.source.source_offset},
                     {"samples", tile.source.samples}, {"status", r.quality}, {"warnings", r.warnings},
                     {"spectral_samples_used", r.samples_used}, {"spectral_frames", r.frames},
                     {"spectral_duration_seconds", r.samples_used / rate},
                     {"mean_power", r.mean_power}, {"dc_fraction", r.dc_fraction},
                     {"spectral_flatness", r.power_fraction.empty() ? json(nullptr) : json(r.spectral_flatness)},
                     {"cyclic_peaks", json::array()}, {"ofdm_candidates", json::array()}};
        for (const auto& p : r.peaks) item["cyclic_peaks"].push_back({{"alpha_hz", p.alpha_hz},
             {"frequency_offset_hz", p.frequency_offset_hz}, {"coherence_squared", p.coherence_squared},
             {"normalized_cross_magnitude", p.normalized_cross_magnitude}});
        for (const auto& m : tile.ofdm) {
            json candidate = {{"timing_label", m.hypothesis.label}, {"status", m.status},
                              {"useful_samples", m.hypothesis.useful_samples}, {"prefix_samples", m.hypothesis.prefix_samples},
                              {"symbol_rate_hz", m.symbol_rate_hz}, {"sample_span", m.samples_used},
                              {"train_symbols", m.train_symbols}, {"holdout_symbols", m.holdout_symbols},
                              {"holdout_prefix_coherence_squared", nullptr}, {"holdout_outside_coherence_squared", nullptr},
                              {"holdout_contrast", nullptr}, {"holdout_symbol_cyclic_coherence_squared", nullptr}};
            if (m.status == "measured") candidate.update({{"holdout_prefix_coherence_squared", m.holdout_prefix_coherence_squared},
                {"holdout_outside_coherence_squared", m.holdout_outside_coherence_squared}, {"holdout_contrast", m.holdout_contrast},
                {"holdout_symbol_cyclic_coherence_squared", m.holdout_symbol_cyclic_coherence_squared}});
            item["ofdm_candidates"].push_back(candidate);
        }
        if (o.count("--include-spectrum")) item["power_fraction"] = r.power_fraction;
        item["roi_measurements"] = roi_json(tile.roi, rate, first.offset_samples + tile.source.source_offset, 1,
                                           j["input"]["capture_center_hz"]);
        item["chirp_structure"] = chirp_json(tile.chirps, rate, first.offset_samples + tile.source.source_offset, 1);
        item["waveform_features"]=waveform_json(tile.waveform,rate,first.offset_samples+tile.source.source_offset,1);
        item["cyclic_background"]=background_json(tile.cyclic_background,tile.waveform,rate,first.offset_samples+tile.source.source_offset,1);
        item["cyclic_background_review"]=review_json(review_cyclic_background(r,tile.waveform,tile.cyclic_background,
            {rate,width,source_rails(packed,format,packed_first,tile.source.samples)}),background_review_version);
        item["linear_sweep_discovery"]=sweeps_json(tile.sweeps,rate,first.offset_samples+tile.source.source_offset,1);
        item["linear_sweep_review"]=review_json(review_linear_sweeps(tile.spectral,tile.sweeps,
            {rate,width,source_rails(packed,format,packed_first,tile.source.samples)}),sweep_review_version);
        item["structure_discovery"]=structure_json(tile.structure,rate,first.offset_samples+tile.source.source_offset,1);
        item["waveform_review"]=review_json(review_waveform_measurements(tile.spectral,tile.structure,tile.waveform,
            {rate,width,source_rails(packed,format,packed_first,tile.source.samples)}));
        item["dsp_evidence"] = evidence_json(assess_link_evidence(r, tile.ofdm, tile.chirps,
            {rate, width, source_rails(packed, format, packed_first, tile.source.samples)}));
        item["burst_analysis"]=burst_json(tile.burst,rate,first.offset_samples+tile.source.source_offset,1,
            {rate,width,source_rails(packed,format,packed_first+tile.burst.selection.offset,tile.burst.selection.samples)});
        item["burst_analysis"]["local_psd_transport"]="float32_fractions_other_metrics_float64";
        const bool clock_burst=tile.burst.selection.samples>0;
        item["clock_refinement"]=clock_json(tile.clock,clock_burst?tile.burst.spectral:tile.spectral,rate,
            first.offset_samples+tile.source.source_offset+tile.burst.selection.offset,1,
            {rate,width,source_rails(packed,format,packed_first+tile.burst.selection.offset,clock_burst?tile.burst.selection.samples:tile.source.samples)},
            clock_burst?"selected_contiguous_burst":"whole_tile");
        packed_first += tile.source.samples;
        j["tiles"].push_back(item);
    }
    return j;
}

json analyze(const Options& o, IqFormat format, double rate) {
    if(o.count("--qualify-candidates") && !o.count("--prepare-candidates"))
        throw std::invalid_argument("candidate qualification requires --prepare-candidates");
    auto input = read_iq_window(required(o, "--input"), format,
                                integer(o, "--offset", 0), integer(o, "--samples", 262144));
    const auto source_metadata = metadata(o, format, rate);
    PreparedSamples prepared;
    const bool select_band = o.count("--roi-offset") || o.count("--roi-bandwidth") ||
                             o.count("--source-bandwidth") || o.count("--decimate");
    if (o.count("--remove-source-dc") && !select_band)
        throw std::invalid_argument("source DC removal requires explicit band selection");
    if (o.count("--usable-bandwidth") && select_band)
        throw std::invalid_argument("use source-bandwidth and roi-bandwidth for filtered analysis, not usable-bandwidth");
    double evidence_width = declared_bandwidth(o, rate);
    json preprocessing = {{"band_selection", false}, {"sample_rate_hz", rate},
                          {"first_output_source_sample", input.offset_samples}, {"input_sample_step", 1},
                          {"analysis_center_hz", source_metadata["capture_center_hz"]}};
    if (select_band) {
        BandSelection b;
        b.sample_rate_hz = rate;
        b.source_usable_bandwidth_hz = real_value(required(o, "--source-bandwidth"), "--source-bandwidth");
        b.passband_width_hz = real_value(required(o, "--roi-bandwidth"), "--roi-bandwidth");
        b.offset_hz = real_value(required(o, "--roi-offset"), "--roi-offset");
        b.decimation = uint_value(required(o, "--decimate"), "--decimate");
        b.remove_source_mean = o.count("--remove-source-dc");
        prepared = select_analysis_band(input.samples, b);
        evidence_width = b.passband_width_hz;
        preprocessing.update({{"band_selection", true}, {"method", "hamming_sinc_integer_decimator_v1"},
                              {"source_usable_bandwidth_hz", b.source_usable_bandwidth_hz},
                              {"roi_offset_hz", b.offset_hz}, {"passband_width_hz", b.passband_width_hz},
                              {"stopband_edge_hz", prepared.stopband_edge_hz},
                              {"sample_rate_hz", prepared.sample_rate_hz},
                              {"output_samples", prepared.samples.size()},
                              {"filter_taps", prepared.filter_taps},
                              {"filter_half_length_source_samples", prepared.first_input_center},
                              {"first_output_source_sample", input.offset_samples + prepared.first_input_center},
                              {"input_sample_step", prepared.input_step},
                              {"filter_transients", "discarded_no_padding"},
                              {"source_mean_removed_before_downmix", b.remove_source_mean},
                              {"removed_source_mean_i", prepared.removed_source_mean.real()},
                              {"removed_source_mean_q", prepared.removed_source_mean.imag()}});
        if (!source_metadata["capture_center_hz"].is_null())
            preprocessing["analysis_center_hz"] = source_metadata["capture_center_hz"].get<double>() + b.offset_hz;
    }
    const auto& analysis_iq = select_band ? prepared.samples : input.samples;
    const double analysis_rate = select_band ? prepared.sample_rate_hz : rate;
    if ((o.count("--refine-clock") || o.count("--analyze-bursts") || o.count("--assess-evidence") || o.count("--refine-chirps") || o.count("--check-cyclic-background") || o.count("--discover-sweeps") || o.count("--review-waveforms") || o.count("--discover-structure") || o.count("--expand-waveforms") || o.count("--prepare-candidates")) && analysis_iq.size() > 65536)
        throw std::invalid_argument("evidence preview requires <=65536 analysis samples");
    SpectralConfig c;
    c.sample_rate_hz = analysis_rate;
    c.fft_size = integer(o, "--fft", 512);
    c.hop_samples = integer(o, "--hop", 0);
    c.max_frames = integer(o, "--frames", 1024);
    c.max_alpha_bins = integer(o, "--alpha-bins", 32);
    auto r = measure_spectral_correlation(analysis_iq, c);
    auto hypotheses = wlan_ofdm_hypotheses(analysis_rate);
    if (o.count("--ofdm-useful") || o.count("--ofdm-prefix"))
        hypotheses = {{"caller_supplied_timing", uint_value(required(o, "--ofdm-useful"), "--ofdm-useful"),
                       uint_value(required(o, "--ofdm-prefix"), "--ofdm-prefix")}};
    const auto ofdm = measure_ofdm_structure(analysis_iq, analysis_rate, hypotheses);
    std::vector<ChirpMeasurement> chirps;
    if (o.count("--measure-chirps") || o.count("--assess-evidence") || o.count("--refine-chirps")) chirps = measure_chirp_structure(analysis_iq, analysis_rate);
    std::vector<ChirpRefinement> refinement;
    if (o.count("--refine-chirps")) refinement = refine_chirp_candidates(analysis_iq, analysis_rate, chirps);
    RoiMeasurements roi;
    if (o.count("--refine-clock") || o.count("--analyze-bursts") || o.count("--measure-rois") || o.count("--prepare-candidates")) roi = measure_rois(analysis_iq, analysis_rate);
    json j = {{"schema", "rfmon.cyclo.features.v1"},
              {"estimator", "hann_phase_corrected_fft_cross_products_v1"},
              {"input", source_metadata}, {"preprocessing", preprocessing},
              {"status", r.quality}, {"warnings", r.warnings},
              {"classification", {{"waveform", "unknown"}, {"drone_assessment", "insufficient_evidence"},
                                   {"link_family", "unknown"}, {"model_status", "not_trained"}}}};
    j["input"].update({{"path", input.path.string()}, {"file_bytes", input.file_bytes},
                        {"file_samples", input.file_samples}, {"offset_samples", input.offset_samples},
                        {"samples_read", input.samples.size()},
                        {"file_integrity", "alignment_and_selected_window_only"}});
    const auto analysis_first=input.offset_samples+(select_band?prepared.first_input_center:0);
    const auto analysis_step=select_band?prepared.input_step:1;
    StructureDiscovery structure; WaveformFeatures waveform;
    const bool structure_requested=o.count("--refine-clock") || o.count("--review-waveforms") || o.count("--discover-structure") || o.count("--expand-waveforms") || o.count("--prepare-candidates");
    if(structure_requested) {
        structure=discover_waveform_structure(analysis_iq,analysis_rate);
        j["structure_discovery"]=structure_json(structure,analysis_rate,analysis_first,analysis_step);
    }
    if(o.count("--check-cyclic-background") || o.count("--review-waveforms") || o.count("--expand-waveforms") || o.count("--prepare-candidates")) {
        waveform=measure_waveform_features(analysis_iq,analysis_rate);
        j["waveform_features"]=waveform_json(waveform,analysis_rate,analysis_first,analysis_step);
    }
    if(structure_requested)j["waveform_review"]=review_json(review_waveform_measurements(r,structure,waveform,
        {analysis_rate,evidence_width,source_rails(input.samples,format,0,select_band?input.samples.size():r.samples_used)}));
    if(o.count("--check-cyclic-background") || o.count("--review-waveforms") || o.count("--expand-waveforms") || o.count("--prepare-candidates")) {
        const auto background=measure_cyclic_background(analysis_iq,analysis_rate,waveform);
        j["cyclic_background"]=background_json(background,waveform,analysis_rate,analysis_first,analysis_step);
        j["cyclic_background_review"]=review_json(review_cyclic_background(r,waveform,background,
            {analysis_rate,evidence_width,source_rails(input.samples,format,0,select_band?input.samples.size():r.samples_used)}),background_review_version);
    }
    if(o.count("--discover-sweeps") || o.count("--review-waveforms") || o.count("--expand-waveforms") || o.count("--prepare-candidates")) {
        const auto sweeps=discover_linear_sweeps(analysis_iq,analysis_rate);
        j["linear_sweep_discovery"]=sweeps_json(sweeps,analysis_rate,analysis_first,analysis_step);
        j["linear_sweep_review"]=review_json(review_linear_sweeps(r,sweeps,
            {analysis_rate,evidence_width,source_rails(input.samples,format,0,select_band?input.samples.size():r.samples_used)}),sweep_review_version);
    }
    BurstAnalysis b;
    if(o.count("--analyze-bursts") || o.count("--refine-clock")) b=analyze_burst(analysis_iq,analysis_rate,roi);
    if(o.count("--analyze-bursts")) {
        j["burst_analysis"]=burst_json(b,analysis_rate,analysis_first,analysis_step,
            {analysis_rate,evidence_width,source_rails(input.samples,format,select_band?0:b.selection.offset,
                select_band?input.samples.size():b.selection.samples)});
        j["burst_analysis"]["source_rail_scope"]=select_band?"entire_pre_filter_source_window":"local_native_burst";
    }
    if(o.count("--refine-clock")) {
        const auto& bs=b.selection;std::vector<std::complex<float>> local;
        if(bs.samples)local.assign(analysis_iq.begin()+bs.offset,analysis_iq.begin()+bs.offset+bs.samples);
        const auto refined=refine_structure_clock(bs.samples?local:analysis_iq,analysis_rate,bs.samples?b.structure:structure);
        j["clock_refinement"]=clock_json(refined,bs.samples?b.spectral:r,analysis_rate,analysis_first+bs.offset*analysis_step,analysis_step,
            {analysis_rate,evidence_width,source_rails(input.samples,format,select_band?0:bs.offset,
                select_band?input.samples.size():bs.samples?bs.samples:r.samples_used)},bs.samples?"selected_contiguous_burst":"whole_tile");
        j["clock_refinement"]["source_rail_scope"]=select_band?"entire_pre_filter_source_window":"raw_selected_scope";
    }
    if(o.count("--prepare-candidates")) {
        const auto candidates=prepare_candidate_bands(analysis_iq,analysis_rate,roi,evidence_width);
        j["candidate_preparation"]=candidate_json(candidates,analysis_rate,analysis_first,analysis_step,evidence_width);
        if(o.count("--qualify-candidates")) j["candidate_qualification"]=candidate_qualification_json(
            candidates,roi,analysis_first,analysis_step,select_band?prepared.filter_taps/2:0);
    }
    if (o.count("--measure-rois")) j["roi_measurements"] = roi_json(roi, analysis_rate,
        input.offset_samples + (select_band ? prepared.first_input_center : 0),
        select_band ? prepared.input_step : 1, preprocessing["analysis_center_hz"]);
    if (o.count("--measure-chirps") || o.count("--assess-evidence") || o.count("--refine-chirps")) j["chirp_structure"] = chirp_json(chirps, analysis_rate,
        input.offset_samples + (select_band ? prepared.first_input_center : 0), select_band ? prepared.input_step : 1);
    if (o.count("--refine-chirps")) j["chirp_refinement"] = refinement_json(refinement, analysis_rate,
        input.offset_samples + (select_band ? prepared.first_input_center : 0), select_band ? prepared.input_step : 1, evidence_width, r.bin_hz);
    j["measurement"] = {{"fft_size", r.fft_size}, {"hop_samples", r.hop_samples},
                          {"frames", r.frames}, {"samples_used", r.samples_used},
                          {"duration_seconds", r.samples_used / analysis_rate},
                          {"frequency_bin_hz", r.bin_hz}, {"alpha_grid_step_hz", r.bin_hz},
                          {"max_alpha_hz", c.max_alpha_bins * r.bin_hz},
                          {"alpha_zero_included_in_peaks", false},
                          {"alpha_domain", "positive_integer_bin_separations"},
                          {"mean_removed", r.samples_used > 0}, {"normalization", "unit_variance_before_periodic_hann"},
                          {"pair_power_floor_relative", c.pair_power_floor_relative},
                          {"coherence_definition", "squared_magnitude_normalized_cross_spectrum"},
                          {"coherence_is_probability", false},
                          {"analysis_region", "entire_selected_window_no_source_separation"},
                          {"frequency_reference", "analysis_center_hz"},
                          {"mixture_status", "not_estimated"},
                          {"statistical_detection_threshold", nullptr}};
    j["features"] = {{"mean_i", r.mean_i}, {"mean_q", r.mean_q},
                       {"mean_power", r.mean_power}, {"variance_power", r.variance_power},
                       {"dc_fraction", r.dc_fraction}, {"max_component_abs", r.max_component_abs},
                       {"spectral_flatness", r.spectral_flatness},
                       {"strongest_bin_fraction", r.strongest_bin_fraction},
                       {"occupied_low_offset_hz", r.occupied_low_hz},
                       {"occupied_high_offset_hz", r.occupied_high_hz},
                       {"occupied_bandwidth_99pct_hz", r.occupied_high_hz - r.occupied_low_hz},
                       {"burst_context", nullptr}, {"calibrated_snr_db", nullptr},
                       {"clipping_fraction", nullptr}, {"int16_full_scale_component_fraction", nullptr},
                       {"cyclic_peaks", json::array()}};
    if (!r.samples_used) {
        for (const auto& field : {"mean_i", "mean_q", "mean_power", "variance_power", "dc_fraction", "max_component_abs"})
            j["features"][field] = nullptr;
    }
    if (r.power_fraction.empty()) {
        for (const auto& field : {"spectral_flatness", "strongest_bin_fraction", "occupied_low_offset_hz",
                                  "occupied_high_offset_hz", "occupied_bandwidth_99pct_hz"})
            j["features"][field] = nullptr;
    }
    if (format == IqFormat::ci16_le && r.samples_used) {
        std::size_t at_limit = 0;
        const auto source_count = select_band ? input.samples.size() : r.samples_used;
        for (std::size_t i = 0; i < source_count; ++i) {
            at_limit += std::abs(input.samples[i].real()) >= 32767.0f / 32768;
            at_limit += std::abs(input.samples[i].imag()) >= 32767.0f / 32768;
        }
        const double fraction = double(at_limit) / (2 * source_count);
        j["features"]["int16_full_scale_component_fraction"] = fraction;
        j["features"]["int16_full_scale_scope"] = select_band ? "source_window_read" : "analyzed_source_samples";
        if (fraction > 0.01) j["warnings"].push_back("frequent_int16_full_scale_components");
    }
    for (const auto& p : r.peaks) {
        json peak = {{"alpha_hz", p.alpha_hz}, {"frequency_offset_hz", p.frequency_offset_hz},
                     {"coherence_squared", p.coherence_squared},
                     {"normalized_cross_magnitude", p.normalized_cross_magnitude}};
        if (!preprocessing["analysis_center_hz"].is_null())
            peak["rf_frequency_hz"] = preprocessing["analysis_center_hz"].get<double>() + p.frequency_offset_hz;
        j["features"]["cyclic_peaks"].push_back(peak);
    }
    if (o.count("--include-spectrum")) {
        j["spectrum"] = {{"first_bin_offset_hz", -analysis_rate / 2}, {"step_hz", r.bin_hz},
                          {"power_fraction", r.power_fraction}, {"kind", "ordinary_alpha_zero"}};
    }
    j["ofdm_structure"] = {{"method", "lag_fold_train_phase_holdout_cp_v1"},
                           {"scope", "selected_window_prefix_up_to_262144_samples"},
                           {"phase_fit", "first_half_only_one_symbol_separation_before_holdout"},
                           {"sample_mean", "shared_selected_window_mean"},
                           {"minimum_symbols_per_partition", 8},
                           {"multiple_hypotheses_corrected", false},
                           {"calibrated_detection_threshold", nullptr},
                           {"vendor_or_drone_identity", false}, {"candidates", json::array()}};
    for (const auto& m : ofdm) {
        json candidate = {{"timing_label", m.hypothesis.label}, {"status", m.status},
                          {"useful_samples", m.hypothesis.useful_samples},
                          {"prefix_samples", m.hypothesis.prefix_samples},
                          {"symbol_rate_hz", m.symbol_rate_hz}, {"samples_used", m.samples_used},
                          {"train_symbols", m.train_symbols}, {"holdout_symbols", m.holdout_symbols}};
        for (const auto& field : {"prefix_phase_samples", "train_prefix_coherence_squared",
                                  "holdout_prefix_coherence_squared", "holdout_outside_coherence_squared",
                                  "holdout_contrast", "holdout_symbol_cyclic_coherence_squared"})
            candidate[field] = nullptr;
        if (m.status == "measured") {
            candidate.update({{"prefix_phase_samples", m.prefix_phase_samples},
                              {"train_prefix_coherence_squared", m.train_prefix_coherence_squared},
                              {"holdout_prefix_coherence_squared", m.holdout_prefix_coherence_squared},
                              {"holdout_outside_coherence_squared", m.holdout_outside_coherence_squared},
                              {"holdout_contrast", m.holdout_contrast},
                              {"holdout_symbol_cyclic_coherence_squared", m.holdout_symbol_cyclic_coherence_squared}});
        }
        j["ofdm_structure"]["candidates"].push_back(candidate);
    }
    if (o.count("--assess-evidence")) j["dsp_evidence"] = evidence_json(assess_link_evidence(r, ofdm, chirps,
        {analysis_rate, evidence_width, source_rails(input.samples, format, 0,
            select_band ? input.samples.size() : r.samples_used)}));
    return j;
}

json inventory(const Options& o, IqFormat format, double rate) {
    const auto root = fs::canonical(required(o, "--root"));
    if (!fs::is_directory(root)) throw std::invalid_argument("inventory root must be a directory");
    const auto expected = integer(o, "--expected-samples", 0);
    const auto probe = integer(o, "--probe-samples", 4096);
    if (!probe || probe > 65536) throw std::invalid_argument("probe count must be in 1..65536");
    std::vector<fs::path> paths, empty_dirs, skipped;
    std::size_t visited = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (++visited > 20000) throw std::invalid_argument("inventory exceeds 20000 entries");
        if (entry.is_symlink()) { skipped.push_back(entry.path()); continue; }
        if (entry.is_directory() && fs::is_empty(entry.path())) empty_dirs.push_back(entry.path());
        if (!entry.is_regular_file()) continue;
        const auto suffix = entry.path().extension().string();
        if (suffix == ".dat" || suffix == ".bin" || suffix == ".cf32_le" || suffix == ".ci16_le")
            paths.push_back(entry.path());
        else skipped.push_back(entry.path());
    }
    if (paths.empty()) throw std::invalid_argument("no supported IQ filenames found");
    std::sort(paths.begin(), paths.end()); std::sort(empty_dirs.begin(), empty_dirs.end());
    json j = {{"schema", "rfmon.cyclo.inventory.v1"}, {"root", root.string()},
              {"input", metadata(o, format, rate)}, {"files", json::array()},
              {"empty_directories", json::array()}, {"skipped_entries", json::array()},
              {"provenance", {{"full_checksums", "not_computed"},
                               {"physical_units", "unknown"}, {"sessions", "unknown"},
                               {"labels", "directory_derived_unverified"}}}};
    std::uint64_t total_bytes = 0;
    std::size_t short_files = 0, long_files = 0, errors = 0;
    std::map<std::string, int> conditions;
    const std::regex folder_pattern("([A-Z0-9]+)_(ON|HO|FY)");
    for (const auto& path : paths) {
        const auto relative = path.lexically_relative(root);
        const auto bytes = fs::file_size(path);
        const auto stride = bytes_per_sample(format);
        json record = {{"relative_path", relative.generic_string()}, {"bytes", bytes},
                       {"split_group", relative.generic_string()},
                       {"sample_count", bytes / stride}, {"alignment_valid", bytes > 0 && bytes % stride == 0},
                       {"duration_seconds_if_declared_format", double(bytes / stride) / rate},
                       {"expected_sample_count", expected ? json(expected) : json(nullptr)}};
        total_bytes += bytes;
        record["nominal_length"] = "not_specified";
        if (expected) {
            const auto samples = bytes / stride;
            record["nominal_length"] = samples < expected ? "short" : samples > expected ? "longer" : "exact";
            short_files += samples < expected; long_files += samples > expected;
        }
        std::smatch match;
        const auto folder = path.parent_path().filename().string();
        if (std::regex_match(folder, match, folder_pattern)) {
            const auto condition = path.parent_path().parent_path().filename().string();
            record["directory_labels"] = {{"model_code", match[1].str()},
                                           {"state_code", match[2].str()}, {"condition", condition}};
            ++conditions[condition];
        }
        try {
            auto window = read_iq_window(path, format, 0, probe);
            record["prefix_probe"] = {{"samples_read", window.samples.size()}, {"finite", true},
                                       {"scope", "prefix_only_not_full_integrity"}};
        } catch (const std::exception& e) {
            ++errors;
            record["prefix_probe"] = {{"error", e.what()}};
        }
        j["files"].push_back(std::move(record));
    }
    for (const auto& p : empty_dirs) j["empty_directories"].push_back(p.lexically_relative(root).generic_string());
    std::sort(skipped.begin(), skipped.end());
    for (const auto& p : skipped) j["skipped_entries"].push_back(p.lexically_relative(root).generic_string());
    j["summary"] = {{"file_count", paths.size()}, {"bytes", total_bytes},
                      {"conditions", conditions}, {"short_files", short_files},
                      {"longer_files", long_files}, {"probe_errors", errors}};
    return j;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { std::cout << usage; return 0; }
        if (argc < 2) { std::cerr << usage; return 2; }
        const std::string command = argv[1];
        std::set<std::string> allowed = {"--format", "--sample-rate", "--center"};
        if (command == "analyze") {
            for (const auto& k : {"--input", "--offset", "--samples", "--fft", "--hop", "--frames", "--alpha-bins", "--include-spectrum",
                                  "--roi-offset", "--roi-bandwidth", "--source-bandwidth", "--decimate",
                                  "--ofdm-useful", "--ofdm-prefix", "--assess-evidence", "--remove-source-dc", "--usable-bandwidth", "--refine-chirps"})
                allowed.insert(k);
            allowed.insert("--measure-rois");
            allowed.insert("--measure-chirps");
            allowed.insert("--qualify-candidates");
            allowed.insert("--refine-clock"); allowed.insert("--analyze-bursts"); allowed.insert("--check-cyclic-background"); allowed.insert("--discover-sweeps"); allowed.insert("--review-waveforms"); allowed.insert("--discover-structure"); allowed.insert("--expand-waveforms"); allowed.insert("--prepare-candidates");
        } else if (command == "inventory") {
            for (const auto& k : {"--root", "--expected-samples", "--probe-samples"}) allowed.insert(k);
        } else if (command == "shadow-replay") {
            for (const auto& k : {"--input", "--offset", "--capture-samples", "--continuous-samples", "--burst-hints", "--include-spectrum", "--usable-bandwidth"}) allowed.insert(k);
        } else throw std::invalid_argument("command must be analyze, inventory or shadow-replay");
        Options options;
        for (int i = 2; i < argc; ++i) {
            const std::string name = argv[i];
            if (!allowed.count(name)) throw std::invalid_argument("unknown option: " + name);
            if (options.count(name)) throw std::invalid_argument("duplicate option: " + name);
            if (name == "--include-spectrum" || name == "--measure-rois" || name == "--measure-chirps" || name == "--assess-evidence" || name == "--remove-source-dc" || name == "--refine-chirps" || name=="--refine-clock" || name=="--analyze-bursts" || name=="--check-cyclic-background" || name=="--discover-sweeps" || name=="--review-waveforms" || name=="--discover-structure" || name=="--expand-waveforms" || name=="--prepare-candidates" || name=="--qualify-candidates") options[name] = "true";
            else {
                if (++i >= argc) throw std::invalid_argument("missing option value: " + name);
                options[name] = argv[i];
            }
        }
        const auto format = parse_format(required(options, "--format"));
        const auto rate = real_value(required(options, "--sample-rate"), "--sample-rate");
        if (rate <= 0 || rate > 1e9) throw std::invalid_argument("sample rate must be in (0, 1e9]");
        auto result = command == "analyze" ? analyze(options, format, rate) :
                      command == "shadow-replay" ? shadow_replay(options, format, rate) : inventory(options, format, rate);
        std::cout << result.dump(2) << '\n';
        if (!std::cout) throw std::runtime_error("JSON output failed");
        return command == "inventory" && result["summary"]["probe_errors"].get<int>() ? 3 : 0;
    } catch (const std::exception& e) {
        std::cerr << "wifi_cyclo_replay: " << e.what() << '\n';
        return 1;
    }
}
