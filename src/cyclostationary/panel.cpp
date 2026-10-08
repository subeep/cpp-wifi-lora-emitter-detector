#include "panel.hpp"
#include "imgui.h"
#include <chrono>
#include <algorithm>

namespace rfmon::cyclo {
namespace {
const char* review_status_label(const std::string& status) {
    if(status=="experimental_pattern" || status=="experimental_patterns")return "Experimental pattern";
    if(status=="cyclic_structure_only")return "Cyclic diagnostic only";
    if(status=="morphology_only")return "Shape diagnostic only";
    if(status=="diagnostic_patterns_only")return "Diagnostics; waveform unresolved";
    if(status=="ambiguous_patterns")return "Competing patterns";
    if(status=="background_rejected")return "Local background checks failed";
    if(status=="quality_rejected")return "Signal quality failed";
    if(status=="passband_rejected")return "Outside declared band";
    if(status=="passband_unverified")return "Band coverage unknown";
    if(status=="no_match" || status=="no_supported_pattern")return "No supported pattern";
    if(status=="not_requested")return "Measurement not requested";
    return "Insufficient support";
}
std::string candidate_status_label(const ReviewedPattern& p) {
    const std::string state=review_status_label(p.status);
    if(!p.pattern_consistent || p.status=="cyclic_structure_only" || p.status=="morphology_only")return state;
    if(p.kind=="ordinary_cyclic" || p.kind=="conjugate_cyclic")return "Cycle diagnostic; "+state;
    if(p.kind=="two_frequency" || p.kind=="two_level_envelope" || p.kind=="linear_sweep")return "Shape diagnostic; "+state;
    return state;
}
std::string check_label(std::string name) {
    if(name=="competing_cp_and_barker_patterns")return "CP and Barker patterns coexist; source/type unresolved";
    if(name=="unknown")return "Receiver band coverage is unknown";
    if(name=="outside_declared_band")return "Measured power extends outside the declared band";
    if(name=="source_int16_full_scale_component_fraction")return "Source ADC rail fraction";
    std::replace(name.begin(),name.end(),'_',' ');return name;
}
void draw_review(const WaveformReview& review,const WaveformReview& sweeps) {
    ImGui::Text("Pattern review: %s",review_status_label(review.status));
    if(review.cp_code_ambiguous)ImGui::TextWrapped("Competing CP/Barker patterns: both checks match. A unique waveform or emitter is unresolved.");
    if(ImGui::BeginTable("cyclo_review_summary",3,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Measured structure",ImGuiTableColumnFlags_WidthFixed,175);
        ImGui::TableSetupColumn("Review");ImGui::TableSetupColumn("Matches",ImGuiTableColumnFlags_WidthFixed,65);ImGui::TableHeadersRow();
        for(const auto kind:{"cp_timing","barker_11","two_frequency","two_level_envelope","ordinary_cyclic","conjugate_cyclic","linear_sweep"}) {
            const ReviewedPattern* selected=nullptr;std::size_t matched=0;
            for(const auto& p:(std::string(kind)=="linear_sweep"?sweeps:review).candidates)if(p.kind==kind){if(!selected || p.pattern_consistent)selected=&p;matched+=p.pattern_consistent;}
            const char* label=std::string(kind)=="cp_timing"?"General CP timing":std::string(kind)=="barker_11"?"Barker-11 code":
                std::string(kind)=="two_frequency"?"Two-frequency shape":std::string(kind)=="two_level_envelope"?"Two-level envelope":
                std::string(kind)=="ordinary_cyclic"?"Ordinary cycles":std::string(kind)=="conjugate_cyclic"?"Conjugate cycles":"Linear sweep shape";
            ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(label);ImGui::TableNextColumn();
            ImGui::TextWrapped("%s",selected?candidate_status_label(*selected).c_str():std::string(kind)=="linear_sweep"?review_status_label(sweeps.status):"Insufficient support");
            ImGui::TableNextColumn();ImGui::Text("%zu",matched);
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Matches are uncalibrated measurements, not votes. Cycles/shapes alone are diagnostics; drone and link identity remain unknown.");
    if(ImGui::TreeNode("Pattern review checks")) {
        ImGui::Text("Observed signal quality: %s",review.observed_quality_passed?"checks passed":"checks failed");
        for(const auto& c:review.quality_checks) {
            if(!c.known)ImGui::TextDisabled("%s: unknown",check_label(c.name).c_str());
            else if(!c.passed)ImGui::TextWrapped("%s: %.4g %s %.4g failed",check_label(c.name).c_str(),c.value,c.comparison.c_str(),c.threshold);
        }
        ImGui::TextWrapped("Band coverage: %s. Receiver calibration remains unverified.",review.passband_status=="unknown"?"unknown":
            review.passband_status=="outside_declared_band"?"outside declared geometry":"fits caller-declared geometry");
        static bool show_rejected=false;ImGui::Checkbox("Show candidates that failed pattern checks",&show_rejected);
        bool shown=false;
        for(const auto& p:review.candidates)if(p.pattern_consistent || show_rejected) {
            shown=true;ImGui::Separator();ImGui::TextWrapped("%s: %s",p.label.c_str(),candidate_status_label(p).c_str());
            for(const auto& reason:p.reasons)ImGui::TextWrapped("Reason: %s",check_label(reason).c_str());
            for(const auto& c:p.checks)ImGui::TextDisabled("%s: %.4g %s %.4g (%s)",check_label(c.name).c_str(),c.value,
                c.comparison.c_str(),c.threshold,c.passed?"pass":"fail");
        }
        if(!shown)ImGui::TextDisabled("No consistent pattern in this window. Enable failed candidates to inspect the measurements.");
        ImGui::TreePop();
    }
}
} // namespace

void draw_shadow_panel(const std::shared_ptr<const ShadowResult>& result,
                       const ShadowStats& stats, int active_band_ghz) {
    ImGui::TextUnformatted("Drone: unknown | Link family: unknown | Waveform measurements");
    if(stats.failures || stats.worker_timeouts || stats.protocol_errors)
        ImGui::TextWrapped("Worker faults %llu | Timeouts %llu | Invalid replies %llu. See diagnostics when a current measurement is available.",
            (unsigned long long)stats.failures,(unsigned long long)stats.worker_timeouts,(unsigned long long)stats.protocol_errors);
    if (!stats.enabled) { ImGui::TextDisabled("Analysis is disabled. Enable it to sample future Wi-Fi captures."); return; }
    if (!result || result->epoch != stats.epoch || result->capture.band_ghz != active_band_ghz) {
        ImGui::TextDisabled("Waiting for a measurement from the current band."); return;
    }
    const auto& r = *result;
    const auto& c = r.capture;
    if (r.tiles.empty()) { ImGui::TextDisabled("No valid analysis windows."); return; }
    static int selected_tile = 0;
    if (selected_tile < 0 || std::size_t(selected_tile) >= r.tiles.size()) selected_tile = 0;
    if (ImGui::BeginCombo("Capture window", std::to_string(selected_tile + 1).c_str())) {
        for (std::size_t i = 0; i < r.tiles.size(); ++i)
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), std::size_t(selected_tile) == i)) selected_tile = int(i);
        ImGui::EndCombo();
    }
    const auto& tile = r.tiles[std::size_t(selected_tile)];
    ImGui::Text("Capture %llu | Window %d/%zu | %.3f MHz | Age %.1f s",
        (unsigned long long)c.capture_sequence,selected_tile+1,r.tiles.size(),c.capture_center_hz/1e6,
        std::chrono::duration<double>(std::chrono::steady_clock::now()-r.completed_at).count());
    ImGui::TextDisabled("Latest sampled window only; mixtures and scan gaps remain unresolved.");
    if(c.capture_overflow || c.timed_out)ImGui::TextWrapped("Capture had a gap/timeout; only the eligible continuous prefix was analysed.");
    draw_review(tile.review,tile.sweep_review);
    std::size_t raw_ordinary=0,raw_conjugate=0,checked_ordinary=0,checked_conjugate=0;
    for(std::size_t i=0;i<tile.waveform.peaks.size();++i) {
        const auto& peak=tile.waveform.peaks[i];const bool raw=peak.discovery_coherence_squared>=.05 && peak.holdout_coherence_squared>=.05;
        if(peak.conjugate)raw_conjugate+=raw;else raw_ordinary+=raw;
        if(i<tile.background_review.candidates.size() && tile.background_review.candidates[i].pattern_consistent) {
            if(peak.conjugate)++checked_conjugate;else ++checked_ordinary;
        }
    }
    ImGui::TextWrapped("Cyclic background: ordinary %zu/%zu | conjugate %zu/%zu raw matches supported. Heuristic diagnostics only.",
        checked_ordinary,raw_ordinary,checked_conjugate,raw_conjugate);
    if(raw_ordinary+raw_conjugate>checked_ordinary+checked_conjugate)
        ImGui::TextWrapped("Some raw cyclic matches lack local background support. Inspect Cyclic background checks for reasons.");
    const auto& burst=tile.burst;const auto& bs=burst.selection;
    if(bs.samples)ImGui::Text("Burst analysis: %zu samples | eligible %zu | skipped %zu | short %zu%s",bs.samples,
        bs.eligible_regions,bs.budget_skipped_regions,bs.short_regions,bs.cropped?" | cropped":"");
    else ImGui::TextDisabled("Burst analysis: %s | short regions %zu",bs.status=="insufficient_burst_support"?
        "insufficient contiguous support":"no contrast region",bs.short_regions);
    if(ImGui::TreeNode("Burst-local waveform analysis")) {
        if(bs.samples) {
            ImGui::Text("Source samples %zu..%zu | %.3f ms | local PSD %s (%zu frames)",
                tile.source.source_offset+bs.offset,tile.source.source_offset+bs.offset+bs.samples,
                1e3*bs.samples/c.sample_rate_hz,burst.spectral.quality.c_str(),burst.spectral.frames);
            ImGui::Text("ROI %zu | central crop %s | 99%% local power %.3f..%.3f MHz",bs.region_index+1,
                bs.cropped?"applied":"not needed",burst.spectral.occupied_low_hz/1e6,burst.spectral.occupied_high_hz/1e6);
            ImGui::PushID("burst_local");draw_review(burst.review,burst.sweep_review);
            std::size_t supported=0;for(const auto& p:burst.background.peaks)supported+=p.background_supported;
            ImGui::Text("Burst cyclic background: %zu/%zu selected lines supported",supported,burst.background.peaks.size());
            for(std::size_t i=0;i<burst.background_review.candidates.size();++i) {
                const auto& p=burst.background_review.candidates[i];
                if(p.pattern_consistent || (i<burst.waveform.peaks.size() && burst.waveform.peaks[i].persistent_pattern)) {
                    ImGui::TextWrapped("%s: %s",p.label.c_str(),candidate_status_label(p).c_str());
                    for(const auto& reason:p.reasons)ImGui::TextWrapped("Reason: %s",check_label(reason).c_str());
                }
            }
            ImGui::PopID();
        }
        ImGui::TextWrapped("Strongest eligible contrast region only; at most 16,384 contiguous samples per tile. "
            "Short regions are skipped. Separate bursts and capture gaps are never joined. "
            "Energy selection is biased and uncalibrated; local support does not validate a drone or link. "
            "Other regions and weak activity can be missed. Whole-tile measurements remain separate.");
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Timing drift refinement")) {
        const auto& clock=tile.clock;
        ImGui::Text("Timing refinement: %s | scope %s | grids %zu | trials CP %zu / code %zu",clock.status.c_str(),
            tile.burst.selection.samples?"selected burst":"whole tile",clock.grids_tested,clock.cp_trials,clock.code_trials);
        ImGui::Text("Partition budget %zu samples | canonical held origin %zu | interpolated %zu samples",
            clock.partition_samples,clock.holdout_offset,clock.interpolated_samples);
        for(const auto& p:clock.cp) {
            const auto& m=p.measurement;const auto factor=timing_grid_scale(p.grid_index);
            ImGui::Text("CP timing grid %+d ppm | source useful %.3f + prefix %.3f samples",
                timing_grid_ppm[p.grid_index],factor*m.useful_samples,factor*m.prefix_samples);
            ImGui::TextDisabled("Discovery CP %.4f / outside %.4f | held CP %.4f / outside %.4f | held cycle %.4f",
                m.train_prefix,m.train_outside,m.holdout_prefix,m.holdout_outside,m.holdout_cyclic);
        }
        for(const auto& p:clock.code) {
            const auto& m=p.measurement;
            ImGui::Text("Barker-11 timing grid %+d ppm | source chip %.4f samples | held words %zu",
                timing_grid_ppm[p.grid_index],timing_grid_scale(p.grid_index)*m.chip_samples,m.holdout_words);
            ImGui::TextDisabled("Discovery code %.4f / competitor %.4f | held code %.4f / competitor %.4f",
                m.train_code_coherence_squared,m.train_other_phase,m.holdout_code_coherence_squared,m.holdout_other_phase);
        }
        ImGui::Text("Timing review: %s",review_status_label(clock.review.status));
        for(const auto& p:clock.review.candidates)if(p.kind=="cp_timing" || p.kind=="barker_11") {
            ImGui::TextWrapped("%s: %s",p.label.c_str(),candidate_status_label(p).c_str());
            for(const auto& reason:p.reasons)ImGui::TextWrapped("Reason: %s",check_label(reason).c_str());
        }
        ImGui::TextWrapped("Nine fixed resampling grids within +/-3,000 ppm; two retained integer seeds per kind. "
            "Grid, phase and geometry are selected on discovery only, then frozen for held checks. "
            "The selected grid is a timing compatibility hypothesis, not a calibrated receiver-clock estimate. "
            "Linear interpolation can smooth or miss signals; original measurements and unknown identity remain separate.");
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Capture and worker diagnostics")) {
        ImGui::Text("Accepted %llu | Measured %llu | Busy drops %llu | Invalid %llu | Discarded %llu | Failures %llu",
                    (unsigned long long)stats.submitted, (unsigned long long)stats.measured,
                    (unsigned long long)stats.dropped_busy, (unsigned long long)stats.dropped_invalid,
                    (unsigned long long)stats.discarded, (unsigned long long)stats.failures);
        ImGui::Text("Pool high water %zu/8 | Max accepted copy submission %.3f ms", stats.high_water, stats.max_submit_ms);
        ImGui::Text("Worker PID %d | Launches %llu | Fault restarts %llu | Timeouts %llu | Bad replies %llu",
                    stats.worker_pid, (unsigned long long)stats.worker_launches, (unsigned long long)stats.worker_restarts,
                    (unsigned long long)stats.worker_timeouts, (unsigned long long)stats.protocol_errors);
        ImGui::Text("Measurement age %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - r.completed_at).count());
        ImGui::Text("Latest capture %llu | Radio session %llu | %.3f MHz | %.3f Msps",
                    (unsigned long long)c.capture_sequence, (unsigned long long)c.radio_session,
                    c.capture_center_hz / 1e6, c.sample_rate_hz / 1e6);
        ImGui::Text("Copied %zu/%zu samples across %zu windows (%.3f ms total) | Worker %.2f ms",
                    r.copied_samples, c.source_samples, r.tiles.size(), r.copied_samples / c.sample_rate_hz * 1e3,
                    r.processing_ms);
        ImGui::Text("Burst-guided windows %zu | Raw hints examined %zu/%zu | Eligible %zu | Rejected %zu",
                    r.selection.burst_windows, r.selection.hints_examined, r.selection.hints_reported,
                    r.selection.hints_eligible, r.selection.hints_rejected);
        if (r.selection.detector_capped || r.selection.hints_examined < r.selection.hints_reported)
            ImGui::TextWrapped("Burst context is sampled or detector-capped; this is not complete activity coverage.");
        ImGui::Text("Window %d: source samples %zu..%zu | %.3f ms | %s", selected_tile + 1,
                    tile.source.source_offset, tile.source.source_offset + tile.source.samples,
                    tile.source.samples / c.sample_rate_hz * 1e3, tile.spectral.quality.c_str());
        ImGui::TextDisabled("Selection: %s", r.selection.burst_guided[std::size_t(selected_tile)]
                            ? "raw burst hint (not a waveform verdict)" : "capture context");
        ImGui::TextDisabled("Latest capture only; partial channel coverage. Whole-band mixtures are unresolved.");
        if (c.capture_overflow || c.timed_out)
            ImGui::TextWrapped("Capture reports overflow/timeout; analysis uses only the prefix before the first known gap.");
        for (const auto& warning : tile.spectral.warnings) ImGui::TextDisabled("%s", warning.c_str());
        ImGui::Text("DSP evidence policy %s | Observed quality checks %s", evidence_policy_version,
                    tile.evidence.observed_quality_passed ? "passed" : "failed");
        for (const auto& check : tile.evidence.quality_checks) {
            if (!check.known) ImGui::TextDisabled("%s: unknown", check.name.c_str());
            else if (!check.passed) ImGui::TextDisabled("%s: %.4g %s %.4g failed", check.name.c_str(),
                                                      check.value, check.comparison.c_str(), check.threshold);
        }
        for (const auto& evidence : tile.evidence.candidates) {
            if (evidence.pattern_consistent) ImGui::Text("%s: %s | %s", evidence.hypothesis.c_str(),
                                                        evidence.status.c_str(), evidence.passband_status.c_str());
        }
        ImGui::TextDisabled("Preview thresholds are uncalibrated. Receiver passband and float ADC rails are unknown.");
        if (ImGui::TreeNode("Waveform evidence checks")) {
            for (const auto& evidence : tile.evidence.candidates) {
                ImGui::Text("%s: %s", evidence.hypothesis.c_str(), evidence.status.c_str());
                for (const auto& check : evidence.checks) {
                    if (check.known) ImGui::TextDisabled("%s: %.4g %s %.4g (%s)", check.name.c_str(),
                        check.value, check.comparison.c_str(), check.threshold, check.passed ? "pass" : "fail");
                    else ImGui::TextDisabled("%s: unknown", check.name.c_str());
                }
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Reference checks and observed regions")) {
        ImGui::Text("Window energy: %s | Contrast intervals %zu | Shown %zu | Removed DC %.1f%%",
                    tile.roi.status.c_str(), tile.roi.regions_seen, tile.roi.regions.size(), tile.roi.dc_fraction * 100);
        if (ImGui::BeginTable("cyclo_roi_measurements", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (const auto* label : {"Window samples", "Energy share", "PSD frames", "Spectral interval offsets"})
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for (const auto& region : tile.roi.regions) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%zu..%zu (%s)%s", region.offset,
                    region.offset + region.samples, region.contrast_selected ? "energy" : "context",
                    region.touches_window_edge ? " (edge)" : "");
                ImGui::TableNextColumn(); ImGui::Text("%.1f%%", region.energy_fraction * 100);
                ImGui::TableNextColumn(); ImGui::Text("%zu / %s", region.spectral_frames, region.spectral_status.c_str());
                ImGui::TableNextColumn();
                if (region.spectral_status == "measured" || region.spectral_status == "few_frames")
                    ImGui::Text("99%% power: %.3f..%.3f MHz", region.occupied_low_hz / 1e6, region.occupied_high_hz / 1e6);
                if (region.bands.empty()) ImGui::TextDisabled("No contrasted frequency interval");
                for (const auto& b : region.bands) ImGui::Text("%.3f..%.3f MHz%s%s", b.low_hz / 1e6, b.high_hz / 1e6,
                    b.edge_bin ? " (edge)" : "", b.contains_dc ? " (DC)" : "");
                if (region.bands_seen > region.bands.size()) ImGui::TextDisabled("%zu more omitted", region.bands_seen - region.bands.size());
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("128-sample energy bounds and spectral intervals are selection hints; mixtures remain unresolved.");
        if (ImGui::BeginTable("cyclo_cp_measurements", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (const auto* label : {"OFDM timing hypothesis", "Holdout CP", "Outside CP", "Contrast", "Symbol alpha"})
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for (const auto& m : tile.ofdm) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(m.hypothesis.label.c_str());
                if (m.status != "measured") {
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(m.status.c_str()); continue;
                }
                ImGui::TableNextColumn(); ImGui::Text("%.4f", m.holdout_prefix_coherence_squared);
                ImGui::TableNextColumn(); ImGui::Text("%.4f", m.holdout_outside_coherence_squared);
                ImGui::TableNextColumn(); ImGui::Text("%.4f", m.holdout_contrast);
                ImGui::TableNextColumn(); ImGui::Text("%.2f kHz / %.4f", m.symbol_rate_hz / 1e3,
                                                    m.holdout_symbol_cyclic_coherence_squared);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Squared coherences are measurements, not probabilities. No calibrated acceptance threshold.");
        if (ImGui::BeginTable("cyclo_chirp_measurements", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (const auto* label : {"Chirp hypothesis", "Maximum coherence", "Source sample span"}) ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for (const auto& m : tile.chirps) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(m.label.c_str());
                ImGui::TableNextColumn();
                if (m.status != "measured") { ImGui::TextUnformatted(m.status.c_str()); continue; }
                ImGui::Text("%.4f (parts %.4f / %.4f)", m.peak_coherence_squared,
                            m.first_half_coherence_squared, m.second_half_coherence_squared);
                if (m.frequency_status == "measured") ImGui::Text("Slope %.2f kHz/us | residual %.1f kHz | unwraps %zu",
                    m.frequency_slope_hz_per_second / 1e9, m.frequency_rmse_hz / 1e3, m.phase_unwraps);
                ImGui::TableNextColumn(); ImGui::Text("%zu..%zu", tile.source.source_offset + m.peak_offset,
                    tile.source.source_offset + m.peak_offset + m.lag_samples + m.gate_samples);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Chirp maxima are experimental; passband, false alarms and link identity are unvalidated.");

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("General timing and short-code structure")) {
        const auto& s=tile.structure;
        ImGui::Text("Structure: %s | partition %zu samples | timing trials %zu | code trials %zu",
            s.status.c_str(),s.partition_samples,s.timing_hypotheses,s.spread_hypotheses);
        for(const auto& p:s.ofdm) {ImGui::Text("CP timing %zu + %zu samples | train %.4f / held %.4f%s",
            p.useful_samples,p.prefix_samples,p.train_prefix,p.holdout_prefix,p.pattern_consistent?" (pattern consistent)":"");
            ImGui::TextDisabled("Held outside %.4f | contrast %.4f | cycle %.4f | support %zu periods",
                p.holdout_outside,p.holdout_prefix-p.holdout_outside,p.holdout_cyclic,p.symbols_per_partition);
            ImGui::TextDisabled("Useful %.3f us | prefix %.3f us | symbol %.2f kHz",
                1e6*p.useful_samples/c.sample_rate_hz,1e6*p.prefix_samples/c.sample_rate_hz,
                c.sample_rate_hz/(p.useful_samples+p.prefix_samples)/1e3);
        }
        for(const auto& p:s.spread) {ImGui::Text("Barker-11: chip %zu samples | train %.4f / held %.4f | words %zu / %zu%s",
            p.chip_samples,p.train_code_coherence_squared,p.holdout_code_coherence_squared,p.train_words,p.holdout_words,
            p.pattern_consistent?" (code compatible)":"");
            ImGui::TextDisabled("Chip %.3f MHz | carrier %.3f MHz | held competing phase %.4f | margin %.4f",
                c.sample_rate_hz/p.chip_samples/1e6,p.carrier_hz/1e6,p.holdout_other_phase,
                p.holdout_code_coherence_squared-p.holdout_other_phase);
        }
        ImGui::TextWrapped("Timing and carrier are fitted on discovery samples only. Checks are uncalibrated. "
            "Barker-11 compatibility also occurs in ordinary traffic; other spreading codes are untested. "
            "Drone, link family and exact modulation remain unknown.");
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Cyclic background checks")) {
        const auto& background=tile.cyclic_background;
        ImGui::Text("Background: %s | extra FFTs %zu | separate partitions %zu samples",background.status.c_str(),background.fft_calls,background.partition_samples);
        for(std::size_t i=0;i<background.peaks.size() && i<tile.waveform.peaks.size();++i) {
            const auto& raw=tile.waveform.peaks[i];const auto& p=background.peaks[i];
            const bool persistent=raw.discovery_coherence_squared>=.05 && raw.holdout_coherence_squared>=.05;
            if(!persistent && !p.background_supported)continue;
            ImGui::Separator();ImGui::Text("%s %.2f kHz | lag %zu | raw %s | background %s",
                raw.conjugate?"Conjugate":"Ordinary",raw.alpha_hz/1e3,raw.lag_samples,persistent?"matched":"weak",
                p.background_supported?"supported":"failed checks");
            ImGui::Text("Line / upper background %.2f / %.2f | line / median %.2f / %.2f",
                p.discovery.line_to_upper,p.holdout.line_to_upper,p.discovery.line_to_median,p.holdout.line_to_median);
            ImGui::TextDisabled("Block phase coherence %.4f / %.4f | minimum block energy %.4f / %.4f | reference bins %zu / %zu",
                p.discovery.block_phase_coherence_squared,p.holdout.block_phase_coherence_squared,
                p.discovery.min_block_energy_fraction,p.holdout.min_block_energy_fraction,p.discovery.reference_bins,p.holdout.reference_bins);
            if(i<tile.background_review.candidates.size()) {
                const auto& review=tile.background_review.candidates[i];ImGui::TextWrapped("Review: %s",candidate_status_label(review).c_str());
                for(const auto& reason:review.reasons)ImGui::TextWrapped("Reason: %s",check_label(reason).c_str());
            }
        }
        ImGui::TextWrapped("Fixed discovery-selected rate and lag; local reference bins and four phase/energy portions in each partition. "
            "Local ratios and phase checks are uncalibrated. Nearby cyclic lines, bursts or clock drift can fail these checks; tones and mixtures can pass. "
            "This does not measure drone probability or identify a drone/link. Raw measurements and earlier reviews remain unchanged.");
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("General linear sweep measurements")) {
        const auto& s=tile.sweeps;
        ImGui::Text("Sweep search: %s | partition %zu samples | discovery windows %zu",s.status.c_str(),s.partition_samples,s.discovery_trials);
        for(std::size_t i=0;i<s.candidates.size();++i) {
            const auto& p=s.candidates[i];ImGui::Separator();
            ImGui::Text("%s sweep | slope %.3g Hz/s | observed sweep %.3f MHz | span %.3f us",
                p.slope_hz_per_second>0?"Upward":"Downward",p.slope_hz_per_second,p.sweep_hz/1e6,1e6*p.span_samples/c.sample_rate_hz);
            ImGui::Text("Discovery coherence %.4f | held coherence %.4f | held windows %zu%s",
                p.discovery_coherence_squared,p.holdout_coherence_squared,p.holdout_trials,p.pattern_consistent?" (shape consistent)":"");
            ImGui::TextDisabled("Frequency RMS (8 increments) / sample rate %.5f / %.5f | held support %s",
                p.discovery_rmse_fraction,p.holdout_rmse_fraction,p.holdout_supported?"available":"unavailable");
            if(i<tile.sweep_review.candidates.size()) {
                const auto& review=tile.sweep_review.candidates[i];ImGui::TextWrapped("Review: %s",candidate_status_label(review).c_str());
                for(const auto& reason:review.reasons)ImGui::TextWrapped("Reason: %s",check_label(reason).c_str());
            }
        }
        if(s.candidates.empty())ImGui::TextDisabled("No supported linear sweep proposal in discovery samples.");
        ImGui::TextWrapped("Slope and observed span are selected on discovery samples and frozen for the separate held partition. "
            "Held positions are searched; carrier centre is fitted separately in each window. Search thresholds are uncalibrated. "
            "Spans are partial measurement windows, not complete chirp durations. Aliased, nonlinear and low-power sweeps may be missed. "
            "A linear sweep alone does not identify a drone, vendor or link.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Expanded cyclic and waveform measurements")) {
        const auto& w=tile.waveform;
        ImGui::Text("Cyclic rates: %s | partitions %zu samples | bin %.2f kHz",w.cyclic_status.c_str(),w.partition_samples,w.alpha_bin_hz/1e3);
        ImGui::TextDisabled("Separate discovery/holdout means; frequency and lag selected on discovery only.");
        for(const auto& p:w.peaks) ImGui::Text("%s: alpha %.2f kHz | lag %zu | discovery %.4f / holdout %.4f%s",
            p.conjugate?"Conjugate":"Ordinary",p.alpha_hz/1e3,p.lag_samples,p.discovery_coherence_squared,
            p.holdout_coherence_squared,p.persistent_pattern?" (persistent pattern)":"");
        ImGui::Text("Envelope/frequency shape: %s",w.morphology_status.c_str());
        if(w.morphology_status=="measured") {
            ImGui::Text("Amplitude CV %.3f | envelope contrast %.3f | fit residual %.3f",w.amplitude_cv,w.envelope_contrast,w.envelope_fit_residual);
            if(w.phase_pairs && w.frequency_second_fraction>0) ImGui::Text("Frequency-state concentration %.3f | %.2f..%.2f kHz | transitions %zu",
                w.frequency_concentration,w.frequency_low_hz/1e3,w.frequency_high_hz/1e3,w.frequency_transitions);
            else ImGui::TextDisabled("Insufficient support for two frequency-state centres");
            ImGui::Text("Two-frequency pattern: %s | two-level envelope pattern: %s",
                w.two_frequency_pattern?"experimental match":"no match",w.two_level_envelope_pattern?"experimental match":"no match");
        }
        ImGui::TextWrapped("Raw shape measurements: ordinary traffic, tones and mixtures can produce these patterns. "
            "Thresholds are uncalibrated; receiver quality and passband need qualification. Exact modulation and drone/link identity remain unknown.");
        ImGui::TreePop();
    }
    if (!tile.spectral.peaks.empty()) {
        const auto& p = tile.spectral.peaks.front();
        ImGui::Text("Largest grid peak: alpha %.2f kHz | RF midpoint %.3f MHz | coherence %.4f",
                    p.alpha_hz / 1e3, (c.capture_center_hz + p.frequency_offset_hz) / 1e6, p.coherence_squared);
    }
}
}
