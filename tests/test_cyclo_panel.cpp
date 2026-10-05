#include "cyclostationary/panel.hpp"
#include "imgui.h"
#include <iostream>

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000); io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto draw = [&](const std::shared_ptr<const rfmon::cyclo::ShadowResult>& r, rfmon::cyclo::ShadowStats stats, int band) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Cyclostationary panel verification");
        rfmon::cyclo::draw_shadow_panel(r, stats, band);
        ImGui::End(); ImGui::Render();
        return ImGui::GetDrawData()->TotalVtxCount;
    };
    rfmon::cyclo::ShadowStats stats; stats.epoch = 3;
    const auto disabled = draw({}, stats, 2);
    stats.enabled = true;
    const auto waiting = draw({}, stats, 2);
    auto result = std::make_shared<rfmon::cyclo::ShadowResult>(); result->epoch = 3;
    result->capture.band_ghz = 2; result->capture.sample_rate_hz = 20e6; result->capture.capture_center_hz = 2438.5e6;
    result->capture.source_samples = 20000000; result->copied_samples = 262144;
    result->completed_at = std::chrono::steady_clock::now();
    result->tiles.resize(1); result->tiles[0].source = {0, 65536};
    result->tiles[0].spectral.quality = "measured";
    result->tiles[0].chirps = rfmon::cyclo::measure_chirp_structure(std::vector<std::complex<float>>(65536, {1, 0}), 20e6);
    result->tiles[0].chirps[0].status = "measured"; result->tiles[0].chirps[0].peak_coherence_squared = 0.7;
    result->tiles[0].roi.status = "contrast_regions"; result->tiles[0].roi.regions_seen = 9;
    rfmon::cyclo::EnergyRegion region; region.offset = 128; region.samples = 8192;
    region.spectral_frames = 16; region.spectral_status = "measured"; region.energy_fraction = 0.8;
    region.bands.push_back({-1e6, 1e6, 0.9, false, true}); result->tiles[0].roi.regions.push_back(region);
    for (const auto& h : rfmon::cyclo::wlan_ofdm_hypotheses(20e6)) {
        rfmon::cyclo::OfdmMeasurement m; m.hypothesis = h; m.status = "measured";
        m.holdout_contrast = 0.2; result->tiles[0].ofdm.push_back(m);
    }
    const auto measured = draw(result, stats, 2);
    result->tiles[0].evidence = rfmon::cyclo::assess_link_evidence(result->tiles[0].spectral,
        result->tiles[0].ofdm, result->tiles[0].chirps, {20e6, 0, -1});
    const auto qualified = draw(result, stats, 2);
    const auto other_band = draw(result, stats, 5);
    ++stats.epoch; const auto stale = draw(result, stats, 2);
    result->tiles[0].ofdm[0].status = "insufficient_symbols"; result->capture.capture_overflow = true;
    result->selection.burst_guided[0] = true; result->selection.burst_windows = 1;
    result->selection.detector_capped = true; result->selection.hints_examined = 512;
    result->selection.hints_reported = 4000;
    stats.epoch = 3; const auto warning = draw(result, stats, 2);
    ImGui::DestroyContext();
    if (disabled <= 0 || waiting <= 0 || measured <= waiting || qualified <= waiting || other_band != waiting || stale != waiting || warning <= waiting) {
        std::cerr << "cyclostationary panel state rendering failed\n"; return 1;
    }
    std::cout << "cyclostationary panel disabled/waiting/measured/gap/stale states passed\n";
}
