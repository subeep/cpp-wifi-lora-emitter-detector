#pragma once
#include "lora_capture.hpp"
#include "lora_observation.hpp"
#include <nlohmann/json.hpp>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
namespace rfmon::lora_security {
constexpr size_t kMaxEvents=2048;
struct Event {
    std::string run_id;uint64_t capture_seq=0,radio_session=0;
    size_t sample_start=0,sample_end=0,start_uncertainty_samples=0;
    int sf=0; double bandwidth_hz=0;
    std::optional<int> cr;
    std::optional<bool> inverted_iq;
    bool header_valid=false,payload_complete=false;
    LoraPacketRow::Crc crc=LoraPacketRow::Crc::NotChecked;
    std::vector<uint8_t> payload;
    std::optional<int64_t> device_time_ns; // time of coarse start index, not exact RF arrival
    std::string decoder,lorawan_candidate;
    bool protocol_eligible() const {return header_valid&&payload_complete&&crc==LoraPacketRow::Crc::Valid&&decoder=="LoRa explicit PHY";}
};
struct Capture {
    std::string run_id,source,device,antenna;
    uint64_t capture_seq=0,radio_session=0;
    double center_hz=0,rate_hz=0;
    std::optional<double> gain_db;
    size_t received=0,analysed=0,hypotheses=0,deduplicated=0,unlocated=0;
    std::optional<CaptureTiming> timing;
    bool overflow=false,processed=false,analysis_limited=false,laboratory=false;
    bool supported=false;
    bool usable() const;
};
struct Batch {Capture capture;std::vector<Event> events;};
Batch make_batch(const LoraCapture& capture,size_t received,const std::vector<LoraPacketRow>& rows,bool laboratory=false);
nlohmann::json to_json(const Batch& batch);
Batch from_json(const nlohmann::json& json);
struct ProfileCoverage {
    double center_hz=0,rate_hz=0,sampled_s=0,usable_s=0;
    std::optional<double> gain_db;
    std::string antenna,device;
    uint64_t captures=0,excluded=0;
};
struct Snapshot {
    uint64_t captures=0,duplicates=0,out_of_order=0,excluded=0,device_timed=0;
    uint64_t events=0,eligible=0,crc_failed=0,crc_absent=0,partial=0,hypotheses_deduplicated=0;
    uint64_t input_loss=0,rejected_inputs=0;
    double sampled_s=0,analysed_s=0,usable_s=0,dead_s=0;
    uint64_t unknown_gaps=0,time_regressions=0,profiles_evicted=0;
    std::map<std::string,ProfileCoverage> coverage; // bounded receiver profiles
    std::string health="No LoRa captures observed",recording_error;
    Capture latest;
    std::deque<Event> recent; // bounded; counts above are cumulative, not retained-row counts
};
nlohmann::json snapshot_json(const Snapshot& snapshot);
class State {
public:
    void ingest(const Batch&);
    void note_loss(uint64_t count);
    void note_rejected();
    const Snapshot& snapshot() const {return snapshot_;}
private:
    Snapshot snapshot_;
    std::map<std::string,uint64_t> last_seq_; // bounded run contexts
    std::map<std::pair<std::string,uint64_t>,std::optional<int64_t>> session_ends_;
};
// Atomic whole-capture queue: cannot deliver frames without their denominator.
// The disk budget covers two generations, each at most half the configured total.
// Capture processing never waits for disk or the consumer. Rejection is explicit.
class Monitor {
public:
    explicit Monitor(std::string recording_path={},size_t queue_capacity=4,uint64_t record_max_bytes=64ull<<20);
    ~Monitor();
    void start();void stop();
    bool submit(Batch batch);
    std::shared_ptr<const Snapshot> snapshot() const;
private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Batch> queue_;
    std::shared_ptr<const Snapshot> published_;
    std::thread thread_;
    bool running_=false,stopping_=false;
    size_t queue_capacity_;uint64_t loss_=0,record_max_bytes_;
    std::string recording_path_;
    State state_;
};
} // namespace rfmon::lora_security
