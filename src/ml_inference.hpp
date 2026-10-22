#pragma once
#include "security/wifi_frame_event.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace rfmon::ml {
struct Candidate { std::string label; double score=0; };
struct StftPreview {
    int rows=256, cols=625;
    double db_min=0, db_max=0, rate_hz=20e6, duration_seconds=.008;
    int nfft=256, hop=256;
    std::vector<unsigned char> pixels; // Frequency ascending, then time.
};
struct Result {
    std::string status, candidate, accepted, reason, raw, source, band, device;
    std::string model_hash, architecture, run;
    uint64_t session=0, capture=0, generation=0;
    uint64_t offset_samples=0;
    size_t interval_index=0, interval_count=1;
    double rate=0, center=0, processing_ms=0, total_ms=0;
    double selected_offset=0;
    bool selected_offset_available=false;
    std::vector<Candidate> top;
    std::shared_ptr<const StftPreview> stft;
    std::chrono::steady_clock::time_point completed{};
    bool current=false;
};
struct Snapshot {
    std::string state="Disabled", error, model_hash, architecture, classes;
    std::string audit_status="disabled", audit_path, audit_error;
    uint64_t audit_saved=0;
    bool enabled=false, ready=false;
    uint64_t offered=0, submitted=0, dropped=0, invalid=0, stale=0, processed=0;
    uint64_t offered_samples=0, submitted_samples=0;
    uint64_t submitted_jobs=0;
    double max_observer_ms=0;
    size_t queued=0;
    std::vector<Result> history;
};
struct Options {
    std::string python, script, model; int deadline_ms=15000;
    std::string audit_root, audit_context="unlabelled", audit_notes;
    bool dispersed_intervals=true;
};
struct IntervalSelection {
    std::array<size_t,3> offsets{};
    size_t count=0, samples=0;
};
// Disjoint 8 ms views spread over a capture, within the existing copy budget.
inline IntervalSelection select_intervals(size_t available,size_t samples,bool dispersed,size_t budget=4000000) {
    IntervalSelection s;s.samples=samples;
    if(!samples||available<samples||budget<samples)return s;
    s.count=std::min<size_t>(dispersed?3:1,std::min(available/samples,budget/samples));
    for(size_t i=0;i<s.count;++i)
        s.offsets[i]=s.count==1?0:(available-samples)/(s.count-1)*i;
    return s;
}
// No presence-qualified model is deployed. Do not promote candidate scores.
inline const char* observation_decision(const Result& r) {
    if(!r.current)return "Historical observation - no current drone decision";
    if(r.status=="insufficient_evidence")return "Insufficient evidence - drone presence undetermined";
    return "Unclassified RF - drone presence undetermined";
}
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
        IntervalSelection selection;
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
    bool dispersed_intervals_=true; // Read/written only under mutex_.
    std::string source_key_;
    std::chrono::steady_clock::time_point last_submit_{};
};
}
