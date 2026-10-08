#pragma once
#include "structure_discovery.hpp"
#include "waveform_review.hpp"
#include <array>
namespace rfmon::cyclo {
inline constexpr const char* clock_refinement_version="discovery_selected_timing_grid_v1";
inline constexpr std::array<int,9> timing_grid_ppm{{-3000,-2000,-1000,-500,0,500,1000,2000,3000}};
inline constexpr std::size_t timing_seed_limit=2, timing_source_margin=16;
struct RefinedCp {
    std::size_t seed_index=0,grid_index=0;
    DiscoveredOfdm measurement;
};
struct RefinedCode {
    std::size_t seed_index=0,grid_index=0;
    SpreadMeasurement measurement;
};
struct ClockRefinement {
    std::string status="insufficient_samples";
    std::size_t samples_examined=0,partition_samples=0,target_samples=0,holdout_offset=0;
    std::size_t grids_tested=0,cp_trials=0,code_trials=0,interpolated_samples=0;
    std::vector<RefinedCp> cp; // at most one discovery-selected explanation
    std::vector<RefinedCode> code;
    WaveformReview review; // supervisor only on live path
};
double timing_grid_scale(std::size_t index);
ClockRefinement clock_refinement_plan(const StructureDiscovery& raw);
ClockRefinement refine_structure_clock(const std::vector<std::complex<float>>& iq,double rate,
                                     const StructureDiscovery& raw);
void review_clock_refinement(ClockRefinement& r,const SpectralFeatures& quality,const EvidenceContext& context);
} // namespace rfmon::cyclo
