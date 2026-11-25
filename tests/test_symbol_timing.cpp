#include "drones/symbol_timing_monitor.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kiss_fft.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

using namespace rfmon::symbol_timing;
using Sample = std::complex<float>;
namespace fs = std::filesystem;

static void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

static int parity(const fs::path& root) {
    std::ifstream input(root / "data/drone_ml/2026-11-11/cpp_port_reference/reference.json");
    if (!input) return 77;
    const auto ref = nlohmann::json::parse(input);
    for (const auto& entry : ref.at("cases"))
        if (!fs::exists(root / entry.at("file").get<std::string>())) {
            std::cout << "Missing recorded IQ: " << entry.at("file") << '\n';
            return 77;
        }
    Probe probe;
    std::vector<Sample> buffer(block_size);
    for (auto it = ref.at("cases").begin(); it != ref.at("cases").end(); ++it) {
        const auto& entry = it.value();
        std::ifstream iq(root / entry.at("file").get<std::string>(), std::ios::binary);
        Fractions fractions{};
        const auto& blocks = entry.at("blocks");
        const auto total = fs::file_size(root / entry.at("file").get<std::string>()) / sizeof(Sample);
        for (const auto& block : blocks) {
            const auto start = block.at("start_sample").get<std::size_t>();
            require(start <= total - block_size, "invalid reference block offset");
            iq.seekg(start * sizeof(Sample));
            require(bool(iq.read(reinterpret_cast<char*>(buffer.data()), buffer.size() * sizeof(Sample))),
                    "short reference IQ read");
            Ratios actual;
            require(probe.measure(buffer.data(), ref.at("rate_hz"), actual), "invalid block");
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const auto name = hypotheses[i].name;
                if (!block.at("ratios").contains(name)) continue;  // added after the reference (v2)
                const double expected = block.at("ratios").at(name);
                require(std::abs(actual[i] - expected) <= std::max(.05, .02 * std::abs(expected)),
                        it.key() + " " + name + " ratio " + std::to_string(actual[i]) +
                        " expected " + std::to_string(expected));
                const bool flagged = actual[i] > flag_ratio;
                require(std::abs(expected - flag_ratio) <= .03 * flag_ratio ||
                        flagged == (expected > flag_ratio), "unexpected flag difference");
            }
        }
        // The JSON lists eight spot checks, but its capture fractions use all 38 blocks.
        const auto& capture = entry.at("capture");
        const auto count = capture.at("blocks").get<std::size_t>();
        fractions.fill(0);
        for (std::size_t i = 0; i < count; ++i) {
            iq.seekg(block_start(total, i, count) * sizeof(Sample));
            require(bool(iq.read(reinterpret_cast<char*>(buffer.data()), buffer.size() * sizeof(Sample))),
                    "short capture IQ read");
            Ratios ratios;
            require(probe.measure(buffer.data(), ref.at("rate_hz"), ratios), "invalid capture block");
            for (std::size_t h = 0; h < ratios.size(); ++h)
                fractions[h] += ratios[h] > flag_ratio;
        }
        for (std::size_t h = 0; h < fractions.size(); ++h) {
            fractions[h] /= count;
            if (!capture.at("flagged_fraction").contains(hypotheses[h].name)) continue;
            const double expected = capture.at("flagged_fraction").at(hypotheses[h].name);
            require(std::abs(fractions[h] - expected) < 1e-9, it.key() + " capture fraction mismatch");
        }
        const auto expected = capture.at("group");
        const auto matches = [&](const char* actual) {
            return expected.is_null() ? !actual : actual && expected == actual;
        };
        require(matches(group(fractions)), it.key() + " group mismatch");

        std::vector<Sample> recorded(total);
        iq.seekg(0);
        require(bool(iq.read(reinterpret_cast<char*>(recorded.data()), recorded.size() * sizeof(Sample))),
                "short monitor replay IQ read");
        Monitor worker;
        rfmon::wifi_security::CaptureRecord metadata;
        metadata.samples_received = total;
        metadata.capture_center_hz = 2452e6;
        metadata.sample_rate_hz = ref.at("rate_hz");
        metadata.run_id = it.key();
        worker.start();
        worker.observe(recorded, metadata);
        worker.stop();
        const auto state = worker.snapshot();
        require(state.history.size() == 1, it.key() + " missing replay result");
        const auto& result = state.history.front();
        require(result.blocks == max_blocks && matches(result.timing_group), it.key() + " live-budget mismatch");
        std::cout << it.key() << " " << (result.timing_group ? result.timing_group : "none")
                  << " | 24 blocks " << result.processing_ms << " ms | hand-off "
                  << state.max_handoff_ms << " ms\n";
    }
    return 0;
}

// Independent Gaussian time samples model a densely populated OFDM symbol.
// Copy its suffix to the prefix, then interpolate the fractional sample rates.
static std::vector<Sample> waveform(int useful, int prefix, double native_rate, float tone, bool link) {
    std::mt19937 rng(42);
    std::normal_distribution<float> normal;
    const auto noise = [&]{ return Sample{normal(rng), normal(rng)}; };
    std::vector<Sample> result(block_size), symbol(useful), native;
    const double step = native_rate / 20e6;
    while (native.size() <= result.size() * step + 1) {
        std::generate(symbol.begin(), symbol.end(), noise);
        native.insert(native.end(), symbol.end() - prefix, symbol.end());
        native.insert(native.end(), symbol.begin(), symbol.end());
    }
    for (std::size_t i = 0; i < result.size(); ++i) {
        const double position = i * step;
        const auto base = static_cast<std::size_t>(position);
        result[i] = (link ? native[base] + float(position-base)*(native[base+1]-native[base]) : Sample{})
                    + .1f * noise() + std::polar(tone, float(2 * std::acos(-1.) * .0137 * i));
    }
    return result;
}

static void synthetic() {
    Probe probe;
    const auto check = [&](int useful, int cp, double rate, int expected, float tone, bool link) {
        const auto iq = waveform(useful, cp, rate, tone, link);
        Ratios ratios;
        require(probe.measure(iq.data(), 20e6, ratios), "synthetic IQ rejected");
        for (std::size_t i = 0; i < ratios.size(); ++i)
            require((ratios[i] > flag_ratio) == (int(i) == expected),
                    std::string("synthetic unexpected flag: ") + hypotheses[i].name +
                    " ratio " + std::to_string(ratios[i]));
    };
    check(64, 16, 20e6, 0, 0, true);
    const auto at_25 = waveform(80, 20, 20e6, 0, true);
    Ratios at_25_ratios;
    require(probe.measure(at_25.data(), 25e6, at_25_ratios), "25 MSps rejected");
    for (std::size_t i = 0; i < at_25_ratios.size(); ++i)
        require((at_25_ratios[i] > flag_ratio) == (i == 0), "25 MSps Wi-Fi flag mismatch");
    check(2048, 144, 30.72e6, 2, 0, true);
    check(1024, 72, 30.72e6, 3, 0, true);
    check(1024, 72, 30.72e6, 3, 120, true);
    check(1024, 72, 30.72e6, -1, 120, false);
    check(1024, 72, 30.72e6, -1, 0, false);

    // Lightbridge: 4096-point symbols at 44.8 MSps with a 512-sample prefix (91.43 + 11.43 us).
    check(4096, 512, 44.8e6, int(lightbridge), 0, true);
    check(4096, 512, 44.8e6, int(lightbridge), 120, true);
    // Hard-clipped, as a close Phantom is at the app's gain: still flagged.
    auto clipped = waveform(4096, 512, 44.8e6, 0, true);
    for (auto& x : clipped) x = {x.real() > .3f ? .3f : x.real() < -.3f ? -.3f : x.real(),
                                 x.imag() > .3f ? .3f : x.imag() < -.3f ? -.3f : x.imag()};
    Ratios clipped_ratios;
    require(probe.measure(clipped.data(), 20e6, clipped_ratios), "clipped IQ rejected");
    require(clipped_ratios[lightbridge] > flag_ratio, "clipped Lightbridge not flagged");

    // Groups: either Lightbridge lag gives the Lightbridge group ("32.8kHz"), ahead of 15/30 kHz.
    Fractions f{};
    f[lightbridge] = .5;
    require(std::string(group(f)) == "32.8kHz", "10.9375 kHz alone is Lightbridge");
    f[lightbridge] = 0; f[lightbridge_third] = .3;
    require(std::string(group(f)) == "32.8kHz", "32.8 kHz alone is still Lightbridge");
    f[lightbridge_third] = .29; f[lightbridge] = .29; f[lte_15k] = .5;
    require(std::string(group(f)) == "15kHz", "below threshold Lightbridge does not take a 15 kHz capture");
    require(std::string(hypotheses[lightbridge].name) == "lightbridge_10.9375kHz" &&
                std::abs(1 / hypotheses[lightbridge].seconds - 10937.5) < 1e-6 && std::string(version) == "symbol-timing-v2",
            "Lightbridge hypothesis and constants version");
}

// Band-limited 15 kHz OFDM (600 subcarriers, 2048 FFT at 30.72 MSps, like DroneID / LTE 10 MHz),
// linearly interpolated to 20 MSps, then shifted by shift_hz at 20 MSps.
static std::vector<Sample> ofdm15(double shift_hz, std::size_t blocks, unsigned seed) {
    constexpr int n = 2048, cp = 144;
    std::mt19937 rng(seed);
    std::normal_distribution<float> normal;
    std::unique_ptr<kiss_fft_state, decltype(&std::free)> inverse{kiss_fft_alloc(n, 1, nullptr, nullptr), std::free};
    std::vector<kiss_fft_cpx> bins(n), time(n);
    const std::size_t total = blocks * block_size;
    const double step = 30.72e6 / 20e6;
    std::vector<Sample> native;
    while (native.size() <= total * step + 1) {
        for (int k = 0; k < n; ++k) {
            const int carrier = k < n / 2 ? k : k - n;
            const bool used = carrier != 0 && std::abs(carrier) <= 300;
            bins[k] = used ? kiss_fft_cpx{normal(rng), normal(rng)} : kiss_fft_cpx{0, 0};
        }
        kiss_fft(inverse.get(), bins.data(), time.data());
        for (int k = n - cp; k < n; ++k) native.emplace_back(time[k].r, time[k].i);
        for (int k = 0; k < n; ++k) native.emplace_back(time[k].r, time[k].i);
    }
    std::vector<Sample> result(total);
    const double pi = std::acos(-1.);
    for (std::size_t i = 0; i < total; ++i) {
        const double position = i * step;
        const auto base = static_cast<std::size_t>(position);
        const Sample x = native[base] + float(position - base) * (native[base + 1] - native[base]);
        result[i] = x * Sample(std::polar(1., 2 * pi * shift_hz * i / 20e6)) + .05f * Sample{normal(rng), normal(rng)};
    }
    return result;
}

// The grid offset puts the subcarrier grid at (centre + shift) modulo 15 kHz, wherever the link
// sits in the capture, including shifts of more than one spacing.
static void grid() {
    Probe probe;
    constexpr double centre = 2414.5e6, spacing = 15e3;
    for (const double shift : {0., 1234., -4321., 7400., 3000123., -2345678., 1e6 + 14990.}) {
        const auto iq = ofdm15(shift, 4, 7);
        std::complex<double> sum{};
        double magnitude = 0;
        std::size_t flagged = 0;
        for (std::size_t b = 0; b < 4; ++b) {
            Ratios ratios;
            Phases phases;
            require(probe.measure(iq.data() + b * block_size, 20e6, ratios, &phases), "OFDM rejected");
            require(ratios[2] > flag_ratio, "15 kHz OFDM not flagged");
            sum += phases[2]; magnitude += std::abs(phases[2]); ++flagged;
        }
        const auto offset = grid_offset(sum, magnitude, flagged, spacing, centre);
        require(offset.has_value(), "no grid offset");
        double expected = std::fmod(centre + shift, spacing);
        if (expected < 0) expected += spacing;
        double error = std::abs(offset->hz - expected);
        error = std::min(error, spacing - error);  // the grid position wraps at one spacing
        std::cout << "grid shift " << shift << " Hz: measured " << offset->hz << " Hz, expected "
                  << expected << " Hz, error " << error << " Hz, coherence " << offset->coherence << '\n';
        require(error < 100, "grid offset error " + std::to_string(error) + " Hz at shift " + std::to_string(shift));
        require(offset->coherence > .9, "grid coherence");
    }
    require(!grid_offset({}, 0, 0, spacing, centre), "grid offset without blocks");
    require(!grid_offset({1, 0}, 1, 1, spacing, std::numeric_limits<double>::quiet_NaN()), "grid offset with NaN centre");

    // Through the monitor: a 15 kHz result carries its grid, found by link_grid.
    Monitor worker;
    worker.start();
    const auto iq = ofdm15(2500., max_blocks, 9);
    rfmon::wifi_security::CaptureRecord capture;
    capture.samples_received = iq.size();
    capture.capture_center_hz = 5756.5e6;
    capture.sample_rate_hz = 20e6;
    capture.run_id = "grid";
    worker.observe(iq, capture);
    worker.stop();
    const auto state = worker.snapshot();
    require(state.completed == 1 && state.history.size() == 1, "monitor did not finish the OFDM capture");
    const auto& result = state.history.front();
    require(result.timing_group && std::string(result.timing_group) == "15kHz", "OFDM capture not grouped 15kHz");
    const auto link = link_grid(result);
    require(link.has_value() && link->blocks == max_blocks, "monitor grid missing");
    double expected = std::fmod(5756.5e6 + 2500., 15e3);
    double error = std::abs(link->hz - expected);
    require(std::min(error, 15e3 - error) < 100, "monitor grid offset error " + std::to_string(error));
    Result empty;
    require(!link_grid(empty), "grid without a timing group");

    // A Lightbridge capture through the monitor: grouped, its grid on the 10.9375 kHz spacing.
    Monitor lb;
    lb.start();
    auto lb_iq = waveform(4096, 512, 44.8e6, 0, true);
    const auto lb_block = lb_iq;
    for (std::size_t i = 1; i < max_blocks; ++i) lb_iq.insert(lb_iq.end(), lb_block.begin(), lb_block.end());
    capture.run_id = "lightbridge";
    capture.capture_center_hz = 2437.5e6;
    capture.samples_received = lb_iq.size();
    lb.observe(lb_iq, capture);
    lb.stop();
    const auto lb_state = lb.snapshot();
    require(lb_state.history.size() == 1, "monitor did not finish the Lightbridge capture");
    const auto& lb_result = lb_state.history.front();
    require(lb_result.timing_group && std::string(lb_result.timing_group) == "32.8kHz" &&
                lb_result.fractions[lightbridge] == 1,
            "Lightbridge capture not grouped");
    const auto lb_grid = link_grid(lb_result);
    require(lb_grid && std::abs(lb_grid->spacing_hz - 10937.5) < 1e-6, "Lightbridge grid not on 10.9375 kHz");
}

static void monitor() {
    Monitor worker;
    worker.start();
    auto iq = waveform(64, 16, 20e6, 0, true);
    const auto block = iq;
    for (std::size_t i = 1; i < max_blocks; ++i) iq.insert(iq.end(), block.begin(), block.end());
    rfmon::wifi_security::CaptureRecord capture;
    capture.samples_received = iq.size();
    capture.capture_center_hz = 2452e6;
    capture.sample_rate_hz = 20e6;
    capture.run_id = "synthetic";
    worker.observe({}, capture);
    for (int i = 0; i < 16; ++i) {
        capture.capture_seq = i;
        worker.observe(iq, capture);
    }
    worker.stop();
    const auto state = worker.snapshot();
    require(state.invalid == 1 && state.full > 0 && state.completed > 0 &&
            state.full + state.contention + state.completed == 16, "monitor drop accounting");
    std::set<uint64_t> ids;
    for (const auto& result : state.history)
        require(result.blocks == max_blocks && ids.insert(result.capture_seq).second, "missing blocks or duplicate result");
    std::cout << "Synthetic 24-block processing " << state.history.front().processing_ms
              << " ms; max hand-off " << state.max_handoff_ms << " ms\n";
    worker.start();
    iq[0] = {std::numeric_limits<float>::infinity(), 0};
    worker.observe(iq, capture);
    worker.stop();
    require(worker.snapshot().invalid == 2, "invalid IQ after restart");
}

int main(int argc, char** argv) try {
    if (argc == 2) return parity(argv[1]);
    synthetic();
    grid();
    monitor();
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
