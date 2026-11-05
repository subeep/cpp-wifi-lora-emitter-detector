// Legacy 802.11a/g (6 Mbps, BPSK r=1/2) transmit-waveform generator for the
// RF identity CFO-match test (docs/WIFI_RF_IDENTITY_2026-11-04.md). It builds a
// deauthentication frame spoofing a chosen BSSID as 20 MS/s baseband IQ
// (cf32), for a HackRF to play at a controlled carrier offset against an
// access point we own. A built-in self-test decodes the waveform through this
// project's own receiver (decode_ofdm_burst) and checks the FCS, so the
// modulator is validated with no radio before anything is transmitted.
#include "wifi_ofdm_rx.hpp"
#include "wifi_phy.hpp"
#include "wifi_frame.hpp"
#include <kissfft/kiss_fft.h>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using C = std::complex<float>;
using namespace rfmon;
static const double PI = 3.14159265358979323846;

// --- bit helpers ---------------------------------------------------------
static int parity(unsigned v) { return __builtin_parity(v); }

// 7-bit data scrambler, x^7 + x^4 + 1 (IEEE 17.3.5.5).
static void scramble(std::vector<uint8_t>& bits, unsigned seed) {
    unsigned s = seed & 0x7f;
    for (auto& b : bits) {
        const unsigned fb = ((s >> 6) ^ (s >> 3)) & 1;  // x^7 ^ x^4
        s = ((s << 1) | fb) & 0x7f;
        b ^= uint8_t(fb);
    }
}

// Rate-1/2 convolutional encoder, g0=133o, g1=171o (IEEE 17.3.5.6).
static std::vector<uint8_t> conv_encode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() * 2);
    unsigned reg = 0;
    for (uint8_t u : in) {
        reg = (reg >> 1) | (unsigned(u & 1) << 6);
        out.push_back(uint8_t(parity(reg & 0133u)));
        out.push_back(uint8_t(parity(reg & 0171u)));
    }
    return out;
}

// Interleaver first permutation, N_CBPS=48, BPSK (second permutation is
// identity for N_BPSC=1). IEEE 17.3.5.7.
static std::vector<uint8_t> interleave48(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out(48);
    for (int k = 0; k < 48; ++k) {
        const int i = (48 / 16) * (k % 16) + (k / 16);
        out[i] = in[k];
    }
    return out;
}

// Pilot polarity, matching this project's receiver (wifi_ofdm_rx.cpp).
static int pilot_polarity(size_t symbol) {
    unsigned state = 127, bit = 0;
    for (size_t i = 0; i <= symbol % 127; ++i) { bit = ((state >> 6) ^ (state >> 3)) & 1; state = ((state << 1) | bit) & 127; }
    return bit ? -1 : 1;
}

// --- frequency-domain symbols -> time ------------------------------------
static const int kDataSc[48] = {
    -26,-25,-24,-23,-22,-20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-10,-9,-8,
    -6,-5,-4,-3,-2,-1,1,2,3,4,5,6,8,9,10,11,12,13,14,15,16,17,18,19,20,22,23,24,25,26};
static const int kPilotSc[4] = {-21,-7,7,21};

static std::vector<C> ifft64(const std::array<C,64>& freq) {
    static kiss_fft_cfg cfg = kiss_fft_alloc(64, 1 /*inverse*/, nullptr, nullptr);
    kiss_fft_cpx in[64], out[64];
    for (int i = 0; i < 64; ++i) { in[i].r = freq[i].real(); in[i].i = freq[i].imag(); }
    kiss_fft(cfg, in, out);
    std::vector<C> t(64);
    for (int i = 0; i < 64; ++i) t[i] = C(out[i].r, out[i].i) / 64.0f;
    return t;
}
// One 80-sample OFDM symbol: 48 data points + 4 pilots, DC=0, 16-sample CP.
static std::vector<C> ofdm_symbol(const C data[48], size_t polarity_index) {
    std::array<C,64> f{};
    for (int i = 0; i < 48; ++i) f[(kDataSc[i] + 64) % 64] = data[i];
    const int p = pilot_polarity(polarity_index);
    for (int i = 0; i < 4; ++i) f[(kPilotSc[i] + 64) % 64] = C(float((i == 3 ? -1 : 1) * p), 0);
    auto t = ifft64(f);
    std::vector<C> sym(80);
    for (int i = 0; i < 16; ++i) sym[i] = t[48 + i];  // cyclic prefix
    for (int i = 0; i < 64; ++i) sym[16 + i] = t[i];
    return sym;
}

// L-STF (160) and L-LTF (160) from the standard sequences.
static std::vector<C> short_preamble() {
    std::array<C,64> f{};
    const float a = std::sqrt(13.f / 6.f);
    auto set = [&](int k, float re, float im) { f[(k + 64) % 64] = C(re * a, im * a); };
    const int sc[] = {-24,-20,-16,-12,-8,-4,4,8,12,16,20,24};
    const float v[][2] = {{1,1},{-1,-1},{1,1},{-1,-1},{-1,-1},{1,1},{-1,-1},{-1,-1},{1,1},{1,1},{1,1},{1,1}};
    for (int i = 0; i < 12; ++i) set(sc[i], v[i][0], v[i][1]);
    auto t = ifft64(f);
    std::vector<C> out(160);
    for (int i = 0; i < 160; ++i) out[i] = t[i % 64];
    return out;
}
static std::vector<C> long_preamble() {
    static const int L[53] = {1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,
                              0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1};
    std::array<C,64> f{};
    for (int k = -26; k <= 26; ++k) f[(k + 64) % 64] = C(float(L[k + 26]), 0);
    auto t = ifft64(f);
    std::vector<C> out(160);
    for (int i = 0; i < 32; ++i) out[i] = t[32 + i];       // double guard interval
    for (int i = 0; i < 64; ++i) out[32 + i] = t[i];
    for (int i = 0; i < 64; ++i) out[96 + i] = t[i];
    return out;
}

// SIGNAL field (L-SIG): 6 Mbps always; carries the PSDU length.
static std::vector<C> signal_symbol(size_t psdu_len) {
    std::vector<uint8_t> b(24, 0);
    const int rate[4] = {1,1,0,1};               // 6 Mbps = 1101
    for (int i = 0; i < 4; ++i) b[i] = uint8_t(rate[i]);
    b[4] = 0;                                     // reserved
    for (int i = 0; i < 12; ++i) b[5 + i] = uint8_t((psdu_len >> i) & 1);  // LENGTH, LSB first
    int par = 0; for (int i = 0; i < 17; ++i) par ^= b[i];
    b[17] = uint8_t(par);                         // even parity
    // b[18..23] tail = 0
    auto coded = interleave48(conv_encode(b));
    C pts[48];
    for (int i = 0; i < 48; ++i) pts[i] = C(coded[i] ? 1.f : -1.f, 0);
    return ofdm_symbol(pts, 0);
}

// --- frame assembly ------------------------------------------------------
static std::vector<C> make_frame(const uint8_t bssid[6]) {
    // Deauthentication MPDU: 24-byte header + reason(2) + FCS(4).
    std::vector<uint8_t> mpdu;
    mpdu.push_back(0xc0); mpdu.push_back(0x00);        // frame control: deauth
    mpdu.push_back(0x00); mpdu.push_back(0x00);        // duration
    for (int i = 0; i < 6; ++i) mpdu.push_back(0xff);  // addr1: broadcast
    for (int i = 0; i < 6; ++i) mpdu.push_back(bssid[i]);  // addr2: transmitter (spoofed AP)
    for (int i = 0; i < 6; ++i) mpdu.push_back(bssid[i]);  // addr3: BSSID
    mpdu.push_back(0x10); mpdu.push_back(0x00);        // sequence control
    mpdu.push_back(0x02); mpdu.push_back(0x00);        // reason: prev auth invalid
    const uint32_t fcs = wifi::fcs32(mpdu.data(), mpdu.size());
    for (int i = 0; i < 4; ++i) mpdu.push_back(uint8_t(fcs >> (8 * i)));
    const size_t psdu = mpdu.size();

    // DATA field bits: SERVICE(16) + PSDU + TAIL(6) + PAD to N_DBPS=24.
    const size_t nbits = 16 + psdu * 8 + 6;
    const size_t nsym = (nbits + 23) / 24;
    std::vector<uint8_t> data(nsym * 24, 0);
    for (size_t i = 0; i < psdu; ++i)
        for (int bit = 0; bit < 8; ++bit) data[16 + i * 8 + bit] = uint8_t((mpdu[i] >> bit) & 1);  // LSB first
    const size_t tail_at = 16 + psdu * 8;
    scramble(data, 0x5d);
    for (int i = 0; i < 6; ++i) data[tail_at + i] = 0;  // tail zeroed after scrambling
    auto coded = conv_encode(data);                      // 48 coded bits per symbol

    std::vector<C> out;
    auto stf = short_preamble(), ltf = long_preamble(), lsig = signal_symbol(psdu);
    out.insert(out.end(), stf.begin(), stf.end());
    out.insert(out.end(), ltf.begin(), ltf.end());
    out.insert(out.end(), lsig.begin(), lsig.end());
    for (size_t s = 0; s < nsym; ++s) {
        auto inter = interleave48({coded.begin() + long(s * 48), coded.begin() + long(s * 48 + 48)});
        C pts[48];
        for (int i = 0; i < 48; ++i) pts[i] = C(inter[i] ? 1.f : -1.f, 0);
        auto sym = ofdm_symbol(pts, s + 1);  // L-SIG is index 0
        out.insert(out.end(), sym.begin(), sym.end());
    }
    return out;
}

int main(int argc, char** argv) {
    uint8_t bssid[6] = {0xfa,0xfa,0x99,0xc6,0x4b,0xc1};
    std::string out_path;
    int reps = 40; double gap_us = 5000, rms = 0.2;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bssid" && i + 1 < argc) {
            int v[6]; if (std::sscanf(argv[++i], "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6)
                for (int k = 0; k < 6; ++k) bssid[k] = uint8_t(v[k]);
        } else if (a == "--reps" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "--gap-us" && i + 1 < argc) gap_us = std::atof(argv[++i]);
        else if (a == "--rms" && i + 1 < argc) rms = std::atof(argv[++i]);
        else if (out_path.empty()) out_path = a;
    }

    auto frame = make_frame(bssid);
    std::printf("frame: %zu samples (%.1f us at 20 MS/s), %zu-sample preamble\n", frame.size(), frame.size() / 20.0, size_t(320));

    // Self-test: decode the single frame through our own receiver.
    {
        std::vector<C> pad(200, C(0,0));
        std::vector<C> one;
        one.insert(one.end(), pad.begin(), pad.end());
        one.insert(one.end(), frame.begin(), frame.end());
        one.insert(one.end(), pad.begin(), pad.end());
        auto r = wifi::decode_ofdm_burst(one.data(), one.size(), 20e6, 2.412e9, 2.412e9);
        std::printf("self-test: status=\"%s\" preamble=%d header=%d fcs=%d rate=%d psdu=%zu\n",
                    r.status.c_str(), r.preamble_found, r.header_valid, r.fcs_valid, r.rate_mbps, r.psdu_length);
        if (r.fcs_valid && !r.mpdu.empty()) {
            std::printf("self-test MPDU:");
            for (size_t i = 0; i < r.mpdu.size() && i < 30; ++i) std::printf(" %02x", r.mpdu[i]);
            std::printf("\n  -> %s deauth from %02x:%02x:%02x:%02x:%02x:%02x\n",
                        r.mpdu[0] == 0xc0 ? "VALID" : "NOT",
                        r.mpdu[10],r.mpdu[11],r.mpdu[12],r.mpdu[13],r.mpdu[14],r.mpdu[15]);
        } else {
            std::printf("self-test FAILED: receiver did not recover an FCS-valid frame\n");
        }
        if (out_path.empty()) return r.fcs_valid ? 0 : 1;
    }

    // Normalise amplitude, then write reps frames separated by gaps (cf32).
    double p = 0; for (auto s : frame) p += std::norm(s);
    const float scale = float(rms / std::sqrt(p / frame.size()));
    const size_t gap = size_t(gap_us * 20.0);
    std::ofstream out(out_path, std::ios::binary);
    std::vector<C> zeros(gap, C(0,0));
    for (int r = 0; r < reps; ++r) {
        std::vector<C> scaled(frame.size());
        for (size_t i = 0; i < frame.size(); ++i) scaled[i] = frame[i] * scale;
        out.write(reinterpret_cast<const char*>(scaled.data()), long(scaled.size() * sizeof(C)));
        out.write(reinterpret_cast<const char*>(zeros.data()), long(zeros.size() * sizeof(C)));
    }
    std::printf("wrote %d frames to %s (%.1f ms)\n", reps, out_path.c_str(), reps * (frame.size() + gap) / 20000.0);
    return 0;
}
