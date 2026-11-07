// LoRa anti-alias decimator (src/lora_decimator.hpp).
#include "lora_decimator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace rfmon;

namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    failures += !ok;
}
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 500e3;

std::vector<std::complex<float>> tone(double hz, size_t n = 200000) {
    std::vector<std::complex<float>> x(n);
    for (size_t i = 0; i < n; ++i) {
        const double cycles = hz * double(i) / kRate;  // wrap in double: float phase loses precision
        x[i] = std::polar(1.0f, float(2 * kPi * (cycles - std::floor(cycles))));
    }
    return x;
}
// Output power (dB) of a unit tone after decimation, edges excluded.
double gain_db(double hz, int factor) {
    const auto y = lora_decimator::decimate(tone(hz), factor);
    double p = 0;
    size_t n = 0;
    for (size_t i = 100; i + 100 < y.size(); ++i, ++n) p += std::norm(y[i]);
    return 10 * std::log10(p / double(n));
}
std::string db(double v) { return std::to_string(v) + " dB"; }
}  // namespace

int main() {
    // BW125 from 500 kHz (factor 4): the live failure was an 866.9 MHz packet
    // decoded on 867.1 MHz, i.e. a signal at -200 kHz folding to +50 kHz.
    check(std::abs(gain_db(0, 4)) < 0.05, "bw125_dc_unity", db(gain_db(0, 4)));
    // In band the response is the former boxcar's (the decoder was tuned on it).
    for (double f : {20e3, 40e3, 62.5e3}) {
        const double boxcar = 20 * std::log10(std::abs(std::sin(4 * kPi * f / kRate) / (4 * std::sin(kPi * f / kRate))));
        check(std::abs(gain_db(f, 4) - boxcar) < 0.5, ("bw125_inband_matches_boxcar_" + std::to_string(int(f / 1e3)) + "k").c_str(),
              db(gain_db(f, 4)) + " vs boxcar " + db(boxcar));
    }
    for (double f : {-200e3, 200e3, 175e3, 250e3 - 1})
        check(gain_db(f, 4) < -60, ("bw125_rejects_" + std::to_string(int(f / 1e3)) + "k").c_str(), db(gain_db(f, 4)));
    check(gain_db(160e3, 4) < -45, "bw125_rejects_160k", db(gain_db(160e3, 4)));

    // BW250 from 500 kHz (factor 2): exactly the former boxcar of 2.
    const auto t2 = lora_decimator::taps(2);
    check(t2.size() == 26, "bw250_length", std::to_string(t2.size()));
    {
        double off = 0;
        for (size_t j = 0; j < t2.size(); ++j) off += std::abs(t2[j] - ((j == 12 || j == 13) ? 0.5f : 0.0f));
        check(off < 1e-5, "bw250_is_boxcar", std::to_string(off));
    }

    // Timing: output k represents input instant k*factor + (factor-1)/2, where
    // the old boxcar's average sat, so packet offsets do not move. A slow
    // tone's output phase must equal the input phase at that instant.
    for (int factor : {2, 3, 4}) {
        const double f0 = 1e3;
        const auto y = lora_decimator::decimate(tone(f0, 20000), factor);
        double worst = 0;
        for (size_t k = 100; k + 100 < y.size(); ++k) {
            const double t = double(k) * factor + 0.5 * (factor - 1);
            const double want = 2 * kPi * f0 * t / kRate;
            worst = std::max(worst, (double)std::abs(std::arg(y[k] * std::polar(1.0f, float(-std::fmod(want, 2 * kPi))))));
        }
        check(worst < 1e-3, ("timing_aligned_x" + std::to_string(factor)).c_str(), std::to_string(worst) + " rad");
        const auto h = lora_decimator::taps(factor);
        double sum = 0;
        for (float v : h) sum += v;
        check(std::abs(sum - 1) < 1e-5 && (h.size() % 2) == size_t(factor % 2), ("taps_x" + std::to_string(factor)).c_str(),
              std::to_string(h.size()));
    }
    check(lora_decimator::decimate(tone(10e3, 10), 1).size() == 10, "factor_one_passthrough");
    check(lora_decimator::decimate(tone(10e3, 10), 4).size() == 2, "output_length_floor");

    if (failures) {
        std::printf("%d decimator check(s) failed\n", failures);
        return 1;
    }
    std::printf("All LoRa decimator checks passed\n");
}
