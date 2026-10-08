#pragma once
#include "link_evidence.hpp"
#include "chirp_discovery.hpp"
#include "cyclic_background.hpp"
#include "structure_discovery.hpp"
#include "waveform_features.hpp"

namespace rfmon::cyclo {
inline constexpr const char* waveform_review_version = "expanded_measurement_review_v2";
inline constexpr std::size_t review_candidate_limit = 18;
struct ReviewedPattern {
    std::string kind, label, status;
    std::size_t measurement_index = 0;
    bool pattern_consistent = false;
    std::vector<EvidenceCheck> checks;
    std::vector<std::string> reasons;
};
struct WaveformReview {
    EvidenceContext context;
    std::string status, passband_status;
    bool observed_quality_passed = false, cp_code_ambiguous = false;
    double extent_plus_bin_hz = 0;
    std::vector<EvidenceCheck> quality_checks;
    std::vector<ReviewedPattern> candidates;
};
// Metadata only; bounded, deterministic and separate from experimental_dsp_v1.
// No IQ processing, probability, exact waveform or drone/family acceptance.
WaveformReview review_waveform_measurements(const SpectralFeatures& spectral,
    const StructureDiscovery& structure, const WaveformFeatures& waveform,
    const EvidenceContext& context);
inline constexpr const char* sweep_review_version="linear_sweep_review_v1";
WaveformReview review_linear_sweeps(const SpectralFeatures& spectral,
    const ChirpDiscovery& sweeps,const EvidenceContext& context);
inline constexpr const char* background_review_version="cyclic_background_review_v1";
WaveformReview review_cyclic_background(const SpectralFeatures& spectral,const WaveformFeatures& selected,
    const CyclicBackground& background,const EvidenceContext& context);
} // namespace rfmon::cyclo
