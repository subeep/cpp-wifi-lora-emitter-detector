#include "panel.hpp"
#include "imgui.h"
#include <chrono>

namespace rfmon::cyclo {
void draw_shadow_panel(const std::shared_ptr<const ShadowResult>& result,
                       const ShadowStats& stats, int active_band_ghz) {
    ImGui::TextWrapped("Experimental DSP measurements. Drone activity and link family remain unknown. "
                       "OFDM and cyclic structure also occur in ordinary Wi-Fi. ML and Remote ID are deferred.");
    ImGui::Text("Accepted %llu | Measured %llu | Busy drops %llu | Invalid %llu | Discarded %llu | Failures %llu",
                (unsigned long long)stats.submitted, (unsigned long long)stats.measured,
                (unsigned long long)stats.dropped_busy, (unsigned long long)stats.dropped_invalid,
                (unsigned long long)stats.discarded, (unsigned long long)stats.failures);
    ImGui::Text("Pool high water %zu/8 | Max accepted copy submission %.3f ms", stats.high_water, stats.max_submit_ms);
    ImGui::Text("Worker PID %d | Launches %llu | Fault restarts %llu | Timeouts %llu | Bad replies %llu",
                stats.worker_pid, (unsigned long long)stats.worker_launches, (unsigned long long)stats.worker_restarts,
                (unsigned long long)stats.worker_timeouts, (unsigned long long)stats.protocol_errors);
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
    if (!tile.spectral.peaks.empty()) {
        const auto& p = tile.spectral.peaks.front();
        ImGui::Text("Largest grid peak: alpha %.2f kHz | RF midpoint %.3f MHz | coherence %.4f",
                    p.alpha_hz / 1e3, (c.capture_center_hz + p.frequency_offset_hz) / 1e6, p.coherence_squared);
    }
}
}
