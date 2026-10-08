#include "cyclostationary/panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <iostream>

namespace {
void export_draw(const std::string& path) {
    using nlohmann::json;
    auto* data=ImGui::GetDrawData();json j={{"width",data->DisplaySize.x},{"height",data->DisplaySize.y},{"lists",json::array()}};
    unsigned char* pixels=nullptr;int width=0,height=0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    j["atlas_width"]=width;j["atlas_height"]=height;
    std::ofstream atlas(path+".rgba",std::ios::binary);atlas.write(reinterpret_cast<const char*>(pixels),width*height*4);
    for(const auto* list:data->CmdLists) {
        json item={{"vertices",json::array()},{"indices",json::array()},{"commands",json::array()}};
        for(const auto& v:list->VtxBuffer)item["vertices"].push_back({v.pos.x,v.pos.y,v.uv.x,v.uv.y,v.col});
        for(const auto i:list->IdxBuffer)item["indices"].push_back(i);
        for(const auto& c:list->CmdBuffer) {
            if(c.UserCallback)throw std::runtime_error("preview does not support draw callbacks");
            item["commands"].push_back({{"clip",{c.ClipRect.x,c.ClipRect.y,c.ClipRect.z,c.ClipRect.w}},
                {"first",c.IdxOffset},{"count",c.ElemCount},{"vertex_offset",c.VtxOffset}});
        }
        j["lists"].push_back(std::move(item));
    }
    std::ofstream output(path);output<<j.dump();
    if(!output || !atlas)throw std::runtime_error("preview output failed");
}
}
int main(int argc,char** argv) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = argc==2?ImVec2(1200,1200):ImVec2(1600,1000); io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::string rendered_text;
    auto draw = [&](const std::shared_ptr<const rfmon::cyclo::ShadowResult>& r, rfmon::cyclo::ShadowStats stats, int band, bool expanded=false) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin(argc==2?"Cyclostationary GUI - synthetic measurement preview":"Cyclostationary panel verification");
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Expanded cyclic and waveform measurements"),expanded);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("General timing and short-code structure"),expanded);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("General linear sweep measurements"),expanded);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Cyclic background checks"),expanded);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Burst-local waveform analysis"),expanded);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Timing drift refinement"),expanded || argc==2);
        ImGui::LogToBuffer(0);
        rfmon::cyclo::draw_shadow_panel(r, stats, band);
        rendered_text=ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
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
    result->tiles[0].spectral.frames=64;result->tiles[0].spectral.variance_power=1;
    result->tiles[0].spectral.bin_hz=20e6/512;result->tiles[0].spectral.power_fraction.assign(512,1./512);
    result->tiles[0].spectral.occupied_low_hz=-7e6;result->tiles[0].spectral.occupied_high_hz=7e6;
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
    result->tiles[0].waveform.cyclic_status="measured"; result->tiles[0].waveform.morphology_status="measured";
    result->tiles[0].waveform.partition_samples=8192; result->tiles[0].waveform.alpha_bin_hz=20e6/8192;
    result->tiles[0].waveform.peaks.push_back({true,1,1e6,1e6,.6,.5,true});
    result->tiles[0].waveform.phase_pairs=65535;
    result->tiles[0].structure.status="measured";
    result->tiles[0].structure.partition_samples=8192;result->tiles[0].structure.holdout_offset=57344;
    result->tiles[0].structure.timing_hypotheses=32;result->tiles[0].structure.spread_hypotheses=32;
    result->tiles[0].structure.samples_examined=65536;result->tiles[0].waveform.samples_examined=65536;
    result->tiles[0].structure.ofdm.push_back({96,24,0,67,.04,1,.001,1,.001,.03,true});
    result->tiles[0].structure.spread.push_back({8,0,93,92,3e6,1,1,.1,.1,true});
    result->tiles[0].review=rfmon::cyclo::review_waveform_measurements(result->tiles[0].spectral,result->tiles[0].structure,result->tiles[0].waveform,{20e6,0,-1});
    auto& sweep=result->tiles[0].sweeps;sweep.status="measured";sweep.samples_examined=65536;
    sweep.partition_samples=8192;sweep.holdout_partition_offset=57344;sweep.discovery_trials=498;
    rfmon::cyclo::LinearSweep sw;sw.span_samples=512;sw.slope_hz_per_second=3.9e11;sw.sweep_hz=9.96e6;
    sw.holdout_trials=31;sw.discovery_coherence_squared=.99;sw.holdout_coherence_squared=.98;
    sw.holdout_supported=true;sw.pattern_consistent=true;sweep.candidates.push_back(sw);
    result->tiles[0].sweep_review=rfmon::cyclo::review_linear_sweeps(result->tiles[0].spectral,sweep,{20e6,0,-1});
    auto& bg=result->tiles[0].cyclic_background;bg.status="measured";bg.samples_examined=65536;
    bg.partition_samples=8192;bg.holdout_offset=57344;bg.fft_calls=2;
    rfmon::cyclo::CyclicBackgroundPeak bp;bp.discovery.reference_bins=34;bp.holdout.reference_bins=34;
    bp.discovery.line_to_median=2;bp.holdout.line_to_median=2;bp.discovery.line_to_upper=1;bp.holdout.line_to_upper=1;
    bp.discovery.block_phase_coherence_squared=.3;bp.holdout.block_phase_coherence_squared=.4;
    bp.discovery.min_block_energy_fraction=.1;bp.holdout.min_block_energy_fraction=.1;bg.peaks.push_back(bp);
    result->tiles[0].background_review=rfmon::cyclo::review_cyclic_background(result->tiles[0].spectral,result->tiles[0].waveform,bg,{20e6,0,-1});
    std::vector<std::complex<float>> burst_iq(65536);
    for(std::size_t i=16384;i<24576;++i)burst_iq[i]=(i%7<3?1.f:-1.f)*std::polar(1.f,float(3.14159265358979323846*(i%256)/128));
    auto& burst=result->tiles[0].burst;
    burst=rfmon::cyclo::analyze_burst(burst_iq,20e6,rfmon::cyclo::measure_rois(burst_iq,20e6));
    rfmon::cyclo::review_burst(burst,{20e6,0,-1});
    const auto begin=burst_iq.begin()+burst.selection.offset;
    result->tiles[0].clock=rfmon::cyclo::refine_structure_clock({begin,begin+burst.selection.samples},20e6,burst.structure);
    rfmon::cyclo::review_clock_refinement(result->tiles[0].clock,burst.spectral,{20e6,0,-1});
    const auto expanded = draw(result, stats, 2, true);
    if(rendered_text.find("Competing patterns")==std::string::npos || rendered_text.find("Barker-11 code")==std::string::npos ||
       rendered_text.find("Drone: unknown")==std::string::npos || rendered_text.find("Conjugate cycles")==std::string::npos || rendered_text.find("Linear sweep shape")==std::string::npos ||
       rendered_text.find("Upward sweep")==std::string::npos || rendered_text.find("carrier centre is fitted")==std::string::npos || rendered_text.find("Local background checks failed")==std::string::npos ||
       rendered_text.find("Cyclic background:")==std::string::npos ||
       rendered_text.find("Timing refinement:")==std::string::npos || rendered_text.find("Timing review:")==std::string::npos ||
       rendered_text.find("Burst cyclic background:")==std::string::npos || rendered_text.find("Source samples 16384..24576")==std::string::npos) {
        std::cerr<<"new review summary or unknown identity missing from GUI text\n";return 1;
    }
    if(argc==2){draw(result,stats,2,false);export_draw(argv[1]);}
    result->capture.band_ghz=5;const auto measured5=draw(result,stats,5);
    result->capture.band_ghz=2;
    if(measured5<=waiting){std::cerr<<"5 GHz review panel missing\n";return 1;}
    const auto other_band = draw(result, stats, 5);
    ++stats.epoch; const auto stale = draw(result, stats, 2);
    result->tiles[0].ofdm[0].status = "insufficient_symbols"; result->capture.capture_overflow = true;
    result->selection.burst_guided[0] = true; result->selection.burst_windows = 1;
    result->selection.detector_capped = true; result->selection.hints_examined = 512;
    result->selection.hints_reported = 4000;
    stats.epoch = 3; const auto warning = draw(result, stats, 2);
    ImGui::DestroyContext();
    if (disabled <= 0 || waiting <= 0 || measured <= waiting || qualified <= waiting || expanded <= qualified || other_band != waiting || stale != waiting || warning <= waiting) {
        std::cerr << "cyclostationary panel state rendering failed\n"; return 1;
    }
    std::cout << "cyclostationary panel disabled/waiting/measured/gap/stale states passed\n";
}
