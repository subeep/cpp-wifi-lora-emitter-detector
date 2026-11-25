#pragma once

#include "symbol_timing.hpp"
#include "link_clock.hpp"
#include "security/wifi_frame_event.hpp"

#include <chrono>
#include <cmath>
#include <complex>
#include <optional>
#include <string_view>
#include <string>

namespace rfmon::symbol_timing {
// Where a link's subcarrier grid sits, modulo its spacing (display and logging only). From the
// phase of the cyclic-prefix correlation summed over the blocks that flagged the hypothesis:
// hz is the RF frequency of a grid line modulo spacing_hz, in [0, spacing_hz). coherence is
// |sum| / sum of magnitudes (1: every block agrees; low: mixed transmitters or noise).
struct GridOffset {
    double hz{}, spacing_hz{}, coherence{};
    std::size_t blocks{};
};
// The receiver's own crystal error shifts every grid by the same ppm, so values from one X310
// compare with each other only. Turning hz into a transmitter's ppm needs its nominal channel.

struct Result {
    std::string run_id;
    uint64_t capture_seq{}, radio_session{};
    int64_t device_ns{}, host_before_ns{}, host_after_ns{};
    wifi_security::ClockDomain clock{};
    double center{}, rate{}, processing_ms{};
    std::size_t blocks{};
    // Samples in the whole capture the blocks came from (block_start positions); 0 when the
    // blocks are not from one capture, which skips the link clock.
    std::size_t capture_samples{};
    Fractions fractions{};
    const char* timing_group{};
    std::array<std::optional<GridOffset>, hypotheses.size()> grid{};
    // The timing group's link clock (Lightbridge or 15 kHz OcuSync), when measured.
    std::optional<link_clock::Estimate> link_clock{};
    const char* constants_version{version};
    std::chrono::steady_clock::time_point observed{};
};

struct Snapshot {
    uint64_t full{}, contention{}, invalid{}, unavailable{}, completed{};
    double max_handoff_ms{};
    std::vector<Result> history;
};

inline std::optional<GridOffset> grid_offset(std::complex<double> sum, double magnitude_sum, std::size_t blocks,
                                      double spacing_hz, double center_hz) {
    if (!blocks || !(magnitude_sum > 0) || !(spacing_hz > 0) || !std::isfinite(center_hz) ||
        !std::isfinite(sum.real()) || !std::isfinite(sum.imag()) || std::abs(sum) == 0)
        return std::nullopt;
    const double baseband = std::arg(sum) / (2 * std::acos(-1.)) * spacing_hz;
    double hz = std::fmod(center_hz + baseband, spacing_hz);
    if (hz < 0) hz += spacing_hz;
    if (hz >= spacing_hz) hz = 0;
    return GridOffset{hz, spacing_hz, std::abs(sum) / magnitude_sum, blocks};
}

// The grid offset for the result's timing group (15 kHz for "15kHz+30kHz"; for Lightbridge the
// 10.9375 kHz grid when measured, else the third-lag one), if measured.
inline std::optional<GridOffset> link_grid(const Result& result) {
    if (!result.timing_group) return std::nullopt;
    const std::string_view group = result.timing_group;
    if (group == "32.8kHz") return result.grid[lightbridge] ? result.grid[lightbridge] : result.grid[lightbridge_third];
    return result.grid[group == "30kHz" ? lte_30k : lte_15k];
}

// A "30kHz" or "15kHz+30kHz" group is kept only when its 30 kHz grid's coherence reaches this
// (otherwise "15kHz" or no group).
inline constexpr double min_30k_grid_coherence = 0.5;
// An OcuSync clock from a capture with Lightbridge timing in some blocks is measured on the other
// blocks, when at least this many remain (of 24).
inline constexpr std::size_t min_clean_ocusync_blocks = 8;

// Measures result.blocks blocks laid end to end at `samples` (block_size each) with result.rate
// and result.center already set: fills fractions, grid, timing_group and, when
// result.capture_samples is set, link_clock. False for nonfinite IQ.
// The live monitor and offline tools use this one function.
bool measure_blocks(Probe& probe, const std::complex<float>* samples, Result& result);

class Monitor {
public:
    Monitor();
    ~Monitor();
    void start();
    void stop();
    void observe(const std::vector<std::complex<float>>& iq,
                 const wifi_security::CaptureRecord& capture) noexcept;
    Snapshot snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
