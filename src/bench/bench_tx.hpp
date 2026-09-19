// Wi-Fi 2.4GHz test-bench TRANSMIT engine - the SMA-loopback counterpart
// to bench_capture.hpp's receive side. Entirely separate from
// BenchCapture (and, like everything else under src/bench/, from the
// main rf_monitor_gui app): its own USRP session, its own USRP RX/TX
// channel choice, its own background thread.
//
// The point is a closed-loop sanity check: transmit a FIXED, KNOWN
// packet (DSSS or OFDM) out of one USRP channel, cable it via SMA
// (with attenuation - see the GUI's own reminder) into the RX side's
// antenna, lock the Live Capture tab to the same frequency, and watch
// whether the computed parameters (CFO, IRR, EVM, sync corr, ...) come
// out sane against a signal whose ground truth you actually control -
// something no over-the-air capture can offer, since there's no
// "known-correct" answer for someone else's AP.
//
// Both waveforms are built to be decodable by the SAME production
// wifi_frame.cpp / wifi_dsss_rx.cpp / wifi_fingerprint.cpp this bench's
// RX side already links (see bench_tx.cpp's own header for exactly how
// each field is derived) - so a successful loopback exercises the real
// decode path, not a toy shortcut.
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"

namespace rfmon::bench {

enum class TxPacketType { DSSS, OFDM };

struct BenchTxStatus {
    bool connected = false;
    std::string error;
    int send_count = 0;
    std::string last_action;
};

// Builds the fixed test waveform at `sample_rate_hz`, silence-padded on
// both ends. Exposed independent of any USRP connection - see
// bench_tx.cpp for the construction of each.
std::vector<std::complex<float>> build_dsss_test_waveform(double sample_rate_hz);
std::vector<std::complex<float>> build_ofdm_test_waveform(double sample_rate_hz);

class BenchTx {
public:
    BenchTx();
    ~BenchTx();

    // Which physical SDR to connect to - only takes effect on the next
    // start(), same contract as BenchCapture::set_device_type().
    void set_device_type(SdrDeviceType type);
    SdrDeviceType device_type() const;

    // Connects on the given TX channel (0 or 1 - same physical meaning
    // as BenchCapture's RX channel selector) and spawns the background
    // thread. No-op if already running.
    void start(size_t usrp_channel);
    void stop();
    bool running() const;
    size_t usrp_channel() const { return usrp_channel_; }

    void set_freq_hz(double hz);
    double freq_hz() const;
    void set_gain_db(double db);
    double gain_db() const;
    void set_packet_type(TxPacketType t);
    TxPacketType packet_type() const;

    // Requests one immediate transmit - picked up by the background
    // thread within its next poll tick (~20ms).
    void send_once();
    void set_repeat(bool enabled, double period_s);
    bool repeat_enabled() const;
    double repeat_period_s() const;

    BenchTxStatus status() const;

private:
    void run();
    bool connect_sdr(SdrDeviceType type, size_t usrp_channel);
    void transmit_one();

    uhd::usrp::multi_usrp::sptr usrp_;
    uhd::tx_streamer::sptr streamer_;
    double current_rate_ = -1.0;
    size_t usrp_channel_ = 0;
    SdrDeviceType connected_device_type_ = SdrDeviceType::X310;  // set by connect_sdr(), read by transmit_one()

    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> send_requested_{false};

    mutable std::mutex config_mutex_;
    SdrDeviceType device_type_ = SdrDeviceType::X310;
    double freq_hz_ = 2437e6;
    double gain_db_ = 10.0;
    TxPacketType packet_type_ = TxPacketType::DSSS;
    bool repeat_enabled_ = false;
    double repeat_period_s_ = 2.0;

    mutable std::mutex status_mutex_;
    BenchTxStatus status_;
};

}  // namespace rfmon::bench
