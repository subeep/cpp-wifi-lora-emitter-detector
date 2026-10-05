#pragma once

#include "ofdm_structure.hpp"
#include "spectral_correlation.hpp"
#include "capture_timing.hpp"
#include "process_client.hpp"
#include "burst_selection.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace rfmon::cyclo {

struct CaptureContext {
    int band_ghz = 0; // 2 (2.4 GHz) or 5; never a protocol inference
    std::uint64_t radio_session = 0, capture_sequence = 0;
    std::uint64_t submission_epoch = 0; // obtained before capture starts
    double sample_rate_hz = 0, capture_center_hz = 0;
    std::size_t source_samples = 0;
    bool device_time_valid = false;
    std::int64_t first_device_time_ns = 0;
    bool capture_overflow = false, timed_out = false;
};

struct ShadowResult {
    CaptureContext capture;
    std::uint64_t epoch = 0;
    std::size_t copied_samples = 0;
    double processing_ms = 0;
    std::chrono::steady_clock::time_point completed_at;
    std::vector<TileMeasurement> tiles; // independently analyzed; never stitched
    TileSelection selection;
};

struct ShadowStats {
    bool enabled = false;
    std::uint64_t submitted = 0, measured = 0, dropped_busy = 0, dropped_invalid = 0;
    std::uint64_t discarded = 0, failures = 0, epoch = 0;
    std::size_t high_water = 0;
    double max_submit_ms = 0;
    std::uint64_t worker_launches = 0, worker_restarts = 0, worker_timeouts = 0, protocol_errors = 0;
    int worker_pid = -1;
    std::uint64_t burst_guided_jobs = 0, hint_descriptors_examined = 0;
};

// Conservative prefix only. Unknown/invalid gap metadata returns no samples.
std::size_t continuous_capture_prefix(const CaptureTiming& timing, std::size_t received,
                                      bool overflow_reported) noexcept;

// Experimental receive-only sidecar. It owns no radio or existing subsystem.
// Producer never waits for queue space or a mutex, never allocates, and never
// exposes exceptions. Eight preallocated 2 MiB slots exist only while enabled.
class ShadowWorker {
public:
    static constexpr std::size_t slot_count = 8, samples_per_slot = 262144;
    explicit ShadowWorker(WorkerOptions options = {}) : options_(std::move(options)) {
        if (options_.deadline_ms < 50 || options_.deadline_ms > 10000 || options_.restart_limit > 8)
            throw std::invalid_argument("invalid cyclostationary worker supervision limits");
    }
    ~ShadowWorker();
    ShadowWorker(const ShadowWorker&) = delete;
    ShadowWorker& operator=(const ShadowWorker&) = delete;
    bool set_enabled(bool enable) noexcept; // UI/lifecycle thread only
    bool enabled() const noexcept { return enabled_.load(); }
    std::uint64_t epoch() const noexcept { return epoch_.load(); }
    bool try_submit(const std::vector<std::complex<float>>& iq, std::size_t contiguous_prefix_samples,
                    const CaptureContext& capture, const BurstHints& hints = {}) noexcept;
    void invalidate() noexcept; // band/channel change; cancels stale queued/results
    std::shared_ptr<const ShadowResult> snapshot() const noexcept;
    ShadowStats stats() const noexcept;
private:
    enum class State { free, queued, working };
    struct Slot {
        std::vector<std::complex<float>> iq;
        CaptureContext capture;
        TilePlan plan;
        TileSelection selection;
        State state = State::free;
        std::uint64_t sequence = 0, epoch = 0;
    };
    void run() noexcept;
    const WorkerOptions options_;
    std::array<Slot, slot_count> slots_;
    std::mutex lifecycle_mutex_, queue_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stopping_ = false; // queue mutex
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint64_t> epoch_{0}, submitted_{0}, measured_{0}, busy_{0}, invalid_{0}, discarded_{0}, failures_{0};
    std::atomic<std::size_t> high_water_{0};
    std::atomic<std::uint64_t> max_submit_ns_{0};
    std::atomic<std::uint64_t> launches_{0}, restarts_{0}, timeouts_{0}, protocol_errors_{0};
    std::atomic<int> worker_pid_{-1};
    std::atomic<std::uint64_t> burst_guided_jobs_{0}, hint_descriptors_examined_{0};
    std::shared_ptr<const ShadowResult> latest_; // C++17 atomic shared_ptr operations
};
} // namespace rfmon::cyclo
