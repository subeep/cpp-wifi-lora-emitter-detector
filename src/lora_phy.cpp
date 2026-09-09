#include "lora_phy.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstring>
#include <utility>

#include <kissfft/kiss_fft.h>

namespace rfmon::lora {

namespace {

// Python's `%` always returns a result with the sign of the divisor
// (e.g. -30 % 128 == 98); C++'s `%` truncates toward zero (-30 % 128 ==
// -30). CFO is a signed value subtracted/compared against bins
// throughout this file, so every modulo here MUST use this, not `%`
// directly, or negative CFOs silently wrap incorrectly.
inline int pymod(long long a, int n) {
    long long r = a % n;
    if (r < 0) r += n;
    return static_cast<int>(r);
}

// --- Chirp generation / dechirping (see file header: critically
// sampled, Fs == bandwidth_hz, N == 2**sf samples/symbol) ---

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

// One dechirp+FFT, computing everything downstream code needs: the
// peak bin (the recovered symbol/bin value) and the peak-to-sum-of-
// magnitude ratio (used by the preamble/SFD sanity checks - see
// find_preamble()'s comment for why this specific ratio, not a raw
// magnitude threshold, is what's empirically validated against real
// hardware).
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

// --- Gray coding (standard, textbook - not protocol-specific) ---

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

// --- Whitening: XOR with a deterministic PN sequence from a 9-bit
// LFSR (self-defined - see file header caveat). ---

std::vector<uint8_t> whitening_sequence(size_t n_bytes) {
    int state = 0x1FF;
    std::vector<uint8_t> out(n_bytes);
    for (size_t i = 0; i < n_bytes; ++i) {
        uint8_t byte = 0;
        for (int bit_idx = 0; bit_idx < 8; ++bit_idx) {
            int bit = state & 1;
            byte |= bit << bit_idx;
            int fb = ((state >> 0) ^ (state >> 5)) & 1;  // taps: x^9 + x^5 + 1
            state = (state >> 1) | (fb << 8);
        }
        out[i] = byte;
    }
    return out;
}

std::vector<uint8_t> whiten(const std::vector<uint8_t>& data) {
    auto pn = whitening_sequence(data.size());
    std::vector<uint8_t> out(data.size());
    for (size_t i = 0; i < data.size(); ++i) out[i] = data[i] ^ pn[i];
    return out;
}
// dewhiten == whiten: XOR is self-inverse with the same sequence.

// --- Hamming FEC: standard (4, 4+cr) systematic Hamming code per
// nibble. cr=1..4 -> codeword length 5..8 bits. ---

std::vector<int> hamming_parity_bits(int d0, int d1, int d2, int d3, int cr) {
    std::vector<int> p;
    if (cr >= 1) p.push_back(d0 ^ d1 ^ d2);
    if (cr >= 2) p.push_back(d1 ^ d2 ^ d3);
    if (cr >= 3) p.push_back(d0 ^ d1 ^ d3);
    if (cr >= 4) p.push_back(d0 ^ d1 ^ d2 ^ d3 ^ p[0] ^ p[1] ^ p[2]);
    return p;
}

int hamming_encode_nibble(int nibble, int cr) {
    int d[4] = {(nibble >> 3) & 1, (nibble >> 2) & 1, (nibble >> 1) & 1, nibble & 1};
    auto parity = hamming_parity_bits(d[0], d[1], d[2], d[3], cr);
    int val = 0;
    for (int i = 0; i < 4; ++i) val = (val << 1) | d[i];
    for (int p : parity) val = (val << 1) | p;
    return val;
}

// Maximum-likelihood decode: try all 16 possible nibbles, re-encode
// each, and pick whichever produces the codeword closest (Hamming
// distance) to what was received - see lora_phy.py's identical
// approach for why brute force over a 4-bit space beats a hand-derived
// syndrome table here.
int hamming_decode_nibble(int codeword, int cr) {
    int best_nibble = 0;
    int best_dist = (4 + cr) + 1;
    for (int candidate = 0; candidate < 16; ++candidate) {
        int trial_cw = hamming_encode_nibble(candidate, cr);
        int dist = std::bitset<8>(trial_cw ^ codeword).count();
        if (dist < best_dist) {
            best_dist = dist;
            best_nibble = candidate;
        }
    }
    return best_nibble;
}

// --- Diagonal interleaver: spreads each Hamming codeword's bits
// across `sf` chirp symbols so a single bad symbol doesn't corrupt one
// whole codeword. ---

std::vector<int> interleave_block(const std::vector<int>& codewords, int sf, int bits_per_symbol) {
    std::vector<std::vector<int>> matrix(codewords.size(), std::vector<int>(bits_per_symbol));
    for (size_t cw_i = 0; cw_i < codewords.size(); ++cw_i) {
        for (int col = 0; col < bits_per_symbol; ++col) {
            matrix[cw_i][col] = (codewords[cw_i] >> (bits_per_symbol - 1 - col)) & 1;
        }
    }
    std::vector<int> symbols(bits_per_symbol);
    for (int col = 0; col < bits_per_symbol; ++col) {
        int val = 0;
        for (int row = 0; row < sf; ++row) {
            int bit = matrix[row][(col + row) % bits_per_symbol];
            val = (val << 1) | bit;
        }
        symbols[col] = val;
    }
    return symbols;
}

std::vector<int> deinterleave_block(const std::vector<int>& symbols, int sf, int bits_per_symbol) {
    std::vector<std::vector<int>> col_bits(symbols.size(), std::vector<int>(sf));
    for (size_t s = 0; s < symbols.size(); ++s) {
        for (int row = 0; row < sf; ++row) {
            col_bits[s][row] = (symbols[s] >> (sf - 1 - row)) & 1;
        }
    }
    std::vector<std::vector<int>> matrix(sf, std::vector<int>(bits_per_symbol, 0));
    for (int col = 0; col < static_cast<int>(symbols.size()); ++col) {
        for (int row = 0; row < sf; ++row) {
            matrix[row][(col + row) % bits_per_symbol] = col_bits[col][row];
        }
    }
    std::vector<int> codewords(sf);
    for (int row = 0; row < sf; ++row) {
        int val = 0;
        for (int bit : matrix[row]) val = (val << 1) | bit;
        codewords[row] = val;
    }
    return codewords;
}

// --- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the payload. ---

uint16_t crc16_ccitt(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::vector<uint8_t> header_bytes(int payload_len, int cr, int sync_word) {
    uint8_t b0 = payload_len & 0xFF;
    uint8_t b1 = ((cr & 0x7) << 5) | 0x10;  // bit4 = crc_present, always set here
    uint8_t checksum = (b0 ^ b1 ^ sync_word) & 0xFF;
    return {b0, b1, checksum};
}

void append(std::vector<std::complex<float>>& dst, const std::vector<std::complex<float>>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// Finds the first run of `min_run` consecutive symbol-aligned windows
// that dechirp to the same bin AND clear a peak/sum ratio floor - see
// lora_phy.py's find_preamble() for the full empirical justification
// (the ratio check is required: dechirping silence gives a degenerate
// "consistent bin 0" false preamble without it; 0.5 is validated
// against real hardware, where noise topped out at ~0.31).
// Returns {start_idx, raw_bin} or {-1, 0} if not found.
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

// Counts how many consecutive symbols from the confirmed anchor still
// match the preamble's bin+ratio - the REAL preamble length, not an
// assumed constant. See lora_phy.py's identical function for why this
// matters (a real device used 40 symbols, not the 8 this module's own
// TX side defaults to).
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
        if (r.peak_idx == target && ratio > 0.5) {
            ++count;
            pos += N;
        } else {
            break;
        }
    }
    kiss_fft_free(cfg);
    return count;
}

// Attempt a full decode assuming the sync word starts exactly at `pos`.
std::optional<DecodedPacket> try_decode_from(const std::complex<float>* iq, size_t n_iq, int sf,
                                              long pos, int cfo_bins, int start_idx,
                                              const std::vector<std::complex<float>>& base_up,
                                              const std::vector<std::complex<float>>& base_down) {
    int N = 1 << sf;
    kiss_fft_cfg cfg = kiss_fft_alloc(N, 0, nullptr, nullptr);
    std::vector<kiss_fft_cpx> in(N), out(N);
    auto cleanup = [&]() { kiss_fft_free(cfg); };

    // 2 sync symbols.
    if (size_t(pos) + 2 * N > n_iq) {
        cleanup();
        return std::nullopt;
    }
    int sync_bins[2];
    for (int k = 0; k < 2; ++k) {
        DechirpResult r = dechirp_analyze(iq + pos, N, base_down.data(), cfg, in, out);
        sync_bins[k] = pymod(r.peak_idx - cfo_bins, N);
        pos += N;
    }

    // SFD: 2.25 downchirps - position follows structurally (see
    // lora_phy.py's try_decode_from() for the full rationale: peak-
    // magnitude can't discriminate alignment within the SFD itself, so
    // this is a coarse sanity check only, not used for positioning).
    long sfd_len = long(2.25 * N);
    if (size_t(pos) + N > n_iq || size_t(pos) + sfd_len > n_iq) {
        cleanup();
        return std::nullopt;
    }
    {
        DechirpResult r = dechirp_analyze(iq + pos, N, base_up.data(), cfg, in, out);
        double ratio = r.peak_mag / (r.sum_mag + 1e-12);
        if (ratio < 0.5) {
            cleanup();
            return std::nullopt;
        }
    }
    pos += sfd_len;

    // Header: one interleaved block of `sf` codewords (CR=4/8) is
    // transmitted as header_bits_per_symbol chirp symbols.
    int header_bits_per_symbol = 4 + HEADER_CR;
    if (size_t(pos) + size_t(header_bits_per_symbol) * N > n_iq) {
        cleanup();
        return std::nullopt;
    }
    std::vector<int> header_raw_symbols(header_bits_per_symbol);
    for (int k = 0; k < header_bits_per_symbol; ++k) {
        DechirpResult r = dechirp_analyze(iq + pos, N, base_down.data(), cfg, in, out);
        int b = pymod(r.peak_idx - cfo_bins, N);
        header_raw_symbols[k] = gray_to_binary(b);
        pos += N;
    }
    std::vector<int> header_codewords = deinterleave_block(header_raw_symbols, sf, header_bits_per_symbol);
    int header_nibbles[6];
    for (int i = 0; i < 6; ++i) header_nibbles[i] = hamming_decode_nibble(header_codewords[i], HEADER_CR);
    int b0 = (header_nibbles[0] << 4) | header_nibbles[1];
    int b1 = (header_nibbles[2] << 4) | header_nibbles[3];
    int b2 = (header_nibbles[4] << 4) | header_nibbles[5];

    int recovered_sync = ((sync_bins[0] >> (sf - 4)) << 4) | (sync_bins[1] >> (sf - 4));
    bool header_valid = ((b0 ^ b1 ^ recovered_sync) & 0xFF) == b2;
    int payload_len = b0;
    int cr = (b1 >> 5) & 0x7;
    if (!header_valid || !(cr >= 1 && cr <= 4) || payload_len > 250) {
        cleanup();
        return std::nullopt;
    }

    // Payload. Codewords grouped into blocks of `sf` (last block
    // zero-padded), each block transmitted as bits_per_symbol chirp
    // symbols - same interleaver mapping as the header.
    int n_bytes_with_crc = payload_len + 2;
    int n_nibbles = n_bytes_with_crc * 2;
    int bits_per_symbol = 4 + cr;
    int n_blocks = (n_nibbles + sf - 1) / sf;  // ceil
    int n_payload_symbols = n_blocks * bits_per_symbol;
    if (size_t(pos) + size_t(n_payload_symbols) * N > n_iq) {
        cleanup();
        return std::nullopt;
    }
    std::vector<int> payload_raw_symbols(n_payload_symbols);
    for (int k = 0; k < n_payload_symbols; ++k) {
        DechirpResult r = dechirp_analyze(iq + pos, N, base_down.data(), cfg, in, out);
        int b = pymod(r.peak_idx - cfo_bins, N);
        payload_raw_symbols[k] = gray_to_binary(b);
        pos += N;
    }
    cleanup();

    std::vector<int> codewords;
    for (int i = 0; i < n_payload_symbols; i += bits_per_symbol) {
        int block_len = std::min(bits_per_symbol, n_payload_symbols - i);
        std::vector<int> block(payload_raw_symbols.begin() + i,
                                payload_raw_symbols.begin() + i + block_len);
        auto cws = deinterleave_block(block, sf, bits_per_symbol);
        codewords.insert(codewords.end(), cws.begin(), cws.end());
    }
    std::vector<int> nibbles;
    for (int i = 0; i < n_nibbles && i < static_cast<int>(codewords.size()); ++i) {
        nibbles.push_back(hamming_decode_nibble(codewords[i], cr));
    }
    std::vector<uint8_t> raw_bytes;
    for (size_t i = 0; i + 1 < nibbles.size(); i += 2) {
        raw_bytes.push_back(static_cast<uint8_t>((nibbles[i] << 4) | nibbles[i + 1]));
    }
    std::vector<uint8_t> dewhitened = whiten(raw_bytes);  // dewhiten == whiten
    if (static_cast<int>(dewhitened.size()) < payload_len + 2) return std::nullopt;
    std::vector<uint8_t> payload(dewhitened.begin(), dewhitened.begin() + payload_len);
    uint16_t rx_crc = (uint16_t(dewhitened[payload_len]) << 8) | dewhitened[payload_len + 1];
    bool crc_valid = rx_crc == crc16_ccitt(payload);

    DecodedPacket result;
    result.sf = sf;
    result.cr = cr;
    result.payload = std::move(payload);
    result.crc_valid = crc_valid;
    result.header_valid = header_valid;
    result.start_sample = long(start_idx) * N;
    result.cfo_bins = cfo_bins;
    return result;
}

}  // namespace

std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& payload,
                                           const LoRaParams& params) {
    int sf = params.sf;
    int N = 1 << sf;
    auto base_up = base_upchirp(sf);
    auto base_down_vec = base_up;
    for (auto& v : base_down_vec) v = std::conj(v);

    auto symbols_to_iq = [&](const std::vector<int>& symbol_values) {
        std::vector<std::complex<float>> out(symbol_values.size() * size_t(N));
        for (size_t i = 0; i < symbol_values.size(); ++i) {
            auto chirp = symbol_chirp(sf, symbol_values[i], base_up);
            std::copy(chirp.begin(), chirp.end(), out.begin() + i * N);
        }
        return out;
    };

    // Preamble: N_PREAMBLE upchirps of symbol 0.
    std::vector<int> preamble_syms(N_PREAMBLE, 0);
    auto preamble_iq = symbols_to_iq(preamble_syms);

    // Sync: 2 symbols, each nibble of sync_word in the top 4 bits of an SF-bit symbol.
    int sync_hi = ((params.sync_word & 0xF0) >> 4) << (sf - 4);
    int sync_lo = (params.sync_word & 0x0F) << (sf - 4);
    auto sync_iq = symbols_to_iq({sync_hi, sync_lo});

    // SFD: 2.25 downchirps.
    std::vector<std::complex<float>> sfd_iq;
    sfd_iq.insert(sfd_iq.end(), base_down_vec.begin(), base_down_vec.end());
    sfd_iq.insert(sfd_iq.end(), base_down_vec.begin(), base_down_vec.end());
    sfd_iq.insert(sfd_iq.end(), base_down_vec.begin(), base_down_vec.begin() + N / 4);

    // Header (always CR=4/8, gray-coded, interleaved over `sf` symbols).
    auto hbytes = header_bytes(static_cast<int>(payload.size()), params.cr, params.sync_word);
    std::vector<int> header_symbols_raw;
    for (uint8_t byte : hbytes) {
        header_symbols_raw.push_back(hamming_encode_nibble((byte >> 4) & 0xF, HEADER_CR));
        header_symbols_raw.push_back(hamming_encode_nibble(byte & 0xF, HEADER_CR));
    }
    int header_bits_per_symbol = 4 + HEADER_CR;
    std::vector<int> header_block = header_symbols_raw;
    header_block.resize(sf, 0);
    auto header_interleaved = interleave_block(header_block, sf, header_bits_per_symbol);
    std::vector<int> header_symbols;
    for (int s : header_interleaved) header_symbols.push_back(binary_to_gray(s));
    auto header_iq = symbols_to_iq(header_symbols);

    // Payload: whitened, CRC-16 appended, Hamming-coded at params.cr, interleaved.
    uint16_t crc = crc16_ccitt(payload);
    std::vector<uint8_t> payload_with_crc = payload;
    payload_with_crc.push_back((crc >> 8) & 0xFF);
    payload_with_crc.push_back(crc & 0xFF);
    auto whitened = whiten(payload_with_crc);
    std::vector<int> codewords;
    for (uint8_t byte : whitened) {
        codewords.push_back(hamming_encode_nibble((byte >> 4) & 0xF, params.cr));
        codewords.push_back(hamming_encode_nibble(byte & 0xF, params.cr));
    }
    int bits_per_symbol = 4 + params.cr;
    std::vector<int> payload_symbols;
    for (size_t i = 0; i < codewords.size(); i += sf) {
        size_t block_len = std::min<size_t>(sf, codewords.size() - i);
        std::vector<int> block(codewords.begin() + i, codewords.begin() + i + block_len);
        block.resize(sf, 0);
        auto interleaved = interleave_block(block, sf, bits_per_symbol);
        for (int s : interleaved) payload_symbols.push_back(binary_to_gray(s));
    }
    auto payload_iq = symbols_to_iq(payload_symbols);

    std::vector<std::complex<float>> result;
    append(result, preamble_iq);
    append(result, sync_iq);
    append(result, sfd_iq);
    append(result, header_iq);
    append(result, payload_iq);
    return result;
}

std::optional<DecodedPacket> demodulate(const std::vector<std::complex<float>>& iq, int sf) {
    int N = 1 << sf;
    auto base_up = base_upchirp(sf);
    std::vector<std::complex<float>> base_down(base_up.size());
    for (size_t i = 0; i < base_up.size(); ++i) base_down[i] = std::conj(base_up[i]);

    auto [start_idx, cfo_raw] = find_preamble(iq.data(), iq.size(), sf, base_down);
    if (start_idx < 0) return std::nullopt;
    int cfo_bins = cfo_raw < N / 2 ? cfo_raw : cfo_raw - N;  // signed offset

    int preamble_len = measure_preamble_length(iq.data(), iq.size(), sf, base_down, start_idx, cfo_bins);
    long base_pos = long(start_idx) * N + long(preamble_len) * N;
    for (int offset_symbols : {0, 1, -1}) {
        long pos = base_pos + long(offset_symbols) * N;
        if (pos < 0) continue;
        auto result = try_decode_from(iq.data(), iq.size(), sf, pos, cfo_bins, start_idx, base_up, base_down);
        if (result.has_value() && result->header_valid) return result;
    }
    return std::nullopt;
}

std::optional<BurstDetection> detect_burst(const std::vector<std::complex<float>>& iq, int sf) {
    int N = 1 << sf;
    auto base_up = base_upchirp(sf);
    std::vector<std::complex<float>> base_down(base_up.size());
    for (size_t i = 0; i < base_up.size(); ++i) base_down[i] = std::conj(base_up[i]);

    auto [start_idx, cfo_raw] = find_preamble(iq.data(), iq.size(), sf, base_down);
    if (start_idx < 0) return std::nullopt;
    int cfo = cfo_raw < N / 2 ? cfo_raw : cfo_raw - N;
    int preamble_len = measure_preamble_length(iq.data(), iq.size(), sf, base_down, start_idx, cfo);

    BurstDetection result;
    result.sf = sf;
    result.start_sample = long(start_idx) * N;
    result.cfo_bins = cfo;
    result.preamble_len = preamble_len;
    return result;
}

}  // namespace rfmon::lora
