#include "cyclostationary/iq_file.hpp"
#include "cyclostationary/analysis_samples.hpp"
#include "cyclostationary/spectral_correlation.hpp"
#include "cyclostationary/ofdm_structure.hpp"
#include "cyclostationary/process_client.hpp"
#include "cyclostationary/burst_selection.hpp"

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
    "  [--assess-evidence] [--usable-bandwidth HZ] (declared profile, not hardware verification)\n"
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
        item["dsp_evidence"] = evidence_json(assess_link_evidence(r, tile.ofdm, tile.chirps,
            {rate, width, source_rails(packed, format, packed_first, tile.source.samples)}));
        packed_first += tile.source.samples;
        j["tiles"].push_back(item);
    }
    return j;
}

json analyze(const Options& o, IqFormat format, double rate) {
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
    if (o.count("--assess-evidence") && analysis_iq.size() > 65536)
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
    if (o.count("--measure-chirps") || o.count("--assess-evidence")) chirps = measure_chirp_structure(analysis_iq, analysis_rate);
    RoiMeasurements roi;
    if (o.count("--measure-rois")) roi = measure_rois(analysis_iq, analysis_rate);
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
    if (o.count("--measure-rois")) j["roi_measurements"] = roi_json(roi, analysis_rate,
        input.offset_samples + (select_band ? prepared.first_input_center : 0),
        select_band ? prepared.input_step : 1, preprocessing["analysis_center_hz"]);
    if (o.count("--measure-chirps") || o.count("--assess-evidence")) j["chirp_structure"] = chirp_json(chirps, analysis_rate,
        input.offset_samples + (select_band ? prepared.first_input_center : 0), select_band ? prepared.input_step : 1);
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
                                  "--ofdm-useful", "--ofdm-prefix", "--assess-evidence", "--remove-source-dc", "--usable-bandwidth"})
                allowed.insert(k);
            allowed.insert("--measure-rois");
            allowed.insert("--measure-chirps");
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
            if (name == "--include-spectrum" || name == "--measure-rois" || name == "--measure-chirps" || name == "--assess-evidence" || name == "--remove-source-dc") options[name] = "true";
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
