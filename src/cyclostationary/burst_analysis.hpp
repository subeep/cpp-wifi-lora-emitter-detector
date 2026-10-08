#pragma once
#include "roi_measurements.hpp"
#include "waveform_review.hpp"

namespace rfmon::cyclo {
inline constexpr const char* burst_analysis_version="bounded_contiguous_burst_v1";
inline constexpr std::size_t burst_sample_limit=16384, burst_min_samples=2064;
struct BurstSelection {
    std::size_t eligible_regions=0, short_regions=0, budget_skipped_regions=0;
    std::size_t region_index=0, offset=0, samples=0;
    bool cropped=false;
    std::string status="no_contrast_region";
};
struct BurstAnalysis {
    BurstSelection selection;
    SpectralFeatures spectral; // local quality/PSD; SCF peaks omitted
    WaveformFeatures waveform;
    CyclicBackground background;
    StructureDiscovery structure;
    ChirpDiscovery sweeps;
    WaveformReview review, background_review, sweep_review; // supervisor-derived on live path
};
// Strongest eligible retained contrast ROI, earliest on equal energy. One
// contiguous central crop per tile; never pad, join regions or retune hardware.
BurstSelection select_burst_region(const RoiMeasurements& roi);
BurstAnalysis analyze_burst(const std::vector<std::complex<float>>& iq,double rate,
                          const RoiMeasurements& roi);
void review_burst(BurstAnalysis& burst,const EvidenceContext& context);
} // namespace rfmon::cyclo
