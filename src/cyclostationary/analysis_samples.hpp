#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace rfmon::cyclo {

struct BandSelection {
    double sample_rate_hz = 0;
    double source_usable_bandwidth_hz = 0; // caller-declared RF passband
    double offset_hz = 0; // relative to the capture center
    double passband_width_hz = 0;
    std::size_t decimation = 1;
    bool remove_source_mean = false; // explicit private-copy option, before downmix
};

struct PreparedSamples {
    std::vector<std::complex<float>> samples;
    double sample_rate_hz = 0;
    double stopband_edge_hz = 0;
    std::size_t filter_taps = 0;
    std::size_t first_input_center = 0;
    std::size_t input_step = 1;
    std::complex<double> removed_source_mean{};
};

// Private downmix + symmetric Hamming-windowed sinc low-pass, then integer
// decimation. Drops filter transients. Output sample centers map explicitly to
// input coordinates; no relabelled rate, padding, extrapolation or phase joins.
PreparedSamples select_analysis_band(const std::vector<std::complex<float>>& iq,
                                     const BandSelection& selection);

} // namespace rfmon::cyclo
