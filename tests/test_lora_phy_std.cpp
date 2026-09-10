// Bit-exact round-trip test for the standards-compliant (SX1272/76-
// family) LoRa PHY codec (src/lora_phy_std.cpp) - the second codec
// added alongside lora_phy.cpp's self-consistent one specifically to
// decode real third-party hardware. Pure software - no hardware
// involved. See lora_phy_std.hpp's file header for what this differs
// from lora_phy.hpp and why.

#include <cstdio>
#include <string>
#include <vector>

#include "../src/lora_phy_std.hpp"

using namespace rfmon::lora::std_phy;

namespace {

int g_failures = 0;

std::vector<uint8_t> to_bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

bool run_case(const std::string& name, const std::vector<uint8_t>& payload, int sf, int cr) {
    StdParams params;
    params.sf = sf;
    params.cr = cr;
    params.crc_on = true;
    auto tx_iq = modulate(payload, params);

    auto result = demodulate(tx_iq, sf);
    if (!result.has_value()) {
        std::fprintf(stderr, "FAIL [%s]: demodulate() returned nullopt (no packet found)\n",
                     name.c_str());
        ++g_failures;
        return false;
    }
    bool ok = true;
    if (!result->header_valid) {
        std::fprintf(stderr, "FAIL [%s]: header invalid\n", name.c_str());
        ok = false;
    }
    if (result->cr != cr) {
        std::fprintf(stderr, "FAIL [%s]: cr mismatch got=%d want=%d\n", name.c_str(), result->cr,
                     cr);
        ok = false;
    }
    if (result->payload != payload) {
        std::fprintf(stderr, "FAIL [%s]: payload mismatch (got %zu bytes, want %zu bytes)\n",
                     name.c_str(), result->payload.size(), payload.size());
        ok = false;
    }
    if (!result->crc_valid) {
        std::fprintf(stderr, "FAIL [%s]: CRC invalid\n", name.c_str());
        ok = false;
    }
    if (ok) {
        std::printf("PASS [%s]: sf=%d cr=%d payload_len=%zu start_sample=%ld cfo_bins=%d\n",
                    name.c_str(), sf, cr, payload.size(), result->start_sample, result->cfo_bins);
    }
    return ok;
}

}  // namespace

int main() {
    bool all_ok = true;

    all_ok &= run_case("basic_sf7_cr1", to_bytes("hello lora"), 7, 1);
    all_ok &= run_case("basic_sf7_cr4", to_bytes("hello lora"), 7, 4);
    all_ok &= run_case("sf9_cr2", to_bytes("this is a twenty byte!"), 9, 2);
    all_ok &= run_case("sf12_cr3", std::vector<uint8_t>(40, 0xAA), 12, 3);
    all_ok &= run_case("empty_payload", {}, 7, 1);
    all_ok &= run_case("single_byte", {0x00}, 7, 1);

    // SF5/SF6: the same range this app added to lora_phy.cpp - the
    // first interleaved block here (header + start of payload) exactly
    // fills up at SF5 (ppm == N_HEADER_CODEWORDS == 5, no payload data
    // merged into block 1 at all), a good edge case to keep covered.
    all_ok &= run_case("sf6_basic", to_bytes("sf6 test"), 6, 1);
    all_ok &= run_case("sf5_basic", to_bytes("sf5"), 5, 1);
    all_ok &= run_case("sf5_cr4", to_bytes("abcdefghij"), 5, 4);

    if (all_ok && g_failures == 0) {
        std::printf("\nAll standards-compliant LoRa PHY round-trip checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\nSome standards-compliant LoRa PHY round-trip checks FAILED.\n");
    return 1;
}
