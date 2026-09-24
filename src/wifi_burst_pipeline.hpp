// The per-burst Wi-Fi classification and decode decisions, shared verbatim
// by the production scanner and the offline security runner
// (tools/wifi_security_replay.cpp). One copy, so a recording replayed
// offline goes through exactly the gates and decode windows the live
// scanner applied - no second, drifting reimplementation.
//
// Only the decisions live here. Identity recording, fingerprinting, the
// packet table and beacon-cadence clustering stay in scanner.cpp.
#pragma once

#include <algorithm>
#include <atomic>
#include <complex>
#include <optional>
#include <thread>
#include <vector>

#include "wifi_burst_policy.hpp"
#include "wifi_dsss_rx.hpp"
#include "wifi_ofdm_rx.hpp"
#include "wifi_phy.hpp"

namespace rfmon::wifi {

struct BurstClassification {
    enum class Outcome { Unknown, Narrowband, Accepted };
    Outcome outcome = Outcome::Unknown;
    ModClassification result;
    double bandwidth_hz = 0.0, power_db = 0.0, duration_s = 0.0;
};

// Correlator classification, then the narrowband gate: nothing in Wi-Fi is
// narrowband, and rejecting sub-4 MHz bursts is what keeps Bluetooth, BLE
// and Zigbee - all of which share 2.4 GHz and can correlate against Barker -
// out of the packet list (see MIN_WIFI_BANDWIDTH_HZ).
inline BurstClassification classify_burst(const std::vector<std::complex<float>>& window, double sample_rate_hz,
                                          double capture_center_hz, double channel_hz, bool try_dsss) {
    BurstClassification c;
    c.result = classify_modulation(window, sample_rate_hz, capture_center_hz, channel_hz, try_dsss);
    if (c.result.mod == ModClass::Unknown) return c;
    c.bandwidth_hz = estimate_occupied_bandwidth_hz(window.data(), window.size(), sample_rate_hz);
    if (c.bandwidth_hz < MIN_WIFI_BANDWIDTH_HZ) {
        c.outcome = BurstClassification::Outcome::Narrowband;
        return c;
    }
    c.outcome = BurstClassification::Outcome::Accepted;
    c.power_db = estimate_mean_power_db(window.data(), window.size());
    c.duration_s = double(window.size()) / sample_rate_hz;
    return c;
}

// Energy segmentation can trim the preamble, so OFDM decodes with 4 us of
// context either side. OFDM does not use the DSSS duration gate.
inline OfdmDecodeResult decode_ofdm_with_context(const std::complex<float>* capture, size_t capture_n,
                                                 const BurstWindow& b, double sample_rate_hz,
                                                 double capture_center_hz, double channel_hz) {
    const size_t pad = size_t(sample_rate_hz * 4e-6);
    const size_t start = b.start > pad ? b.start - pad : 0;
    const size_t end = std::min(capture_n, b.start + b.length + pad);
    return decode_ofdm_burst(capture + start, end - start, sample_rate_hz, capture_center_hz, channel_hz);
}

struct DsssBurstDecode {
    DsssBurstPolicy policy;
    std::optional<DsssDecodeResult> result;  // set exactly when policy.decode
};

inline DsssBurstDecode decode_dsss_with_policy(const std::vector<std::complex<float>>& window,
                                               double sample_rate_hz, double capture_center_hz,
                                               double channel_hz, double duration_s) {
    DsssBurstDecode d;
    d.policy = dsss_burst_policy(duration_s);
    if (d.policy.decode)
        d.result = decode_dsss_burst(window.data(), window.size(), sample_rate_hz, capture_center_hz, channel_hz);
    return d;
}

// Everything about one burst that is a pure function of the capture: the
// classification/gate and whichever decode applies. Independent across
// bursts, which is what lets process_bursts() spread them over cores.
struct BurstWork {
    BurstClassification cls;
    std::optional<OfdmDecodeResult> ofdm;  // set for accepted OFDM bursts
    DsssBurstDecode dsss;                  // policy set for accepted DSSS bursts; result when decoded
};

inline BurstWork process_burst(const std::complex<float>* capture, size_t capture_n, const BurstWindow& b,
                               double sample_rate_hz, double capture_center_hz, double channel_hz, bool try_dsss) {
    BurstWork w;
    std::vector<std::complex<float>> window(capture + b.start, capture + b.start + b.length);
    w.cls = classify_burst(window, sample_rate_hz, capture_center_hz, channel_hz, try_dsss);
    if (w.cls.outcome != BurstClassification::Outcome::Accepted) return w;
    if (w.cls.result.mod == ModClass::OFDM)
        w.ofdm = decode_ofdm_with_context(capture, capture_n, b, sample_rate_hz, capture_center_hz, channel_hz);
    else
        w.dsss = decode_dsss_with_policy(window, sample_rate_hz, capture_center_hz, channel_hz, w.cls.duration_s);
    return w;
}

// Worker count for process_bursts(): leave headroom for the acquisition and
// GUI threads, cap where returns diminish.
inline unsigned default_burst_workers() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 8u);
}

// process_burst() for every burst, spread over `workers` threads. Results
// are stored by burst index, so the output is identical to a sequential
// loop regardless of thread count or scheduling - determinism the offline
// runner and its record/replay equality depend on.
inline std::vector<BurstWork> process_bursts(const std::complex<float>* capture, size_t capture_n,
                                             const std::vector<BurstWindow>& bursts, double sample_rate_hz,
                                             double capture_center_hz, double channel_hz, bool try_dsss,
                                             unsigned workers = default_burst_workers()) {
    std::vector<BurstWork> out(bursts.size());
    std::atomic<size_t> next{0};
    auto run = [&] {
        for (size_t i; (i = next.fetch_add(1)) < bursts.size();)
            out[i] = process_burst(capture, capture_n, bursts[i], sample_rate_hz, capture_center_hz, channel_hz, try_dsss);
    };
    workers = std::max(1u, std::min<unsigned>(workers, unsigned(bursts.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < workers; ++t) pool.emplace_back(run);
    run();
    for (auto& t : pool) t.join();
    return out;
}

}  // namespace rfmon::wifi
