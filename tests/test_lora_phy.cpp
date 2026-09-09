// Bit-exact round-trip test for the LoRa PHY codec (src/lora_phy.cpp).
// Direct C++ port of the validated Python test (tests/test_lora_phy.py
// in the sibling newrocktest project) - same cases, same tolerances.
// Pure software - no hardware involved.

#include <cstdio>
#include <random>
#include <vector>

#include "../src/lora_phy.hpp"

using namespace rfmon::lora;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

std::vector<uint8_t> to_bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> random_bytes(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<uint8_t> out(n);
    for (auto& b : out) b = static_cast<uint8_t>(dist(rng));
    return out;
}

bool run_case(const std::string& name, const std::vector<uint8_t>& payload, int sf, int cr,
              double noise_amp = 0.0, int pad_before = 0, int pad_after = 0, unsigned seed = 0) {
    LoRaParams params;
    params.sf = sf;
    params.cr = cr;
    auto tx_iq = modulate(payload, params);

    std::mt19937 rng(seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    std::vector<std::complex<float>> rx_iq;
    rx_iq.reserve(pad_before + tx_iq.size() + pad_after);
    for (int i = 0; i < pad_before; ++i)
        rx_iq.emplace_back(float(normal(rng) * noise_amp), float(normal(rng) * noise_amp));
    for (auto& s : tx_iq) {
        std::complex<float> n(float(normal(rng) * noise_amp), float(normal(rng) * noise_amp));
        rx_iq.push_back(s + n);
    }
    for (int i = 0; i < pad_after; ++i)
        rx_iq.emplace_back(float(normal(rng) * noise_amp), float(normal(rng) * noise_amp));

    auto result = demodulate(rx_iq, sf);
    if (!result.has_value()) {
        std::fprintf(stderr, "FAIL [%s]: demodulate() returned nullopt (no packet found)\n",
                     name.c_str());
        ++g_failures;
        return false;
    }
    bool ok = true;
    if (result->payload != payload) {
        std::fprintf(stderr, "FAIL [%s]: payload mismatch (got %zu bytes, want %zu bytes)\n",
                     name.c_str(), result->payload.size(), payload.size());
        ok = false;
    }
    check(result->crc_valid, name + ": CRC invalid");
    check(result->header_valid, name + ": header invalid");
    check(result->cr == cr, name + ": cr mismatch");
    ok = ok && result->crc_valid && result->header_valid && result->cr == cr;
    if (ok) {
        std::printf("PASS [%s]: sf=%d cr=%d payload_len=%zu start_sample=%ld cfo_bins=%d\n",
                    name.c_str(), sf, cr, payload.size(), result->start_sample, result->cfo_bins);
    }
    return ok;
}

}  // namespace

int main() {
    bool all_ok = true;

    std::vector<uint8_t> hello = to_bytes("hello lora");
    all_ok &= run_case("basic_sf7_cr1", hello, 7, 1);
    all_ok &= run_case("basic_sf7_cr4", hello, 7, 4);
    all_ok &= run_case("sf9", random_bytes(20, 1), 9, 2);
    all_ok &= run_case("sf12_long_payload", random_bytes(40, 2), 12, 3);
    all_ok &= run_case("empty_payload", {}, 7, 1);
    all_ok &= run_case("single_byte", {0x00}, 7, 1);

    std::vector<uint8_t> padded = to_bytes("padded test");
    all_ok &= run_case("with_padding", padded, 8, 2, 0.0, 512, 512);

    std::vector<uint8_t> noisy = to_bytes("noisy channel");
    all_ok &= run_case("with_light_noise", noisy, 7, 4, 0.05);

    all_ok &= run_case("all_zero_bytes", std::vector<uint8_t>(16, 0x00), 7, 1);
    all_ok &= run_case("all_ff_bytes", std::vector<uint8_t>(16, 0xFF), 8, 4);

    // SF5/SF6: TarangNet's own Default Data Rate table (config cmd
    // 0x08) supports SF05 through SF12, but this codec only ever
    // ported SF7-12 - the header interleaver reuses `sf` as its row
    // count assuming that's always >= the header's fixed 6 codewords,
    // which is false at SF5 specifically (see lora_phy.cpp).
    all_ok &= run_case("sf6_basic", to_bytes("sf6 test"), 6, 1);
    all_ok &= run_case("sf5_basic", to_bytes("sf5 test"), 5, 1);
    all_ok &= run_case("sf5_cr4", random_bytes(12, 3), 5, 4);

    if (all_ok && g_failures == 0) {
        std::printf("\nAll LoRa PHY round-trip checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\nSome LoRa PHY round-trip checks FAILED.\n");
    return 1;
}
