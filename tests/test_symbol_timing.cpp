#include "drones/symbol_timing_monitor.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    monitor();
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
