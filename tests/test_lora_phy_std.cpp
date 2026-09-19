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

// Same as run_case(), but TXes with LDRO on and confirms demodulate()
// both round-trips correctly AND reports ldro=true provenance - i.e.
// that the ldro=false first attempt correctly fails (or produces a
// non-validating header) and the fallback ldro=true attempt is what
// actually recovers the packet, not a coincidental pass on the wrong
// hypothesis.
bool run_case_ldro(const std::string& name, const std::vector<uint8_t>& payload, int sf, int cr) {
    StdParams params;
    params.sf = sf;
    params.cr = cr;
    params.crc_on = true;
    params.ldro = true;
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
    if (!result->ldro) {
        std::fprintf(stderr, "FAIL [%s]: expected ldro=true provenance, got false\n", name.c_str());
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
        std::printf("PASS [%s]: sf=%d cr=%d payload_len=%zu ldro=%d start_sample=%ld cfo_bins=%d\n",
                    name.c_str(), sf, cr, payload.size(), result->ldro, result->start_sample,
                    result->cfo_bins);
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

    // LDRO on: the exact real-world regime (SF11/12@125kHz, SF12@250kHz)
    // the standard mandates this for. demodulate()'s ldro=false-then-
    // ldro=true fallback must land on the right hypothesis on its own -
    // callers never pass ldro in explicitly, only sf.
    all_ok &= run_case_ldro("sf12_ldro_cr1", std::vector<uint8_t>(40, 0x5A), 12, 1);
    all_ok &= run_case_ldro("sf12_ldro_cr4", to_bytes("low data rate optimization test"), 12, 4);
    all_ok &= run_case_ldro("sf11_ldro_cr2", to_bytes("sf11 ldro"), 11, 2);

    if (all_ok && g_failures == 0) {
        std::printf("\nAll standards-compliant LoRa PHY round-trip checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\nSome standards-compliant LoRa PHY round-trip checks FAILED.\n");
    return 1;
}
