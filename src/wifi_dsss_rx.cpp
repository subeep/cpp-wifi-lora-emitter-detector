#include "wifi_dsss_rx.hpp"

#include <algorithm>
#include <cmath>

namespace rfmon::wifi {

namespace {

constexpr double kChipRateHz = 11e6;
constexpr int kSamplesPerChip = 2;                       // exact grid, see resample below
constexpr int kChipsPerSymbol = 11;                      // Barker-11
constexpr int kSamplesPerSymbol = kChipsPerSymbol * kSamplesPerChip;  // 22
constexpr int kBarker[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};

// 802.11b long PLCP: 128 scrambled ONE bits, then the SFD. After
// descrambling the SYNC field reads as a run of ones, which is an
// unambiguous anchor regardless of bit-order convention.
constexpr int kMinSyncRun = 16;
constexpr uint16_t kLongSfd = 0xF3A0;

// Linear-interpolating resample onto the exact chip grid. Matches the
// approach wifi_phy.cpp uses for its Barker correlator: putting the
// signal on a whole-samples-per-chip grid makes the despreader exact
// instead of quantising chip boundaries onto sample boundaries.
std::vector<std::complex<float>> resample_to(const std::complex<float>* x, size_t n,
                                              double in_rate, double out_rate) {
    std::vector<std::complex<float>> out;
    if (n == 0 || in_rate <= 0.0 || out_rate <= 0.0) return out;
    double ratio = in_rate / out_rate;
    size_t n_out = size_t(double(n) / ratio);
    out.resize(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        double pos = double(i) * ratio;
        size_t i0 = size_t(pos);
        if (i0 + 1 >= n) {
            out[i] = x[n - 1];
            continue;
        }
        float frac = float(pos - double(i0));
        out[i] = x[i0] * (1.0f - frac) + x[i0 + 1] * frac;
    }
    return out;
}

// Despreads one symbol: correlate 22 samples against the Barker
// sequence, two samples per chip.
std::complex<double> despread_symbol(const std::vector<std::complex<float>>& x, size_t at) {
    std::complex<double> acc(0.0, 0.0);
    for (int i = 0; i < kSamplesPerSymbol; ++i) {
        double w = double(kBarker[i / kSamplesPerChip]);
        const auto& v = x[at + size_t(i)];
        acc += std::complex<double>(double(v.real()) * w, double(v.imag()) * w);
    }
    return acc;
}

// Reads `count` bits starting at `pos` as an MSB-first integer - used
// only for the SFD pattern compare, where the value is a fixed marker
// rather than a transmitted field.
uint16_t bits_to_u16_msb(const std::vector<uint8_t>& bits, size_t pos) {
    uint16_t v = 0;
    for (int i = 0; i < 16; ++i) v = uint16_t((v << 1) | (bits[pos + size_t(i)] & 1u));
    return v;
}

uint16_t bit_reverse16(uint16_t v) {
    uint16_t r = 0;
    for (int i = 0; i < 16; ++i) {
        if (v & (1u << i)) r = uint16_t(r | (1u << (15 - i)));
    }
    return r;
}

}  // namespace

DsssDecodeResult decode_dsss_burst(const std::complex<float>* x, size_t n,
                                    double sample_rate_hz, double capture_center_hz,
                                    double segment_center_hz) {
    DsssDecodeResult result;
    if (n == 0 || sample_rate_hz <= 0.0) return result;

    // Bring the channel to baseband first if it is offset in the
    // capture. DBPSK tolerates a residual carrier offset well, but not
    // the megahertz-scale offset the scanner's deliberate capture
    // detuning introduces.
    std::vector<std::complex<float>> mixed;
    const std::complex<float>* src = x;
    double offset_hz = segment_center_hz - capture_center_hz;
    if (offset_hz != 0.0) {
        mixed.resize(n);
        double phase = 0.0, inc = -2.0 * M_PI * offset_hz / sample_rate_hz;
        for (size_t i = 0; i < n; ++i) {
            mixed[i] = x[i] * std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
            phase += inc;
            if (phase > M_PI) phase -= 2.0 * M_PI;
            else if (phase < -M_PI) phase += 2.0 * M_PI;
        }
        src = mixed.data();
    }

    const double chip_grid_rate = kChipRateHz * kSamplesPerChip;  // 22 Msps
    std::vector<std::complex<float>> s =
        (std::abs(sample_rate_hz - chip_grid_rate) < 1.0)
            ? std::vector<std::complex<float>>(src, src + n)
            : resample_to(src, n, sample_rate_hz, chip_grid_rate);
    if (s.size() < size_t(kSamplesPerSymbol) * 64) return result;

    // --- Timing recovery -------------------------------------------
    // Search every whole-sample chip phase and keep the one with the
    // most despread energy. At 2 samples/chip the grid is half a chip,
    // which costs about 1.16dB on average and 2.5dB worst case - the
    // largest single yield loss in this chain, and the obvious next
    // improvement (parabolic interpolation of the correlation peak plus
    // a fractional-delay resample) if decode rates disappoint.
    size_t n_symbols_max = (s.size() / size_t(kSamplesPerSymbol)) - 1;
    int best_phase = 0;
    double best_energy = -1.0;
    for (int phase = 0; phase < kSamplesPerSymbol; ++phase) {
        double energy = 0.0;
        size_t count = std::min<size_t>(n_symbols_max, 64);  // preamble is plenty
        for (size_t m = 0; m < count; ++m) {
            size_t at = size_t(phase) + m * size_t(kSamplesPerSymbol);
            if (at + size_t(kSamplesPerSymbol) > s.size()) break;
            energy += std::norm(despread_symbol(s, at));
        }
        if (energy > best_energy) {
            best_energy = energy;
            best_phase = phase;
        }
    }
    result.chip_phase = best_phase;

    // --- Despread every symbol at the winning phase -----------------
    std::vector<std::complex<double>> syms;
    for (size_t at = size_t(best_phase); at + size_t(kSamplesPerSymbol) <= s.size();
         at += size_t(kSamplesPerSymbol)) {
        syms.push_back(despread_symbol(s, at));
    }
    if (syms.size() < 64) return result;

    // --- DBPSK: the bit is carried by the CHANGE in phase ------------
    // 802.11b DBPSK encodes 0 as no phase change and 1 as a 180deg
    // flip, so the differential product's real part carries the bit and
    // no absolute phase reference (and therefore no carrier recovery)
    // is needed - which is also why a residual frequency offset costs
    // so little here.
    std::vector<uint8_t> raw_bits;
    raw_bits.reserve(syms.size());
    for (size_t k = 1; k < syms.size(); ++k) {
        double d = (syms[k] * std::conj(syms[k - 1])).real();
        raw_bits.push_back(uint8_t(d < 0.0 ? 1 : 0));
    }

    // --- Descramble BEFORE looking for anything ---------------------
    // The scrambler covers every transmitted bit, SYNC and SFD and the
    // PLCP header included. Searching raw channel bits for the SFD (as
    // the reference prototype does) only ever works against a generator
    // that also skipped scrambling - it fails on 100% of real frames.
    std::vector<uint8_t> bits = descramble_bits(raw_bits);

    // --- Locate SYNC then SFD ---------------------------------------
    // The SYNC field descrambles to a run of ones, which is a
    // convention-independent anchor. The SFD pattern itself is compared
    // both ways round because bit-order conventions for it are easy to
    // get backwards, and getting it wrong would look like a sensitivity
    // problem rather than a lookup error.
    // Test the SFD pattern at EVERY position preceded by a long enough
    // run of ones, rather than only where a zero bit appears. The SFD
    // (0xF3A0) begins with four ones, so it flows straight out of the
    // all-ones SYNC field with no zero boundary to trigger on - keying
    // the search off the first zero lands four bits into the pattern and
    // never matches.
    size_t sfd_end = 0;
    size_t sfd_start = 0;
    bool found = false;
    for (size_t i = size_t(kMinSyncRun); i + 16 <= bits.size(); ++i) {
        bool sync_ok = true;
        for (size_t j = i - size_t(kMinSyncRun); j < i; ++j) {
            if (!bits[j]) {
                sync_ok = false;
                break;
            }
        }
        if (!sync_ok) continue;
        uint16_t v = bits_to_u16_msb(bits, i);
        if (v == kLongSfd || v == bit_reverse16(kLongSfd)) {
            sfd_start = i;
            sfd_end = i + 16;
            found = true;
            break;
        }
    }
    if (!found) return result;
    result.preamble_found = true;

    // Walk backward from the matched SFD to the true start of the
    // confirmed all-ones run (the search above only required
    // kMinSyncRun=16 consecutive ones to anchor, but a real SYNC field
    // is 128 bits - recover however much of it is actually present).
    // Bit index b in `bits` is the transition from symbol b to symbol
    // b+1 (raw_bits[k] came from syms[k] vs syms[k-1], see the loop
    // above), so the SYNC symbol range is [sync_start_bit, sfd_start]
    // inclusive - sfd_start itself (not sfd_start-1) because the bit at
    // sfd_start-1 is the last transition still inside the all-ones run,
    // landing on symbol sfd_start.
    size_t sync_start_bit = sfd_start - size_t(kMinSyncRun);
    while (sync_start_bit > 0 && bits[sync_start_bit - 1]) --sync_start_bit;
    if (sfd_start < syms.size()) {
        result.sync_symbols.assign(syms.begin() + long(sync_start_bit),
                                    syms.begin() + long(sfd_start) + 1);
    }

    // --- PLCP header: 48 bits, LSB-first within each octet -----------
    if (sfd_end + 48 > bits.size()) return result;
    std::vector<uint8_t> hdr_bits(bits.begin() + long(sfd_end), bits.begin() + long(sfd_end + 48));
    std::vector<uint8_t> hdr = bits_to_bytes_lsb_first(hdr_bits);
    auto plcp = parse_plcp_header(hdr.data(), hdr.size());
    if (!plcp || !plcp->crc_valid) return result;
    result.plcp = plcp;

    // --- PSDU ---------------------------------------------------------
    // LENGTH is the PSDU duration in microseconds; at 1 Mbps one
    // microsecond is exactly one bit, so it doubles as the bit count.
    size_t psdu_bits = size_t(plcp->length_us);
    size_t body_start = sfd_end + 48;
    if (psdu_bits < 8 || body_start + psdu_bits > bits.size()) return result;

    std::vector<uint8_t> psdu_bits_v(bits.begin() + long(body_start),
                                      bits.begin() + long(body_start + psdu_bits));
    std::vector<uint8_t> mpdu = bits_to_bytes_lsb_first(psdu_bits_v);
    result.mpdu_len = mpdu.size();

    // parse_beacon() verifies the FCS itself and rejects anything that
    // is not a beacon or probe response.
    auto info = parse_beacon(mpdu.data(), mpdu.size());
    if (info && info->fcs_valid) result.beacon = info;
    return result;
}

}  // namespace rfmon::wifi
