#pragma once

#include "symbol_timing.hpp"
#include "security/wifi_frame_event.hpp"

#include <chrono>
#include <string>

namespace rfmon::symbol_timing {
struct Result {
    std::string run_id;
    uint64_t capture_seq{}, radio_session{};
    int64_t device_ns{}, host_before_ns{}, host_after_ns{};
    wifi_security::ClockDomain clock{};
    double center{}, rate{}, processing_ms{};
    std::size_t blocks{};
    Fractions fractions{};
    const char* timing_group{};
    const char* constants_version{version};
    std::chrono::steady_clock::time_point observed{};
};

struct Snapshot {
    uint64_t full{}, contention{}, invalid{}, unavailable{}, completed{};
    double max_handoff_ms{};
    std::vector<Result> history;
};

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
