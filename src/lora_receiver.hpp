#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfmon::lora::receiver {
// Receive-only commercial-format path. No vendor IDs, UART framing, payload
// strings or network sync-word whitelist participate in decoding.
struct Packet {
    int sf = 0, cr = 0, declared_payload_len = 0;
    bool header_valid = false, payload_complete = false, crc_on = false, crc_valid = false;
    bool ldro = false; // hypothesis, not a field signalled by the header
    bool ldro_ambiguous = false;
    long start_sample = 0, data_start_sample = 0, end_sample = 0;
    double cfo_bins = 0, drift_bins_per_symbol = 0;
    std::array<double, 2> sync_bins{};
    std::optional<int> sync_word; // quantized observation; not an identity/filter
    int fec_disagreements = 0;
    std::vector<uint8_t> payload;
    std::string detail;
};
struct Options {
    int min_preamble_symbols = 6;
    int max_preamble_symbols = 256;
    int max_packets = 32;
    double minimum_peak_ratio = 0.5;
};
// Independent symbol-layer entry point for published vectors. Symbols are
// compensated physical FFT-bin values, before Gray mapping, at sample rate BW.
Packet decode_symbols(const std::vector<double>& symbols, int sf, bool ldro);
// Input rate must equal bandwidth. Explicit headers, SF7..12. No sync bypass.
std::vector<Packet> demodulate(const std::vector<std::complex<float>>& iq,
                               int sf, double bandwidth_hz, const Options& options = {});
} // namespace rfmon::lora::receiver
