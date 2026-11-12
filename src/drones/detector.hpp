#pragma once
#include "broadcast.hpp"
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace drone {
struct TelemetryField {
    std::string group, name, value, status;
};
struct Observation {
    std::string protocol, link, evidence, serial, model, source;
    double frequency{}, power{}, timestamp{}, latitude{}, longitude{}, altitude{};
    bool confirmed{}, legacy{};
    double sample_rate{}, carrier_correction_hz{};
    int64_t sequence{-1};
    std::string packet_json, position_status{"Unavailable"};
    std::string transmitter, remote_id_json, mavlink_json;
    std::vector<TelemetryField> telemetry;
    uint64_t count{1}, receiver_epoch{};
    std::chrono::steady_clock::time_point acquired_at{};
    // DroneID payload contract (drones/droneid_packet.hpp). Defaults mean "not stated": the
    // legacy backend does not fill these; native rows carry them in Snapshot::native_packets.
    int payload_version{0};
    bool identity_readable{false};
    int product_code{-1};
    std::string model_source;
};
struct Snapshot {
    std::string status{"Idle"};
    bool ready{};
    uint64_t samples{}, dropped{}, candidates{}, rejected{}, decoded{};
    double processing_ms{}, preprocessing_ms{}, queue_age_ms{}, oldest_queue_ms{}, end_to_end_ms{};
    size_t queue_depth{};
    uint64_t discontinuities{}, partial_discarded{}, stop_discarded{}, analysis_errors{};
    // Chunks refused at submit(), by cause. `dropped` above stays the total in samples.
    uint64_t dropped_full{}, dropped_busy{}, dropped_invalid{};
    uint64_t priority_chunks{}, displaced_chunks{}; // follow-up chunks accepted; ordinary chunks discarded for them
    // Native evidence stays distinct from authenticated identity (not provided).
    uint64_t native_packet_count{}, native_rows_omitted{}, o4_windows{}, o4_candidates{};
    std::vector<std::string> native_packets; // bounded JSON, receive metadata included
    std::string o4_latest;
    std::vector<Observation> observations;
    std::map<std::string, BroadcastTrack> tracks; // per validated serial, bounded by kMaxTracks
};
constexpr size_t kMaxTracks=64;
// Native backend input queue. One coordinated visit arrives as several consecutive <=160 ms
// chunks in a tight loop; a one-chunk queue dropped all but the first. These bounds hold about
// two 0.5 s visits at 20 MSps (192 MB of IQ at most) and refuse the rest, counted.
constexpr size_t kNativeQueueChunks=8, kNativeQueueSamples=24000000;
class Detector {
public:
    Detector() = default;
    ~Detector();
    // listed_channels_only (native backend): when a listed DroneID channel lies inside the capture,
    // the worker decodes only those channels and skips its slower automatic-channel pass. For live
    // captures, whose centre frequency is exact; offline replay keeps the automatic pass.
    void start(bool native_backend=false, bool listed_channels_only=false);
    void stop();
    bool running() const { return running_; }
    // priority (native backend): when the queue is full, waiting ordinary chunks are discarded
    // (oldest first, counted as dropped_full) to make room. Used for DroneID follow-up visits,
    // which exist only to be decoded. The chunk being decoded is never interrupted.
    void submit(const std::complex<float>* data, size_t count, double rate,
                double frequency, double timestamp, uint64_t epoch, const std::string& source = "IQ",
                bool priority = false);
    Snapshot snapshot();
private:
    struct Chunk {
        std::vector<std::complex<float>> data;
        double rate{}, frequency{}, timestamp{};
        uint64_t epoch{}, receiver_epoch{};
        std::string source;
        std::chrono::steady_clock::time_point enqueued;
        bool priority{};
    };
    void run();
    void status(const std::string& message, bool ready=false);
    std::atomic<bool> native_backend_{false}, listed_channels_only_{false}; // immutable while the worker runs
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0}, gap_{0}, dropped_full_{0}, dropped_busy_{0}, dropped_invalid_{0}, priority_chunks_{0}, displaced_chunks_{0};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Chunk> queue_;
    Snapshot snapshot_;
};
}
