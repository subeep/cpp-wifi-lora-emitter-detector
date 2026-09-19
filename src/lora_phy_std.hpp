// Standards-compliant (SX1272/76-family) LoRa PHY codec - a second,
// separate implementation alongside lora_phy.hpp's self-consistent one.
//
// lora_phy.hpp's codec was validated as internally self-consistent
// (round-trips with itself over real B210 TX/RX loopback and HackRF -
// see TARANGMINI_LORA_FINDINGS.md) but was never claimed to match
// Semtech's exact undisclosed tables. Comparing it line-by-line against
// the canonical open-source reference implementation
// (https://github.com/myriadrf/LoRa-SDR, LoRaCodes.hpp/LoRaEncoder.cpp/
// LoRaDecoder.cpp - itself the widely-cited origin for gr-lora and
// other LoRa SDR projects) turned up real differences in nearly every
// layer, not just the header checksum:
//   - Header checksum: a specific 5-bit XOR formula over length/CR
//     bits (headerChecksum() below), not our invented
//     length^cr_byte^sync_word.
//   - Header framing: exactly 5 codewords (length hi/lo, one combined
//     CR+CRC-on nibble, checksum hi/lo), not 6 zero-padded ones - and
//     the *rest* of that first interleaved block (codewords 5..SF-1)
//     is the START OF THE PAYLOAD, forced to CR=4/8 for that one block
//     only, before the payload's own configured CR takes over from the
//     second block onward.
//   - Diagonal interleaver: extracts bits LSB-first per codeword, not
//     MSB-first.
//   - Hamming FEC: a different parity-check formula per code rate
//     ("non-standard version used in sx1272," per the reference's own
//     comment) - not equivalent to ours even after accounting for bit
//     order.
//   - Payload CRC: CCITT-16 (poly 0x1021) further masked by an 8-bit
//     LFSR sequence - not a plain CRC-16.
//   - Whitening: a specific hardcoded 510-bit sequence table, not our
//     self-derived LFSR.
//
// This module reimplements all of the above matching the reference
// bit-for-bit, reusing lora_phy.hpp's generic (vendor-agnostic) chirp
// generation, dechirping, and preamble detection - those parts are
// just the physical CSS math and don't depend on any of the above.

#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

namespace rfmon::lora::std_phy {

struct StdParams {
    int sf = 7;
    int cr = 1;              // payload coding rate: 1=4/5 .. 4=4/8 (header is always 4/8)
    bool crc_on = true;
    double bandwidth_hz = 125e3;
    // Low Data Rate Optimization: reduces the usable symbol alphabet
    // from 2^sf to 2^(sf-2) (the standard's fixed 2-bit reduction, not
    // a tunable amount here), trading a small capacity loss for timing
    // margin against clock drift over one symbol's now-much-longer
    // duration. The LoRa Alliance regional parameters mandate this ON
    // whenever symbol duration exceeds 16ms - true for SF11/SF12 at
    // 125kHz and SF12 at 250kHz, the exact regime a real transmitter
    // at this project's default SF12/BW125 falls into. See
    // lora_phy_std.cpp's modulate()/demodulate() for exactly how this
    // changes the symbol-to-bit mapping (transcribed from LoRaEncoder.cpp/
    // LoRaDecoder.cpp in https://github.com/myriadrf/LoRa-SDR, which -
    // unlike the rest of this file - never modeled LDRO at all; this
    // one piece is added from that reference project's OWN source
    // rather than copied from code already in this codebase).
    bool ldro = false;
};

struct StdDecodedPacket {
    int sf = 0;
    int cr = 0;
    bool crc_on = false;
    std::vector<uint8_t> payload;
    bool crc_valid = false;
    bool payload_complete = false;
    int declared_payload_len = 0;
    bool header_valid = false;
    long start_sample = 0;
    int cfo_bins = 0;
    // Which LDRO hypothesis this specific result came from - demodulate()
    // tries ldro=false first (unchanged legacy behavior) and only falls
    // back to ldro=true if that one's header doesn't validate, so this
    // is INFERRED per-attempt provenance, not a signaled/known fact
    // about the transmitter (the TarangNet API doesn't expose an LDRO
    // setting - see data/lora_m2/.../transmitter.txt). Meaningless
    // (left false) when header_valid is false and no ldro=true attempt
    // ran at all (sf < 7 - see demodulate()'s own comment).
    bool ldro = false;
    // True if this decode ran with the sync-word value check bypassed
    // (either via the skip_sync_check argument to demodulate() or the
    // LORA_STD_SKIP_SYNC_CHECK env var) - a header_valid=true result
    // with this set has NOT been confirmed against the sync word at
    // all, only its own checksum. See demodulate()'s own comment for
    // why this exists and what it does and doesn't prove.
    bool sync_check_skipped = false;
};

// iq must already be at sample_rate == bandwidth_hz (same convention
// as lora_phy.hpp's demodulate()).
std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& payload,
                                           const StdParams& params);
// skip_sync_check: bypasses the recovered_sync != SYNC_WORD_DEFAULT gate
// (see file header for why that gate currently rejects real TarangNet
// captures - SYNC_WORD_DEFAULT was only ever validated against this
// project's own TX, not independently against third-party hardware).
// Diagnostic only, off by default: a header_valid=true result obtained
// this way is verified by its own checksum but NOT cross-checked
// against the sync word, so it's weaker evidence than normal. See
// StdDecodedPacket::sync_check_skipped for how a caller can tell.
// Equivalent in effect to (and independent of) the LORA_STD_SKIP_SYNC_CHECK
// env var already used for this in offline tooling - either bypasses it.
std::optional<StdDecodedPacket> demodulate(const std::vector<std::complex<float>>& iq, int sf,
                                            bool skip_sync_check = false);

// Exploratory decode for the implicit-header-mode hypothesis: TarangNet
// is a proprietary network stack (addressing/PAN/routing, per the
// TarangNet API doc) layered on raw LoRa PHY, and never exposes a
// coding-rate setting - only SF+BW. That's consistent with implicit
// header mode, where there is no explicit 5-codeword header block at
// all (SF/CR/CRC-presence are fixed out of band instead) and the
// standard explicit-header decode in demodulate() above would never
// validate against it, which matches what we've observed.
//
// This assumes no header, decodes num_blocks worth of raw payload
// blocks for every coding-rate guess (1=4/5 .. 4=4/8), and hands back
// the FEC-decoded + de-whitened bytes for each guess unvalidated (no
// length/CRC is known yet) - the caller inspects the output for
// anything recognizable (known payload bytes, a plausible address).
struct ImplicitAttempt {
    int cr = 0;
    std::vector<uint8_t> raw_bytes;
};
std::optional<std::vector<ImplicitAttempt>> demodulate_implicit(
    const std::vector<std::complex<float>>& iq, int sf, int num_blocks = 10);

}  // namespace rfmon::lora::std_phy
