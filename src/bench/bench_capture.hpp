// Wi-Fi 2.4GHz test-bench capture engine - a SEPARATE, standalone
// pipeline from Scanner (scanner.hpp/.cpp), built for one purpose: let
// you send a custom test packet at a known frequency and watch every
// parameter this project's Wi-Fi RF chain computes for it, live, plus
// replay it afterwards. It does not touch Scanner, the persistent
// wifi_master_/lora_master_ lists, or anything else the main
// rf_monitor_gui app owns - this links against the SAME production
// wifi_phy.cpp/wifi_fingerprint.cpp/wifi_frame.cpp/wifi_dsss_rx.cpp
// files (not copies), so "the math" is identical, but it is a
// completely independent capture loop:
//   - single locked frequency (not a multi-channel scan plan)
//   - a user-chosen SDR device (B210 or X310, see set_device_type())
//     AND a user-chosen RX channel (0/1) - same two independent
//     selectors main.cpp's own device selector exposes, so you can run
//     two instances of this same binary side by side, one per device,
//     and compare the SAME test transmission's measured parameters
//     across two different pieces of hardware
//   - decodes/fingerprints EVERY classified burst unconditionally,
//     not just beacon-duration-shaped ones (production's
//     WIFI_BEACON_MIN_DURATION_S gate exists to keep a busy real
//     channel's phantom-source rate down - irrelevant here, where the
//     "channel" is one deliberately-sent test transmission at a time)
//   - retains each classified burst's raw IQ window (for the top-half
//     plot and post-disconnect playback), which Scanner's WifiPacketRow
//     deliberately does not (that log runs unbounded in spirit and
//     would never fit raw IQ in memory)

#pragma once

#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "sdr_capture.hpp"
#include "wifi_fingerprint.hpp"
#include "wifi_frame.hpp"

namespace rfmon::bench {

struct BenchStatus {
    bool connected = false;
    std::string error;
    bool running = false;
    int chunk_count = 0;
    bool last_overflow = false;
    bool rx_stalled = false;
    double actual_sample_rate_hz = 0.0;
};

// One classified Wi-Fi burst, with every parameter wifi_fingerprint.cpp
// / wifi_phy.cpp / wifi_frame.cpp compute for it - a superset of
// scanner.hpp's WifiPacketRow (adds fp_snr_db/fp_dc_ang_deg/fp_n_samp,
// which the main app's table doesn't surface today, plus the raw IQ
// window itself for replay).
struct BenchPacketRow {
    uint64_t seq = 0;  // stable id - packets_ can evict from the front, vector index cannot be used as a key
    std::string time;  // HH:MM:SS
    double freq_mhz = 0.0;  // the locked TARGET frequency (segment center), not the offset USRP tuner frequency
    std::string modulation;  // "DSSS" or "OFDM"
    double power_db = 0.0;
    double bandwidth_khz = 0.0;
    double duration_us = 0.0;
    double confidence = 0.0;

    std::optional<wifi::BeaconInfo> identity;  // set only for an FCS-valid DSSS beacon/probe response

    std::optional<double> fp_cfo_ppm;
    std::optional<double> fp_irr_db;
    std::optional<double> fp_iq_eps;
    std::optional<double> fp_iq_phi_deg;
    std::optional<double> fp_dc_dbc;
    std::optional<double> fp_dc_ang_deg;
    std::optional<double> fp_snr_db;
    std::optional<double> fp_evm_pct;
    std::optional<double> fp_sync_corr;
    std::optional<int> fp_n_samp;
    std::optional<std::string> fp_gate_reason;

    // Raw IQ for the burst window, at the rate/centers it was captured
    // at - enough to reproduce the exact same IQ/FFT plot on replay
    // that was shown live. capture_center_hz is where the USRP was
    // actually tuned (target + WIFI_CHANNEL_CAPTURE_OFFSET_HZ);
    // segment_center_hz is the real target frequency (== freq_mhz*1e6).
    std::vector<std::complex<float>> iq;
    double sample_rate_hz = 0.0;
    double capture_center_hz = 0.0;
    double segment_center_hz = 0.0;
};

using BenchPacketPtr = std::shared_ptr<const BenchPacketRow>;

// The most recent raw capture chunk, kept for the top-half live view
// while nothing has classified yet (or to inspect the noise floor
// between packets). Same shared_ptr-snapshot reasoning as
// BenchPacketRow - this can be tens of millions of samples, so the GUI
// thread must never deep-copy it every frame.
struct LiveChunk {
    std::vector<std::complex<float>> iq;
    double sample_rate_hz = 0.0;
    double capture_center_hz = 0.0;
    double segment_center_hz = 0.0;
    uint64_t chunk_id = 0;
};
using LiveChunkPtr = std::shared_ptr<const LiveChunk>;

constexpr size_t BENCH_PACKET_LOG_MAX = 300;

class BenchCapture {
public:
    BenchCapture();
    ~BenchCapture();

    // Which physical SDR to connect to - only takes effect on the next
    // start(), exactly like Scanner::set_device_type() (changing it
    // while running does not hot-swap hardware).
    void set_device_type(SdrDeviceType type);
    SdrDeviceType device_type() const;

    // Connects on the given RX channel (0 or 1 - B210: the two RF
    // chains on its one integrated AD9361; X310: the two independent
    // UBX-160 daughterboard slots, see config.hpp's DeviceProfile
    // comment) and spawns the capture thread. No-op if already running.
    void start(size_t usrp_channel);
    void stop();
    bool running() const;

    // Target frequency is the real signal center you want to receive -
    // the USRP is actually tuned WIFI_CHANNEL_CAPTURE_OFFSET_HZ away
    // from it (same DC-guard-avoidance convention scanner.cpp uses),
    // transparently to every formula downstream. Takes effect on the
    // capture thread's next loop iteration; no reconnect needed.
    void set_target_freq_hz(double hz);
    double target_freq_hz() const;

    void set_gain(std::optional<double> gain_db);  // nullopt = AGC (only the B210's AD9361 supports it - see DeviceProfile::supports_agc)
    void set_threshold_db(double db);
    double threshold_db() const;
    void set_chunk_duration_s(double s);
    double chunk_duration_s() const;

    BenchStatus status() const;
    size_t usrp_channel() const { return usrp_channel_; }

    // Cheap: copies the vector of shared_ptrs, not the packets
    // themselves. Most-recent-last, matching scanner.cpp's convention.
    std::vector<BenchPacketPtr> packets_snapshot() const;
    void clear_packets();

    LiveChunkPtr latest_chunk_snapshot() const;

private:
    void run();
    bool connect_sdr(SdrDeviceType type, size_t usrp_channel, std::optional<double> gain);

    std::unique_ptr<UsrpCapture> sdr_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};

    mutable std::mutex config_mutex_;
    SdrDeviceType device_type_ = SdrDeviceType::X310;
    double target_freq_hz_ = 2437e6;  // Wi-Fi channel 6 center, a reasonable default
    double threshold_db_ = 12.0;
    double chunk_duration_s_ = 0.3;
    std::optional<double> gain_db_;
    bool gain_dirty_ = false;
    size_t usrp_channel_ = 0;

    mutable std::mutex status_mutex_;
    BenchStatus status_;
    int rx_fail_streak_ = 0;
    static constexpr int kRxStallThreshold = 3;

    mutable std::mutex packet_mutex_;
    std::vector<BenchPacketPtr> packets_;
    uint64_t next_seq_ = 0;

    mutable std::mutex chunk_mutex_;
    LiveChunkPtr latest_chunk_;
    uint64_t next_chunk_id_ = 0;
};

}  // namespace rfmon::bench
