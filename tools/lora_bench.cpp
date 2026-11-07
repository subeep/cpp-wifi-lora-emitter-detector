// LoRa wideband processing benchmark: no radio. Builds a realistic capture
// (receiver noise plus the recorded SX1262 packet, FFT-upsampled to the
// capture rate and placed on several planned channels at different times),
// then runs the production capture/process path (Scanner::capture_lora /
// process_lora) and reports wall time, CPU seconds, peak memory, per-stage
// times and packets decoded. Restrict cores with taskset to model a smaller
// PC, e.g.  taskset -c 0-3 build/lora_bench --repeat 3
#include "scanner.hpp"
#include "lora_capture.hpp"

#include <kissfft/kiss_fft.h>
#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace rfmon;

namespace {
class ReplayRadio : public CaptureDevice {
public:
    const std::vector<std::complex<float>>* iq = nullptr;
    CaptureResult capture_detailed(double, double rate, double, double, bool) override {
        CaptureResult r;
        r.samples = *iq;
        r.sample_rate_hz = rate;
        r.timing.device_time_valid = true;
        r.timing.device_time_ns = 5000000000;
        r.timing.host_before_ns = 1700000000000000000;
        r.timing.host_after_ns = r.timing.host_before_ns + 100000;
        r.timing.requested_samples = iq->size();
        return r;
    }
    void set_gain(std::optional<double>) override {}
};
double cpu_seconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + 1e-6 * double(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
}
long peak_rss_mb() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss / 1024;
}
}  // namespace

namespace rfmon {
struct ScannerTestAccess {
    static void attach(Scanner& s, std::unique_ptr<CaptureDevice> d) {
        s.sdr_ = std::move(d);
        s.radio_session_ = 1;
        s.lora_security_->start();
    }
    static Scanner::LoraCaptured capture(Scanner& s, const lora_plan::Capture& p) {
        return s.capture_lora(p, device_profile(SdrDeviceType::X310));
    }
    static void process(Scanner& s, Scanner::LoraCaptured& c) {
        std::vector<Detection> d;
        s.process_lora(c, device_profile(SdrDeviceType::X310), DEFAULT_DETECTION_THRESHOLD_DB, d);
    }
    static void stop(Scanner& s) { s.lora_security_->stop(); }
};
}  // namespace rfmon

int main(int argc, char** argv) {
    std::vector<double> channels = {865.0625e6, 865.4025e6, 865.985e6, 866.9e6};
    int repeat = 3, packets = 8;
    double seconds = 6.0, snr_db = 20.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--channels") {
            std::string err;
            channels = lora_plan::parse_channels_mhz(next(), &err);
            if (channels.empty()) { std::fprintf(stderr, "bad --channels: %s\n", err.c_str()); return 2; }
        } else if (a == "--repeat") repeat = std::stoi(next());
        else if (a == "--packets") packets = std::stoi(next());
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--snr") snr_db = std::stod(next());
        else { std::fprintf(stderr, "usage: lora_bench [--channels MHz,..] [--repeat N] [--packets N] [--seconds S] [--snr dB]\n"); return 2; }
    }
    char tmpl[] = "/tmp/rfmon-lora-bench-XXXXXX";
    if (!mkdtemp(tmpl)) return 1;
    const std::string root = tmpl;
    int status = 0;
    try {
        Scanner scanner(root);
        scanner.set_lora_capture_seconds(seconds);
        scanner.set_lora_channels(channels);
        const auto rotation = scanner.lora_listen_plan();
        if (rotation.size() != 1 || !rotation[0].wideband) throw std::runtime_error("plan is not one wideband capture");
        const auto plan = rotation[0];
        const double rate = plan.rate_hz;
        const size_t total = size_t(seconds * rate);

        // Recorded packet (500 kHz) upsampled by FFT zero-padding.
        const auto fixture =
            load_lora_capture(std::string(PROJECT_ROOT_DIR) + "/tests/fixtures/lora_sx1262/capture-1790061256825496286-0");
        const size_t n = fixture.iq.size() & ~size_t(1), up = size_t(std::llround(rate / fixture.sample_rate_hz)), big = n * up;
        std::vector<kiss_fft_cpx> a(n), A(n);
        for (size_t i = 0; i < n; ++i) a[i] = {fixture.iq[i].real(), fixture.iq[i].imag()};
        kiss_fft_cfg f1 = kiss_fft_alloc(int(n), 0, nullptr, nullptr), f2 = kiss_fft_alloc(int(big), 1, nullptr, nullptr);
        kiss_fft(f1, a.data(), A.data());
        double packet_power = 0;
        for (auto& v : fixture.iq) packet_power += std::norm(v);
        packet_power /= double(fixture.iq.size());

        // Noise sized for the requested SNR in 125 kHz, then packets added.
        std::vector<std::complex<float>> iq(total);
        std::mt19937 rng(7);
        const double noise_in_band = packet_power / std::pow(10.0, snr_db / 10);
        const double sigma = std::sqrt(noise_in_band * (rate / 125e3) / 2);
        std::normal_distribution<float> g(0.f, float(sigma));
        for (auto& v : iq) v = {g(rng), g(rng)};
        std::vector<kiss_fft_cpx> B(big), b(big);
        for (int p = 0; p < packets; ++p) {
            const double ch = channels[size_t(p) % channels.size()];
            const long shift = std::lround((ch - plan.center_hz) / (rate / double(big)));
            std::fill(B.begin(), B.end(), kiss_fft_cpx{0, 0});
            for (long k = -long(n / 2); k < long(n / 2); ++k) {
                const auto& v = A[size_t((k + long(n)) % long(n))];
                B[size_t(((k + shift) % long(big) + long(big)) % long(big))] = {v.r / float(n), v.i / float(n)};
            }
            kiss_fft(f2, B.data(), b.data());
            const size_t at = size_t(double(total - big) * (p + 0.5) / packets);
            for (size_t i = 0; i < big && at + i < total; ++i) iq[at + i] += std::complex<float>(b[i].r, b[i].i);
        }
        kiss_fft_free(f1);
        kiss_fft_free(f2);

        auto radio = std::make_unique<ReplayRadio>();
        radio->iq = &iq;
        ScannerTestAccess::attach(scanner, std::move(radio));
        std::printf("plan: %zu channels in one %.0f MHz capture @ %.4f MHz, %.1f s, %d packets, threads available %u\n",
                    channels.size(), rate / 1e6, plan.center_hz / 1e6, seconds, packets, std::thread::hardware_concurrency());
        std::printf("baseline RSS after building the capture: %ld MB\n", peak_rss_mb());
        for (int r = 0; r < repeat; ++r) {
            auto captured = ScannerTestAccess::capture(scanner, plan);
            const double cpu0 = cpu_seconds();
            const auto w0 = std::chrono::steady_clock::now();
            ScannerTestAccess::process(scanner, captured);
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
            const double cpu = cpu_seconds() - cpu0;
            const auto st = scanner.lora_listen_stats();
            size_t valid = 0;
            for (const auto& row : scanner.lora_packets()) valid += row.crc == LoraPacketRow::Crc::Valid;
            std::printf("run %d: process %.2f s wall, %.2f s CPU (%.1f cores busy) | channelize %.2f energy %.2f analyze %.2f "
                        "commit %.2f | peak RSS %ld MB | CRC-valid rows so far %zu | real-time margin %.2fx\n",
                        r + 1, wall, cpu, cpu / wall, st.last_channelize_s, st.last_energy_s, st.last_analyze_s,
                        st.last_commit_s, peak_rss_mb(), valid, seconds / wall);
        }
        ScannerTestAccess::stop(scanner);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lora_bench: %s\n", e.what());
        status = 1;
    }
    fs::remove_all(root);
    return status;
}
