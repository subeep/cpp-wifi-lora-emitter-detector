#pragma once
#include "security/wifi_frame_event.hpp"
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rfmon::ml {
struct Candidate { std::string label; double score=0; };
struct Result {
    std::string status, candidate, accepted, reason, raw, source, band, device;
    std::string model_hash, architecture, run;
    uint64_t session=0, capture=0, generation=0;
    double rate=0, center=0, processing_ms=0, total_ms=0;
    double selected_offset=0;
    bool selected_offset_available=false;
    std::vector<Candidate> top;
    std::chrono::steady_clock::time_point completed{};
    bool current=false;
};
struct Snapshot {
    std::string state="Disabled", error, model_hash, architecture, classes;
    bool enabled=false, ready=false;
    uint64_t offered=0, submitted=0, dropped=0, invalid=0, stale=0, processed=0;
    uint64_t offered_samples=0, submitted_samples=0;
    double max_observer_ms=0;
    size_t queued=0;
    std::vector<Result> history;
};
struct Options { std::string python, script, model; int deadline_ms=15000; };
// Optional processing-tail consumer. Never owns a receiver or modifies an input.
class Inference {
public:
    static constexpr size_t max_samples=4000000, history_limit=100;
    Inference()=default;
    ~Inference();
    bool start(Options options);
    void stop() noexcept;
    void invalidate();
    void clear_history();
    void observe(const std::vector<std::complex<float>>& iq,
                 const wifi_security::CaptureRecord& capture) noexcept;
    Snapshot snapshot() const;
private:
    struct Job {
        std::vector<std::complex<float>> iq;
        wifi_security::CaptureRecord capture;
        uint64_t generation=0;
        std::chrono::steady_clock::time_point submitted;
    };
    void run(Options options);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> enabled_{false}, ready_{false};
    std::atomic<uint64_t> generation_{0}, offered_{0}, dropped_{0}, invalid_{0}, offered_samples_{0};
    std::atomic<uint64_t> max_observer_ns_{0};
    Snapshot state_;
    Job pending_, working_;
    bool queued_=false;
    std::string source_key_;
    std::chrono::steady_clock::time_point last_submit_{};
};
}
