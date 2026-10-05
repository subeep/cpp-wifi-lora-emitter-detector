#pragma once
#include "spectral_correlation.hpp"
#include "ofdm_structure.hpp"
#include "chirp_structure.hpp"

namespace rfmon::cyclo {
inline constexpr const char* evidence_policy_version = "experimental_dsp_v1";
struct EvidenceContext {
    double sample_rate_hz = 0;
    double usable_bandwidth_hz = 0; // 0 means unknown; caller declaration, not hardware verification
    double full_scale_component_fraction = -1; // -1 unknown; source int16 rails only
};
struct EvidenceCheck {
    std::string name, comparison;
    double value = 0, threshold = 0;
    bool known = true, passed = false;
};
struct WaveformEvidence {
    std::string kind, hypothesis, status, passband_status;
    bool pattern_consistent = false, observed_quality_passed = false;
    std::vector<EvidenceCheck> checks;
};
struct LinkEvidence {
    EvidenceContext context;
    std::vector<EvidenceCheck> quality_checks;
    bool observed_quality_passed = false;
    std::vector<WaveformEvidence> candidates;
};
// Bounded, deterministic preview policy over already computed measurements.
// Thresholds are development-informed heuristics, NOT calibrated accuracy or
// named-family acceptance. No model, metadata label, state tracking or IQ DSP.
LinkEvidence assess_link_evidence(const SpectralFeatures& spectral,
    const std::vector<OfdmMeasurement>& ofdm, const std::vector<ChirpMeasurement>& chirps,
    const EvidenceContext& context);
} // namespace rfmon::cyclo
