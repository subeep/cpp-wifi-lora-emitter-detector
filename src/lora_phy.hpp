// LoRa chirp-spread-spectrum (CSS) PHY: modulator + demodulator.
//
// Direct C++ port of the validated Python prototype (rf_monitor/lora_phy.py)
// in the sibling newrocktest project. PHY-level only (no LoRaWAN MAC
// parsing) - see that module's docstring for the full scope/honesty
// note. Key facts ported over unchanged, confirmed against real
// hardware (a Melange TarangMini ST22LR01 module) in the Python
// prototype:
//   - SYNC_WORD_DEFAULT = 0x12 (LoRaWAN "private network" convention,
//     not the 0x34 "public" one) - confirmed via direct capture.
//   - Real preamble length varies by device (a real module used ~40
//     symbols, not the 8-symbol LoRaWAN-typical default) - demodulate()
//     measures it dynamically rather than assuming a constant.
//   - The header's exact Hamming/interleaver bit encoding does NOT yet
//     match real third-party hardware (only preamble/sync/SFD framing
//     does) - full payload decode of real devices whose header format
//     hasn't been reverse-engineered will fail; detect_burst() exists
//     specifically to still report those as "seen" without payload.

#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

namespace rfmon::lora {

constexpr int N_PREAMBLE = 8;             // our own TX default (modulate())
constexpr int SYNC_WORD_DEFAULT = 0x12;   // see file header note
constexpr int HEADER_CR = 4;              // header is always coded at the most robust rate (4/8)

struct LoRaParams {
    int sf = 7;
    int cr = 1;
    double bandwidth_hz = 125e3;
    int sync_word = SYNC_WORD_DEFAULT;
};

struct DecodedPacket {
    int sf = 0;
    int cr = 0;
    std::vector<uint8_t> payload;
    bool crc_valid = false;
    bool header_valid = false;
    long start_sample = 0;
    int cfo_bins = 0;
};

// A real chirp preamble was found and measured, but the rest of the
// frame didn't fully decode - see file header note.
struct BurstDetection {
    int sf = 0;
    long start_sample = 0;
    int cfo_bins = 0;
    int preamble_len = 0;
};

// Generates a complex64 IQ waveform at sample_rate == params.bandwidth_hz.
std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& payload,
                                           const LoRaParams& params);

// iq must already be at sample_rate == bandwidth_hz (e.g. capture the
// B210 directly at 125e3 sps - no resampling needed).
std::optional<DecodedPacket> demodulate(const std::vector<std::complex<float>>& iq, int sf);

// Lightweight, protocol-agnostic burst detector - see file header note.
std::optional<BurstDetection> detect_burst(const std::vector<std::complex<float>>& iq, int sf);

}  // namespace rfmon::lora
