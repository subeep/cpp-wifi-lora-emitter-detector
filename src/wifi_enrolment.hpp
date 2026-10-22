#pragma once
#include "wifi_master.hpp"
#include "security/wifi_frame_event.hpp"
#include <condition_variable>
#include <functional>
#include <map>
#include <thread>

namespace rfmon::wifi_master {
inline std::string enrolment_observation_key(const std::string& run,uint64_t session,uint64_t capture,size_t offset,const std::string& phy) {
    return run+"/"+std::to_string(session)+"/"+std::to_string(capture)+"/"+std::to_string(offset)+"/"+phy;
}
struct EnrolmentObservation {
    wifi_security::FrameEvent frame;
    std::optional<wifi_fingerprint::WifiFingerprint> fingerprint;
};
struct AnonymousReading {
    std::string phy;
    double hz=0, bandwidth=0;
    int64_t ts=0;
    wifi_fingerprint::WifiFingerprint fingerprint;
    std::string source;
};
struct EnrolmentBatch {
    wifi_security::CaptureRecord capture;
    std::vector<EnrolmentObservation> observations;
    std::vector<AnonymousReading> anonymous;
};
struct EnrolmentHealth {
    uint64_t dropped_batches=0, rejected_sources=0;
    size_t queued=0;
    std::string error, command_result;
};
// Owns the existing repository. One bounded background writer, no SDR/DSP work.
// The GUI reads immutable cached snapshots and submits explicit commands only.
class WifiEnrolment {
public:
    explicit WifiEnrolment(const std::string& directory);
    ~WifiEnrolment();
    bool submit(EnrolmentBatch batch);
    bool command(EnrolmentCommand command);
    void flush(); // shutdown/tests only; never called from the render loop
    std::vector<WifiMasterRow> snapshot() const;
    EnrolmentHealth health() const;
    std::map<std::string,std::string> associations() const;
    std::string storage_error() const { return health().error; }
private:
    struct State;
    struct Job { EnrolmentBatch batch; std::optional<EnrolmentCommand> command; size_t bytes=0; };
    void run();
    void process(const EnrolmentBatch& batch);
    void apply(const EnrolmentCommand& command);
    void publish();
    void associate(const std::string& source,const std::string& key);
    bool save(const std::string& key, const State& state);
    bool backup(const std::string& key);
    std::string directory_;
    WifiMasterList master_;
    std::map<std::string,std::unique_ptr<State>> states_;
    std::map<std::string,std::pair<uint64_t,uint64_t>> capture_highwater_;
    std::string error_, command_result_;
    bool last_save_replaced_=false;
    mutable std::mutex mutex_;
    std::condition_variable ready_, drained_;
    std::deque<Job> jobs_;
    size_t queued_bytes_=0;
    bool stopping_=false, busy_=false;
    uint64_t dropped_=0, rejected_sources_=0;
    std::vector<WifiMasterRow> rows_;
    std::map<std::string,std::string> associations_;
    std::deque<std::string> association_order_;
    std::string published_error_, published_command_;
    std::thread worker_;
};
}
