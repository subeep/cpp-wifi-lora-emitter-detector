#include "lora_phy_std.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <kissfft/kiss_fft.h>

#include "lora_phy.hpp"  // SYNC_WORD_DEFAULT - the sync word itself was already
                          // confirmed against real hardware independently of the
                          // header mystery; reused as-is, not part of this rewrite.

namespace rfmon::lora::std_phy {

namespace {

// Same helper as lora_phy.cpp - see that file for why this matters
// (Python-style non-negative modulo, needed for signed CFO handling).
inline int pymod(long long a, int n) {
    long long r = a % n;
    if (r < 0) r += n;
    return static_cast<int>(r);
}

// --- Chirp generation / dechirping - duplicated from lora_phy.cpp
// rather than shared, deliberately: this is stable, already-validated,
// vendor-agnostic CSS math (identical for any LoRa-compatible
// transmitter), and duplicating ~100 lines here is a much lower-risk
// choice than refactoring the existing, real-hardware-validated
// lora_phy.cpp to share internals while also introducing this new,
// separate codec. ---

std::vector<std::complex<float>> base_upchirp(int sf) {
    int N = 1 << sf;
    std::vector<std::complex<float>> out(N);
    for (int n = 0; n < N; ++n) {
        double phase = 2.0 * M_PI * (double(n) * n / (2.0 * N) - n / 2.0);
        out[n] = std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
    }
    return out;
}

std::vector<std::complex<float>> symbol_chirp(int sf, int symbol,
                                               const std::vector<std::complex<float>>& base_up) {
    int N = 1 << sf;
    std::vector<std::complex<float>> out(N);
    for (int n = 0; n < N; ++n) {
        double phase = 2.0 * M_PI * double(symbol) * n / N;
        std::complex<float> shift(float(std::cos(phase)), float(std::sin(phase)));
        out[n] = base_up[n] * shift;
    }
    return out;
}

struct DechirpResult {
    int peak_idx;
    double peak_mag;
    double sum_mag;
};

DechirpResult dechirp_analyze(const std::complex<float>* seg, int N,
                               const std::complex<float>* reference, kiss_fft_cfg cfg,
                               std::vector<kiss_fft_cpx>& in, std::vector<kiss_fft_cpx>& out) {
    double sum_mag = 0.0;
    for (int i = 0; i < N; ++i) {
        std::complex<float> v = seg[i] * reference[i];
        in[i].r = v.real();
        in[i].i = v.imag();
        sum_mag += std::sqrt(double(v.real()) * v.real() + double(v.imag()) * v.imag());
    }
    kiss_fft(cfg, in.data(), out.data());
    int best = 0;
    double best_mag = -1.0;
    for (int i = 0; i < N; ++i) {
        double mag = std::sqrt(double(out[i].r) * out[i].r + double(out[i].i) * out[i].i);
        if (mag > best_mag) {
            best_mag = mag;
            best = i;
        }
    }
    return {best, best_mag, sum_mag};
}

int binary_to_gray(int n) { return n ^ (n >> 1); }

int gray_to_binary(int g) {
    int b = g;
    int mask = g >> 1;
    while (mask) {
        b ^= mask;
        mask >>= 1;
    }
    return b;
}

std::pair<int, int> find_preamble(const std::complex<float>* iq, size_t n_iq, int sf,
                                   const std::vector<std::complex<float>>& base_down,
                                   int min_run = 6) {
    int N = 1 << sf;
    int num_windows = static_cast<int>(n_iq / N);
    std::vector<int> bins(num_windows);
    std::vector<double> ratios(num_windows);

    kiss_fft_cfg cfg = kiss_fft_alloc(N, 0, nullptr, nullptr);
    std::vector<kiss_fft_cpx> in(N), out(N);
    for (int i = 0; i < num_windows; ++i) {
        DechirpResult r = dechirp_analyze(iq + size_t(i) * N, N, base_down.data(), cfg, in, out);
        bins[i] = r.peak_idx;
        ratios[i] = r.peak_mag / (r.sum_mag + 1e-12);
    }
    kiss_fft_free(cfg);

    for (int i = 0; i + min_run <= num_windows; ++i) {
        bool consistent = true;
        int first = bins[i];
        for (int k = 0; k < min_run; ++k) {
            if (bins[i + k] != first || ratios[i + k] <= 0.5) {
                consistent = false;
                break;
            }
        }
        if (consistent) return {i, first};
    }
    return {-1, 0};
}

// Circular distance between two bins on a mod-N wheel (the short way
// round either direction) - needed because bin indices wrap at N and a
// naive |a-b| would misjudge a drift crossing the 0/N boundary.
int circular_bin_distance(int a, int b, int n) {
    int d = pymod(a - b, n);
    return std::min(d, n - d);
}

// Changed from an exact-bin match to step-limited ADAPTIVE tracking on
// 2026-09-19 after real-hardware evidence (see the M2 session trace/
// evidence files): this transmitter's actual preamble is far longer
// than what an exact match against a single fixed-integer CFO estimate
// can track, because a small residual (sub-bin) CFO accumulates phase
// across the preamble and slowly drifts the measured bin by roughly 1
// bin per ~8-9 symbols - real captures showed drift from bin 0 out to
// -4 over ~30 symbols, still clearly "preamble" (ratio still high),
// before jumping by hundreds/thousands of bins into what the trace
// shows is consistent, repeatable non-preamble content across
// independent captures of the same payload. An exact match terminates
// after only 8-11 symbols (as soon as the drift first crosses an
// integer bin).
//
// A single WIDENED fixed-tolerance version of this fix was tried first
// and reverted: it broke test_lora_phy_std's SF5/SF6/SF7 synthetic
// cases (Section 4's regression gate), because the sync word's own
// bin shift is `nibble << (sf-4)` - only 2-4 bins from zero at low SF,
// well inside a tolerance sized for this SF12 capture's ~4-bin drift.
// A fixed bin-count tolerance is therefore unsafe in general: whether
// it wrongly swallows the real sync symbols depends on SF and the sync
// word's own value, not just on how much a given transmitter's CFO
// drifts.
//
// This version instead tracks the target adaptively, symbol to symbol,
// and only requires each STEP to be small (PREAMBLE_DRIFT_STEP_BINS) -
// this follows an arbitrarily large cumulative drift correctly (as
// observed), while a genuine transition to sync-word content is a
// single large jump in one step regardless of SF, so it gets rejected
// the same way an exact match always correctly rejected it.
//
// Set to 1, not 2: at SF5 (the lowest SF this codebase tests), the sync
// word's own first symbol lands at bin `sync_hi << (sf-4)` = `1 << 1` =
// 2 for the default sync word - a step tolerance of 2 swallowed that
// real sync symbol as "still drifting" (confirmed: broke
// test_lora_phy_std's sf5_basic/sf5_cr4 synthetic regression cases,
// which have zero real CFO drift and so only needed step 0 to keep
// passing). 1 leaves those synthetic cases their full margin while
// still tracking this real capture's observed ~1-bin-per-8-9-symbol
// drift rate. A sync word whose own nibble is 0 (bin shift 0) would
// still be indistinguishable from drift at ANY tolerance - a known
// residual gap, not present in the current test suite or in this
// transmitter's observed default sync word.
//
// Still an assumption about how fast real CFO drift can move bin-to-
// bin, not a value derived from the standard - exactly the kind of
// thing EXECUTE_NEXT.md Section 9.2 flags needing independent
// verification once more captures/other SF/BW modes exist.
constexpr int PREAMBLE_DRIFT_STEP_BINS = 1;

int measure_preamble_length(const std::complex<float>* iq, size_t n_iq, int sf,
                             const std::vector<std::complex<float>>& base_down, int start_idx,
                             int cfo_bins) {
    int N = 1 << sf;
    kiss_fft_cfg cfg = kiss_fft_alloc(N, 0, nullptr, nullptr);
    std::vector<kiss_fft_cpx> in(N), out(N);
    long pos = long(start_idx) * N;
    int count = 0;
    int target = pymod(cfo_bins, N);
    while (size_t(pos) + N <= n_iq) {
        DechirpResult r = dechirp_analyze(iq + pos, N, base_down.data(), cfg, in, out);
        double ratio = r.peak_mag / (r.sum_mag + 1e-12);
        if (circular_bin_distance(r.peak_idx, target, N) <= PREAMBLE_DRIFT_STEP_BINS &&
            ratio > 0.5) {
            target = r.peak_idx;  // adapt to the slow drift, not a fixed reference
            ++count;
            pos += N;
        } else {
            break;
        }
    }
    kiss_fft_free(cfg);
    return count;
}

// --- Standards-compliant building blocks, transcribed directly from
// https://github.com/myriadrf/LoRa-SDR (LoRaCodes.hpp) - see
// lora_phy_std.hpp's file header for what differs from lora_phy.cpp
// and why. Kept as close to the original variable names/structure as
// possible to make cross-checking against the source easy. ---

constexpr int HEADER_RDD = 4;
constexpr int N_HEADER_SYMBOLS = HEADER_RDD + 4;  // 8
constexpr int N_HEADER_CODEWORDS = 5;

// CR=4 (8/4 Hamming) - used for the header always, and optionally for
// payload.
uint8_t encode_hamming84_sx(uint8_t x) {
    int d0 = (x >> 0) & 1, d1 = (x >> 1) & 1, d2 = (x >> 2) & 1, d3 = (x >> 3) & 1;
    uint8_t b = x & 0xf;
    b |= (d0 ^ d1 ^ d2) << 4;
    b |= (d1 ^ d2 ^ d3) << 5;
    b |= (d0 ^ d1 ^ d3) << 6;
    b |= (d0 ^ d2 ^ d3) << 7;
    return b;
}

// CR=3 (7/4 Hamming).
uint8_t encode_hamming74_sx(uint8_t x) {
    int d0 = (x >> 0) & 1, d1 = (x >> 1) & 1, d2 = (x >> 2) & 1, d3 = (x >> 3) & 1;
    uint8_t b = x & 0xf;
    b |= (d0 ^ d1 ^ d2) << 4;
    b |= (d1 ^ d2 ^ d3) << 5;
    b |= (d0 ^ d1 ^ d3) << 6;
    return b;
}

// CR=2 (6/4 parity).
uint8_t encode_parity64_sx(uint8_t b) {
    int x = (b ^ (b >> 1) ^ (b >> 2)) & 1;
    int y = (x ^ b ^ (b >> 3)) & 1;
    return uint8_t(((x & 1) << 4) | ((y & 1) << 5) | (b & 0xf));
}

// CR=1 (5/4 parity).
uint8_t encode_parity54_sx(uint8_t b) {
    int x = b ^ (b >> 2);
    x = x ^ (x >> 1);
    return uint8_t((b & 0xf) | ((x << 4) & 0x10));
}

uint8_t encode_fec_nibble_sx(uint8_t nibble, int cr) {
    switch (cr) {
        case 0: return nibble & 0xf;
        case 1: return encode_parity54_sx(nibble);
        case 2: return encode_parity64_sx(nibble);
        case 3: return encode_hamming74_sx(nibble);
        case 4: return encode_hamming84_sx(nibble);
        default: return nibble & 0xf;
    }
}

// Maximum-likelihood decode over all 16 candidate nibbles, same brute-
// force approach as lora_phy.cpp's hamming_decode_nibble() (simpler and
// more robust than transcribing the reference's separate syndrome-
// table decoders for each code rate, and just as correct as long as
// the encode function above is transcribed correctly).
uint8_t decode_fec_codeword_sx(uint8_t codeword, int cr) {
    int bits = 4 + cr;
    uint8_t best_nibble = 0;
    int best_dist = bits + 1;
    for (int candidate = 0; candidate < 16; ++candidate) {
        uint8_t trial = encode_fec_nibble_sx(uint8_t(candidate), cr);
        int dist = std::bitset<8>((trial ^ codeword) & ((1 << bits) - 1)).count();
        if (dist < best_dist) {
            best_dist = dist;
            best_nibble = uint8_t(candidate);
        }
    }
    return best_nibble;
}

uint8_t header_checksum_sx(const uint8_t* h) {
    int a0 = (h[0] >> 4) & 1, a1 = (h[0] >> 5) & 1, a2 = (h[0] >> 6) & 1, a3 = (h[0] >> 7) & 1;
    int b0 = (h[0] >> 0) & 1, b1 = (h[0] >> 1) & 1, b2 = (h[0] >> 2) & 1, b3 = (h[0] >> 3) & 1;
    int c0 = (h[1] >> 0) & 1, c1 = (h[1] >> 1) & 1, c2 = (h[1] >> 2) & 1, c3 = (h[1] >> 3) & 1;

    uint8_t res = (a0 ^ a1 ^ a2 ^ a3) << 4;
    res |= (a3 ^ b1 ^ b2 ^ b3 ^ c0) << 3;
    res |= (a2 ^ b0 ^ b3 ^ c1 ^ c3) << 2;
    res |= (a1 ^ b0 ^ b2 ^ c0 ^ c1 ^ c2) << 1;
    res |= a0 ^ b1 ^ c0 ^ c1 ^ c2 ^ c3;
    return res;
}

// Diagonal interleaver/deinterleaver - LSB-first bit extraction, PPM-
// modulo diagonal shift (opposite bit order from lora_phy.cpp's
// interleave_block/deinterleave_block, which extracts MSB-first and
// wraps modulo bits_per_symbol instead).
void diagonal_interleave_sx(const std::vector<uint8_t>& codewords, size_t num_codewords,
                             std::vector<int>& symbols, size_t ppm, int rdd) {
    int bits_per_symbol = 4 + rdd;
    for (size_t x = 0; x < num_codewords / ppm; ++x) {
        size_t cw_off = x * ppm;
        size_t sym_off = x * bits_per_symbol;
        for (int k = 0; k < bits_per_symbol; ++k) {
            for (size_t m = 0; m < ppm; ++m) {
                size_t i = (m + k) % ppm;
                int bit = (codewords[cw_off + i] >> k) & 1;
                symbols[sym_off + k] |= (bit << m);
            }
        }
    }
}

void diagonal_deinterleave_sx(const std::vector<int>& symbols, size_t num_symbols,
                               std::vector<uint8_t>& codewords, size_t ppm, int rdd) {
    int bits_per_symbol = 4 + rdd;
    for (size_t x = 0; x < num_symbols / size_t(bits_per_symbol); ++x) {
        size_t cw_off = x * ppm;
        size_t sym_off = x * bits_per_symbol;
        for (int k = 0; k < bits_per_symbol; ++k) {
            for (size_t m = 0; m < ppm; ++m) {
                size_t i = (m + k) % ppm;
                int bit = (symbols[sym_off + k] >> m) & 1;
                codewords[cw_off + i] |= uint8_t(bit << k);
            }
        }
    }
}

// Whitening - reverse-engineered sequence table, transcribed verbatim
// (opaque magic constants, not independently derivable - copied
// exactly from LoRaCodes.hpp rather than re-typed by hand piecemeal).
void whiten_sx(uint8_t* buffer, size_t buffer_size, int bit_ofs, int rdd) {
    static const int ofs0[8] = {6, 4, 2, 0, -112, -114, -302, -34};
    static const int ofs1[5] = {6, 4, 2, 0, -360};
    static const int whiten_len = 510;
    static const uint64_t whiten_seq[8] = {
        0x0102291EA751AAFFULL, 0xD24B050A8D643A17ULL, 0x5B279B671120B8F4ULL, 0x032B37B9F6FB55A2ULL,
        0x994E0F87E95E2D16ULL, 0x7CBCFC7631984C26ULL, 0x281C8E4F0DAEF7F9ULL, 0x1741886EB7733B15ULL};
    const int* ofs = (rdd == 1) ? ofs1 : ofs0;
    for (size_t j = 0; j < buffer_size; ++j) {
        uint8_t x = 0;
        for (int i = 0; i < 4 + rdd; ++i) {
            int t = (ofs[i] + int(j) + bit_ofs + whiten_len) % whiten_len;
            if (whiten_seq[t >> 6] & (uint64_t(1) << (t & 0x3F))) x |= uint8_t(1 << i);
        }
        buffer[j] ^= x;
    }
}

// Payload CRC-16 - CCITT (poly 0x1021) further masked by an 8-bit LFSR
// sequence (reverse-engineered from the real over-the-air data
// stream), NOT a plain CRC-16. Transcribed verbatim.
uint16_t crc16sx(uint16_t crc, uint16_t poly) {
    for (int i = 0; i < 8; ++i) {
        if (crc & 0x8000) crc = uint16_t((crc << 1) ^ poly);
        else crc = uint16_t(crc << 1);
    }
    return crc;
}

uint8_t xsum8(uint8_t t) {
    t ^= t >> 4;
    t ^= t >> 2;
    t ^= t >> 1;
    return t & 1;
}

uint16_t sx1272_data_checksum(const uint8_t* data, int length) {
    uint16_t res = 0;
    uint8_t v = 0xff;
    for (int i = 0; i < length; ++i) {
        uint16_t crc = crc16sx(res, 0x1021);
        v = uint8_t(xsum8(v & 0xB8) | (v << 1));
        res = uint16_t(crc ^ data[i]);
    }
    res ^= v;
    v = uint8_t(xsum8(v & 0xB8) | (v << 1));
    res ^= uint16_t(v << 8);
    return res;
}

int roundUp(int num, int factor) { return ((num + factor - 1) / factor) * factor; }

}  // namespace

std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& payload,
                                           const StdParams& params) {
    int sf = params.sf;
    int N = 1 << sf;
    // LDRO reduces the usable symbol alphabet from sf to sf-2 bits -
    // see StdParams::ldro's own comment for why and when. Everything
    // below that already threads `ppm` generically (interleaving,
    // block-size math) adapts automatically; the one piece that does
    // NOT fall out for free is the final symbol-value shift applied
    // just before chirping, added below per LoRaEncoder.cpp's own
    // "gray decode, when SF > PPM, pad out LSBs" comment.
    size_t ppm = size_t(params.ldro ? sf - 2 : sf);
    auto base_up = base_upchirp(sf);
    std::vector<std::complex<float>> base_down(base_up.size());
    for (size_t i = 0; i < base_up.size(); ++i) base_down[i] = std::conj(base_up[i]);

    // Preamble/sync/SFD framing reused as-is from lora_phy.hpp's
    // modulate() convention - already confirmed correct against real
    // TarangMini hardware independently of the header mystery (see
    // lora_phy.hpp's SYNC_WORD_DEFAULT comment).
    std::vector<std::complex<float>> result;
    auto append = [&](const std::vector<std::complex<float>>& seg) {
        result.insert(result.end(), seg.begin(), seg.end());
    };
    for (int i = 0; i < 8; ++i) append(symbol_chirp(sf, 0, base_up));
    int sync_hi = (SYNC_WORD_DEFAULT >> 4) << (sf - 4);
    int sync_lo = (SYNC_WORD_DEFAULT & 0xF) << (sf - 4);
    append(symbol_chirp(sf, sync_hi, base_up));
    append(symbol_chirp(sf, sync_lo, base_up));
    append(base_down);
    append(base_down);
    result.insert(result.end(), base_down.begin(), base_down.begin() + N / 4);

    // Header + payload, real SX1272-style framing (see file header).
    std::vector<uint8_t> bytes = payload;
    if (params.crc_on) {
        uint16_t crc = sx1272_data_checksum(payload.data(), int(payload.size()));
        bytes.push_back(uint8_t(crc & 0xff));
        bytes.push_back(uint8_t((crc >> 8) & 0xff));
    }

    uint8_t hdr[3];
    hdr[0] = uint8_t(payload.size() & 0xff);
    hdr[1] = uint8_t((params.crc_on ? 1 : 0) | (params.cr << 1));
    hdr[2] = header_checksum_sx(hdr);

    int numCodewords = roundUp(int(bytes.size()) * 2 + N_HEADER_CODEWORDS, int(ppm));
    int numSymbols =
        N_HEADER_SYMBOLS + (numCodewords / int(ppm) - 1) * (4 + params.cr);

    std::vector<uint8_t> codewords(numCodewords, 0);
    size_t cOfs = 0, dOfs = 0;
    codewords[cOfs++] = encode_hamming84_sx(hdr[0] >> 4);
    codewords[cOfs++] = encode_hamming84_sx(hdr[0] & 0xf);
    codewords[cOfs++] = encode_hamming84_sx(hdr[1] & 0xf);
    codewords[cOfs++] = encode_hamming84_sx(hdr[2] >> 4);
    codewords[cOfs++] = encode_hamming84_sx(hdr[2] & 0xf);

    auto encode_fec_run = [&](int rdd, size_t count) {
        for (size_t i = 0; i < count; ++i, ++dOfs) {
            uint8_t byte = bytes[dOfs >> 1];
            uint8_t nibble = (dOfs & 1) ? (byte >> 4) : (byte & 0xf);
            codewords[cOfs++] = encode_fec_nibble_sx(nibble, rdd);
        }
    };

    size_t cOfs1 = cOfs;
    encode_fec_run(4, ppm - cOfs);  // rest of block 1 is payload, forced CR=4/8
    whiten_sx(codewords.data() + cOfs1, ppm - cOfs1, 0, HEADER_RDD);

    if (size_t(numCodewords) > ppm) {
        size_t cOfs2 = cOfs;
        encode_fec_run(params.cr, size_t(numCodewords) - ppm);
        whiten_sx(codewords.data() + cOfs2, size_t(numCodewords) - cOfs2, int(ppm - cOfs1),
                  params.cr);
    }

    std::vector<int> symbols(numSymbols, 0);
    std::vector<uint8_t> block1(codewords.begin(), codewords.begin() + long(ppm));
    diagonal_interleave_sx(block1, ppm, symbols, ppm, HEADER_RDD);
    if (size_t(numCodewords) > ppm) {
        std::vector<uint8_t> block2(codewords.begin() + long(ppm), codewords.end());
        std::vector<int> symbols2(numSymbols - N_HEADER_SYMBOLS, 0);
        diagonal_interleave_sx(block2, size_t(numCodewords) - ppm, symbols2, ppm, params.cr);
        std::copy(symbols2.begin(), symbols2.end(), symbols.begin() + N_HEADER_SYMBOLS);
    }

    // NOTE: the reference encoder applies grayToBinary16 here (not
    // binaryToGray16, despite that being the more "intuitive" naming) -
    // confirmed by cross-checking LoRaEncoder.cpp (applies
    // grayToBinary16 before transmitting) against LoRaDecoder.cpp
    // (applies binaryToGray16 after dechirping, the exact inverse
    // pairing). Getting this backwards is a symmetric bug an
    // encode/decode self-test can't catch (gray_to_binary and
    // binary_to_gray are each other's true inverse either way you pair
    // them), which is exactly what happened here initially - only real
    // TarangMini captures caught it, since our own encoder+decoder
    // round-tripped fine with the wrong pairing too.
    //
    // LDRO shift: when ppm < sf (reduced alphabet), the gray-coded
    // value only occupies the LOW ppm bits of meaning - it has to be
    // left-shifted into the HIGH bits of the full sf-bit symbol before
    // chirping, exactly matching LoRaEncoder.cpp's own
    // `sym <<= (_sf - PPM)` (applied there right after grayToBinary16,
    // same order as here). A no-op (shift 0) when ldro is off, so this
    // changes nothing for every already-validated non-LDRO case.
    int ldro_shift = sf - int(ppm);
    for (int sym : symbols) append(symbol_chirp(sf, gray_to_binary(sym) << ldro_shift, base_up));
    return result;
}

// Not declared in lora_phy_std.hpp - internal to this file, called by
// the public demodulate() below once per LDRO hypothesis. `ppm` is the
// caller's choice (sf for LDRO off, sf-2 for LDRO on); everything here
// that already threaded `ppm` generically (interleaving, block-size
// math) adapts automatically. The two symbol-reading loops get one new
// piece each: the rounding+shift LoRaDecoder.cpp calls "gray encode,
// when SF > PPM, depad the LSBs with rounding" - the exact inverse of
// modulate()'s own shift, applied before gray-decoding rather than
// after. A no-op when ppm==sf (shift 0), so this changes nothing for
// every already-validated non-LDRO real-hardware case.
std::optional<StdDecodedPacket> demodulate_with_ppm(const std::vector<std::complex<float>>& iq,
                                                      int sf, size_t ppm, bool skip_sync_check) {
    int N = 1 << sf;
    int ldro_shift = sf - int(ppm);
    bool ldro = ldro_shift > 0;
    auto round_and_shift = [&](int b) {
        if (ldro_shift <= 0) return b;
        b += (1 << ldro_shift) / 2;
        b >>= ldro_shift;
        return b;
    };
    auto base_up = base_upchirp(sf);
    std::vector<std::complex<float>> base_down(base_up.size());
    for (size_t i = 0; i < base_up.size(); ++i) base_down[i] = std::conj(base_up[i]);

    auto [start_idx, cfo_raw] = find_preamble(iq.data(), iq.size(), sf, base_down);
    if (start_idx < 0) return std::nullopt;
    int cfo_bins = cfo_raw < N / 2 ? cfo_raw : cfo_raw - N;
    int preamble_len = measure_preamble_length(iq.data(), iq.size(), sf, base_down, start_idx, cfo_bins);

    long pos = long(start_idx) * N + long(preamble_len) * N;
    kiss_fft_cfg cfg = kiss_fft_alloc(N, 0, nullptr, nullptr);
    std::vector<kiss_fft_cpx> in(N), out(N);
    auto cleanup = [&]() { kiss_fft_free(cfg); };

    // Bounded, env-gated raw-bin trace covering well past any plausible
    // preamble length (up to 40 symbols from start_idx) - to see
    // directly where the true preamble ends, since measure_preamble_length()
    // is suspected of terminating early/inconsistently (see the M2
    // session notes: measured length varies 8-11 symbols run to run on
    // nominally identical real packets, yet the two symbols read
    // immediately after it consistently dechirp to ~bin 0 - i.e. still
    // plain-chirp/preamble-shaped - every time).
    if (const char* dbg = std::getenv("LORA_STD_TRACE")) {
        (void)dbg;
        std::fprintf(stderr, "[std trace] start_idx=%d cfo_bins=%d preamble_len(measured)=%d bins:",
                     start_idx, cfo_bins, preamble_len);
        long trace_pos = long(start_idx) * N;
        for (int k = 0; k < 80 && size_t(trace_pos) + N <= iq.size(); ++k, trace_pos += N) {
            DechirpResult r = dechirp_analyze(iq.data() + trace_pos, N, base_down.data(), cfg, in, out);
            int b = pymod(r.peak_idx - cfo_bins, N);
            std::fprintf(stderr, " %d", b);
        }
        std::fprintf(stderr, "\n");
    }
    // Dual-reference trace over the transition region specifically
    // (preamble_len-3 .. preamble_len+14): dechirps each symbol against
    // BOTH the upchirp (base_up, correct reference for a downchirp/SFD
    // symbol) and downchirp (base_down, correct for an upchirp/preamble/
    // sync symbol) reference, printing bin+ratio for both, so the two
    // can be told apart directly instead of assumed. Added 2026-09-19
    // after the preamble-length fix left `sync_bins_raw` landing on
    // small (4,12)/(5,13)-type values inconsistent with the expected
    // `nibble << (sf-4)` sync-word encoding - this checks whether that
    // region is genuinely upchirp-shaped (a sync symbol at an unexpected
    // value) or actually downchirp-shaped (meaning the true sync
    // symbols are somewhere else and this is already SFD).
    if (const char* dbg = std::getenv("LORA_STD_TRACE2")) {
        (void)dbg;
        std::fprintf(stderr, "[std trace2] start_idx=%d preamble_len=%d region (idx: down_bin/ratio up_bin/ratio):\n",
                     start_idx, preamble_len);
        int lo = std::max(0, preamble_len - 3);
        for (int k = lo; k < preamble_len + 15; ++k) {
            long p = long(start_idx) * N + long(k) * N;
            if (size_t(p) + N > iq.size()) break;
            DechirpResult rd = dechirp_analyze(iq.data() + p, N, base_down.data(), cfg, in, out);
            DechirpResult ru = dechirp_analyze(iq.data() + p, N, base_up.data(), cfg, in, out);
            int bd = pymod(rd.peak_idx - cfo_bins, N);
            int bu = pymod(ru.peak_idx - cfo_bins, N);
            double rd_ratio = rd.peak_mag / (rd.sum_mag + 1e-12);
            double ru_ratio = ru.peak_mag / (ru.sum_mag + 1e-12);
            std::fprintf(stderr, "  [%2d] down: %4d/%.2f   up: %4d/%.2f\n", k, bd, rd_ratio, bu,
                         ru_ratio);
        }
    }
    if (size_t(pos) + 2 * N > iq.size()) {
        cleanup();
        return std::nullopt;
    }
    int sync_bins[2];
    for (int k = 0; k < 2; ++k) {
        DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_down.data(), cfg, in, out);
        sync_bins[k] = pymod(r.peak_idx - cfo_bins, N);
        pos += N;
    }
    int recovered_sync = ((sync_bins[0] >> (sf - 4)) << 4) | (sync_bins[1] >> (sf - 4));
    if (const char* dbg = std::getenv("LORA_STD_DEBUG")) {
        (void)dbg;
        std::fprintf(stderr,
                     "[std debug] preamble start_idx=%d cfo_bins=%d preamble_len=%d | "
                     "sync_bins_raw=%d,%d recovered_sync=0x%02x expected=0x%02x\n",
                     start_idx, cfo_bins, preamble_len, sync_bins[0], sync_bins[1], recovered_sync,
                     SYNC_WORD_DEFAULT);
    }
    // Diagnostic-only bypass (either the skip_sync_check argument - now
    // exposed as a GUI toggle, see lora_gui.cpp - or the
    // LORA_STD_SKIP_SYNC_CHECK env var used by offline tooling; NOT a
    // behavior change when both are off/unset): lets the rest of the
    // chain (SFD/header/payload) run even when the sync word doesn't
    // match, to test whether positional alignment past this point is
    // otherwise correct. Added 2026-09-19 to separate "sync symbols are
    // in the right place but decode to an unexpected value" from
    // "something after this is still misaligned too" - see the M2
    // session notes for why those are now believed to be different
    // questions, still unresolved as of this bypass's addition.
    bool sync_gate_bypassed = skip_sync_check || std::getenv("LORA_STD_SKIP_SYNC_CHECK") != nullptr;
    if (!sync_gate_bypassed && recovered_sync != SYNC_WORD_DEFAULT) {
        cleanup();
        return std::nullopt;
    }

    long sfd_len = long(2.25 * N);
    if (size_t(pos) + N > iq.size() || size_t(pos) + sfd_len > iq.size()) {
        cleanup();
        return std::nullopt;
    }
    {
        DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_up.data(), cfg, in, out);
        double ratio = r.peak_mag / (r.sum_mag + 1e-12);
        if (const char* dbg = std::getenv("LORA_STD_DEBUG")) {
            (void)dbg;
            std::fprintf(stderr, "[std debug] SFD ratio=%.3f (need >=0.5)\n", ratio);
        }
        if (ratio < 0.5) {
            cleanup();
            return std::nullopt;
        }
    }
    pos += sfd_len;

    // Block 1: N_HEADER_SYMBOLS (8) symbols -> ppm codewords, first 5
    // are the header, the rest (if ppm > 5) are the start of the
    // payload at forced CR=4/8.
    if (size_t(pos) + size_t(N_HEADER_SYMBOLS) * N > iq.size()) {
        cleanup();
        return std::nullopt;
    }
    std::vector<int> block1_symbols(N_HEADER_SYMBOLS);
    bool dbg = std::getenv("LORA_STD_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[std debug] raw_bins:");
    for (int k = 0; k < N_HEADER_SYMBOLS; ++k) {
        DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_down.data(), cfg, in, out);
        int b = pymod(r.peak_idx - cfo_bins, N);
        b = round_and_shift(b);
        if (dbg) std::fprintf(stderr, " %3d(gray=%3d,ratio=%.2f)", b, binary_to_gray(b),
                             r.peak_mag / (r.sum_mag + 1e-12));
        block1_symbols[k] = binary_to_gray(b);  // see modulate()'s comment on gray direction
        pos += N;
    }
    if (dbg) std::fprintf(stderr, "\n");
    std::vector<uint8_t> block1_codewords(ppm, 0);
    diagonal_deinterleave_sx(block1_symbols, size_t(N_HEADER_SYMBOLS), block1_codewords, ppm,
                              HEADER_RDD);

    uint8_t hdr[3];
    hdr[0] = uint8_t((decode_fec_codeword_sx(block1_codewords[0], HEADER_RDD) << 4) |
                      decode_fec_codeword_sx(block1_codewords[1], HEADER_RDD));
    uint8_t cr_crc_nibble = decode_fec_codeword_sx(block1_codewords[2], HEADER_RDD);
    hdr[1] = cr_crc_nibble;
    hdr[2] = uint8_t((decode_fec_codeword_sx(block1_codewords[3], HEADER_RDD) << 4) |
                      decode_fec_codeword_sx(block1_codewords[4], HEADER_RDD));

    bool header_valid = header_checksum_sx(hdr) == hdr[2];
    if (const char* dbg = std::getenv("LORA_STD_DEBUG")) {
        (void)dbg;
        std::fprintf(stderr,
                     "[std debug] cw0..4=%02x %02x %02x %02x %02x  hdr=%02x %02x %02x  "
                     "computed_checksum=%02x  valid=%d\n",
                     block1_codewords[0], block1_codewords[1], block1_codewords[2],
                     block1_codewords[3], block1_codewords[4], hdr[0], hdr[1], hdr[2],
                     header_checksum_sx(hdr), header_valid);
    }
    int payload_len = hdr[0];
    int cr = (hdr[1] >> 1) & 0x7;
    bool crc_on = (hdr[1] & 0x1) != 0;
    if (!header_valid || !(cr >= 1 && cr <= 4) || payload_len > 250) {
        cleanup();
        StdDecodedPacket result;
        result.sf = sf;
        result.start_sample = long(start_idx) * N;
        result.cfo_bins = cfo_bins;
        result.header_valid = false;
        result.ldro = ldro;
        result.sync_check_skipped = sync_gate_bypassed;
        return result;  // caller can still see header_valid=false + raw fields if useful
    }

    StdDecodedPacket partial;
    partial.sf = sf;
    partial.cr = cr;
    partial.crc_on = crc_on;
    partial.header_valid = true;
    partial.declared_payload_len = payload_len;
    partial.start_sample = long(start_idx) * N;
    partial.cfo_bins = cfo_bins;
    partial.ldro = ldro;
    partial.sync_check_skipped = sync_gate_bypassed;

    int n_bytes_with_crc = payload_len + (crc_on ? 2 : 0);
    int n_nibbles = n_bytes_with_crc * 2;
    int block1_payload_nibbles = int(ppm) - N_HEADER_CODEWORDS;
    std::vector<uint8_t> data_codewords;

    // Decode block1's leftover payload nibbles (forced CR=4/8), then
    // however many more full ppm-row blocks are needed at the packet's
    // real CR.
    std::vector<uint8_t> whitened_cw;
    for (int i = N_HEADER_CODEWORDS; i < int(ppm); ++i) whitened_cw.push_back(block1_codewords[i]);
    whiten_sx(whitened_cw.data(), whitened_cw.size(), 0, HEADER_RDD);
    for (uint8_t cw : whitened_cw) data_codewords.push_back(decode_fec_codeword_sx(cw, HEADER_RDD));

    int remaining_nibbles = n_nibbles - int(data_codewords.size());
    int bits_per_symbol2 = 4 + cr;
    int remaining_codewords = std::max(0, remaining_nibbles);
    int more_blocks = remaining_codewords > 0 ? (remaining_codewords + int(ppm) - 1) / int(ppm) : 0;
    int more_symbols = more_blocks * bits_per_symbol2;

    if (more_symbols > 0) {
        if (size_t(pos) + size_t(more_symbols) * N > iq.size()) {
            cleanup();
            return partial;
        }
        std::vector<int> more_raw(more_symbols);
        for (int k = 0; k < more_symbols; ++k) {
            DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_down.data(), cfg, in, out);
            int b = pymod(r.peak_idx - cfo_bins, N);
            b = round_and_shift(b);
            more_raw[k] = binary_to_gray(b);  // see modulate()'s comment on gray direction
            pos += N;
        }
        std::vector<uint8_t> more_cw(size_t(more_blocks) * ppm, 0);
        diagonal_deinterleave_sx(more_raw, size_t(more_symbols), more_cw, ppm, cr);
        whiten_sx(more_cw.data(), more_cw.size(), int(block1_payload_nibbles), cr);
        for (uint8_t cw : more_cw) data_codewords.push_back(decode_fec_codeword_sx(cw, cr));
    }
    cleanup();

    if (int(data_codewords.size()) < n_nibbles) return partial;
    std::vector<uint8_t> data_bytes(n_bytes_with_crc, 0);
    for (int i = 0; i < n_nibbles; ++i) {
        if (i & 1) data_bytes[size_t(i >> 1)] |= uint8_t(data_codewords[size_t(i)] << 4);
        else data_bytes[size_t(i >> 1)] |= data_codewords[size_t(i)];
    }

    StdDecodedPacket result;
    result.sf = sf;
    result.cr = cr;
    result.crc_on = crc_on;
    result.start_sample = long(start_idx) * N;
    result.cfo_bins = cfo_bins;
    result.header_valid = true;
    result.payload_complete = true;
    result.ldro = ldro;
    result.sync_check_skipped = sync_gate_bypassed;
    result.declared_payload_len = payload_len;
    result.payload.assign(data_bytes.begin(), data_bytes.begin() + payload_len);
    if (crc_on) {
        uint16_t got_crc = uint16_t(data_bytes[size_t(payload_len)]) |
                            uint16_t(uint16_t(data_bytes[size_t(payload_len) + 1]) << 8);
        uint16_t want_crc = sx1272_data_checksum(result.payload.data(), payload_len);
        result.crc_valid = (got_crc == want_crc);
    }
    return result;
}

// Public entry point: tries LDRO off first (ppm=sf), which is the
// unchanged legacy behavior already validated against synthetic
// round-trips and real captures. Only falls back to LDRO on (ppm=sf-2)
// if that header doesn't validate - so a non-LDRO transmitter's packets
// decode exactly as before (same codepath, same result) and the extra
// attempt only ever fires on packets the old code already rejected.
// Guarded to sf>=7 because header_valid can only require ppm>=
// N_HEADER_CODEWORDS(5) codewords per row; sf-2 must stay above that
// floor for the diagonal interleaver's block1 sizing to make sense.
std::optional<StdDecodedPacket> demodulate(const std::vector<std::complex<float>>& iq, int sf,
                                            bool skip_sync_check) {
    auto result = demodulate_with_ppm(iq, sf, size_t(sf), skip_sync_check);
    if (result.has_value() && result->header_valid) return result;
    if (sf >= 7) {
        auto ldro_result = demodulate_with_ppm(iq, sf, size_t(sf - 2), skip_sync_check);
        if (ldro_result.has_value()) return ldro_result;
    }
    return result;
}

std::optional<std::vector<ImplicitAttempt>> demodulate_implicit(
    const std::vector<std::complex<float>>& iq, int sf, int num_blocks) {
    int N = 1 << sf;
    size_t ppm = size_t(sf);
    auto base_up = base_upchirp(sf);
    std::vector<std::complex<float>> base_down(base_up.size());
    for (size_t i = 0; i < base_up.size(); ++i) base_down[i] = std::conj(base_up[i]);

    // Preamble/sync/SFD detection duplicated from demodulate() rather
    // than factored out, same reasoning as the rest of this file:
    // don't touch or risk anything the real-hardware-validated path
    // depends on while adding this new, unvalidated one.
    auto [start_idx, cfo_raw] = find_preamble(iq.data(), iq.size(), sf, base_down);
    if (start_idx < 0) return std::nullopt;
    int cfo_bins = cfo_raw < N / 2 ? cfo_raw : cfo_raw - N;
    int preamble_len = measure_preamble_length(iq.data(), iq.size(), sf, base_down, start_idx, cfo_bins);

    long pos = long(start_idx) * N + long(preamble_len) * N;
    kiss_fft_cfg cfg = kiss_fft_alloc(N, 0, nullptr, nullptr);
    std::vector<kiss_fft_cpx> in(N), out(N);
    auto cleanup = [&]() { kiss_fft_free(cfg); };

    if (size_t(pos) + 2 * N > iq.size()) {
        cleanup();
        return std::nullopt;
    }
    int sync_bins[2];
    for (int k = 0; k < 2; ++k) {
        DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_down.data(), cfg, in, out);
        sync_bins[k] = pymod(r.peak_idx - cfo_bins, N);
        pos += N;
    }
    int recovered_sync = ((sync_bins[0] >> (sf - 4)) << 4) | (sync_bins[1] >> (sf - 4));
    if (recovered_sync != SYNC_WORD_DEFAULT) {
        cleanup();
        return std::nullopt;
    }

    long sfd_len = long(2.25 * N);
    if (size_t(pos) + N > iq.size() || size_t(pos) + sfd_len > iq.size()) {
        cleanup();
        return std::nullopt;
    }
    {
        DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_up.data(), cfg, in, out);
        double ratio = r.peak_mag / (r.sum_mag + 1e-12);
        if (ratio < 0.5) {
            cleanup();
            return std::nullopt;
        }
    }
    pos += sfd_len;

    // Symbol demodulation doesn't depend on the coding-rate guess, so
    // dechirp the max number of symbols any CR guess could need once,
    // then slice per guess below.
    int max_bits_per_symbol = 8;  // cr=4
    int max_symbols = num_blocks * max_bits_per_symbol;
    if (size_t(pos) + size_t(max_symbols) * N > iq.size()) {
        max_symbols = int((iq.size() - size_t(pos)) / size_t(N));
    }
    if (max_symbols <= 0) {
        cleanup();
        return std::nullopt;
    }
    size_t n_max_symbols = size_t(max_symbols);
    std::vector<int> raw_syms(n_max_symbols);
    for (int k = 0; k < max_symbols; ++k) {
        DechirpResult r = dechirp_analyze(iq.data() + pos, N, base_down.data(), cfg, in, out);
        int b = pymod(r.peak_idx - cfo_bins, N);
        raw_syms[size_t(k)] = binary_to_gray(b);  // see modulate()'s comment on gray direction
        pos += N;
    }
    cleanup();

    std::vector<ImplicitAttempt> results;
    for (int cr = 1; cr <= 4; ++cr) {
        int bits_per_symbol = 4 + cr;
        int n_sym = (max_symbols / bits_per_symbol) * bits_per_symbol;
        if (n_sym <= 0) continue;
        int blocks = n_sym / bits_per_symbol;
        std::vector<int> slice(raw_syms.begin(), raw_syms.begin() + n_sym);
        std::vector<uint8_t> cw(size_t(blocks) * ppm, 0);
        diagonal_deinterleave_sx(slice, size_t(n_sym), cw, ppm, cr);
        // Whitening starts fresh at offset 0 for the first payload
        // nibble in explicit-header mode too (see modulate()'s
        // whiten_sx calls) - with no header to skip in implicit mode,
        // the whole codeword stream is the payload from the start.
        whiten_sx(cw.data(), cw.size(), 0, cr);
        std::vector<uint8_t> nibbles;
        nibbles.reserve(cw.size());
        for (uint8_t c : cw) nibbles.push_back(decode_fec_codeword_sx(c, cr));
        std::vector<uint8_t> bytes((nibbles.size() + 1) / 2, 0);
        for (size_t i = 0; i < nibbles.size(); ++i) {
            if (i & 1) bytes[i >> 1] |= uint8_t(nibbles[i] << 4);
            else bytes[i >> 1] |= nibbles[i];
        }
        results.push_back(ImplicitAttempt{cr, std::move(bytes)});
    }
    if (results.empty()) return std::nullopt;
    return results;
}

}  // namespace rfmon::lora::std_phy
