#pragma once
#include "roi_measurements.hpp"
#include "waveform_features.hpp"

namespace rfmon::cyclo {
inline constexpr std::size_t prepared_candidate_limit = 2;
struct CandidateBand {
    std::size_t region_index = 0;
    std::string proposal_kind;
    std::size_t source_offset = 0, source_samples = 0, first_input_center = 0, input_step = 1;
    std::size_t output_samples = 0, filter_taps = 0;
    double offset_hz = 0, width_hz = 0, output_rate_hz = 0;
    std::string status = "passband_unknown";
    WaveformFeatures waveform;
};
struct CandidateBands {
    std::size_t proposals_seen = 0, fir_operations = 0;
    // Accounting only: does not change proposal order or the existing DSP.
    std::size_t regions_seen = 0, regions_without_spectrum = 0, short_regions = 0;
    std::size_t geometry_eligible_proposals = 0, duplicates_of_returned = 0, budget_omitted = 0;
    std::vector<CandidateBand> candidates;
};
// Offline preparation only. Explicit usable width is necessary and is not a
// hardware calibration. Always retain raw/context measurements separately.
CandidateBands prepare_candidate_bands(const std::vector<std::complex<float>>& iq, double rate_hz,
                                      const RoiMeasurements& rois, double declared_width_hz);
} // namespace rfmon::cyclo
