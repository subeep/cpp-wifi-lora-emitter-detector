// End-to-end test for the 802.11b 1 Mbps DSSS receive chain
// (src/wifi_dsss_rx.cpp): synthesise a real-structured beacon, put it
// through the radio's own impairments, and require the decoder to
// recover the BSSID and SSID.
//
// The transmitter below is written INDEPENDENTLY of the receiver, which
// is the point. It scrambles by feeding back its own OUTPUT (the
// descrambler keys off its INPUT), spreads with a literal Barker
// sequence, and builds the PLCP header from the spec's field layout.
// Nothing is shared with the code under test except the standard
// itself - so a passing test means the chain decodes 802.11, not that
// two copies of the same mistake agree with each other. That distinction
// is not hypothetical here: the reference prototype this work draws on
// had a green test suite built exactly the wrong way round.
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <chrono>

#include "security/wifi_mac_frame.hpp"
#include "wifi_burst_policy.hpp"
#include "wifi_dsss_rx.hpp"
#include "wifi_frame.hpp"

using namespace rfmon::wifi;

namespace {

int failures = 0;

void check(bool cond, const std::string& name, const std::string& detail) {
    if (cond) {
        std::printf("PASS [%s]: %s\n", name.c_str(), detail.c_str());
    } else {
        std::printf("FAIL [%s]: %s\n", name.c_str(), detail.c_str());
        ++failures;
    }
}

constexpr int kBarker[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};

void push_byte_lsb_first(std::vector<uint8_t>& bits, uint8_t v) {
    for (int b = 0; b < 8; ++b) bits.push_back(uint8_t((v >> b) & 1u));
}

// The PLCP header's CRC-16 field specifically is transmitted MSB-first
// per octet, unlike every other field here (SIGNAL/SERVICE/LENGTH,
// and the payload) - a real, documented 802.11b asymmetry, confirmed
// against live over-the-air capture (see wifi_frame.cpp's
// parse_plcp_header() and tests/test_wifi_frame.cpp's
// plcp_header_crc_byte_order_confirmed_against_real_capture). This
// generator used to push the CRC bytes with push_byte_lsb_first() too
// - independently written from the decoder, but it made the identical
// wrong assumption, so this synthetic test round-tripped regardless
// of whether that assumption was correct. It wasn't; fixed here to
// model actual transmission.
void push_byte_msb_first(std::vector<uint8_t>& bits, uint8_t v) {
    for (int b = 7; b >= 0; --b) bits.push_back(uint8_t((v >> b) & 1u));
}

// A complete beacon MPDU including its FCS, built from the 802.11
// management-frame layout.
std::vector<uint8_t> build_beacon_mpdu(const std::string& bssid_bytes, const std::string& ssid,
                                        int channel) {
    std::vector<uint8_t> f;
    auto push = [&](std::initializer_list<uint8_t> v) { f.insert(f.end(), v); };
    push({0x80, 0x00});                          // FC: management / beacon
    push({0x00, 0x00});                          // duration
    for (int i = 0; i < 6; ++i) f.push_back(0xFF);   // A1 broadcast
    for (int i = 0; i < 6; ++i) f.push_back(0x11);   // A2 transmitter
    for (char c : bssid_bytes) f.push_back(uint8_t(c));  // A3 = BSSID
    push({0x00, 0x00});                          // sequence control
    for (int i = 0; i < 8; ++i) f.push_back(uint8_t(i));  // timestamp
    push({0x64, 0x00});                          // beacon interval 100 TU
    push({0x01, 0x04});                          // capability
    f.push_back(0x00);                           // IE: SSID
    f.push_back(uint8_t(ssid.size()));
    for (char c : ssid) f.push_back(uint8_t(c));
    push({0x03, 0x01});                          // IE: DS Parameter Set
    f.push_back(uint8_t(channel));

    uint32_t crc = fcs32(f.data(), f.size());
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t((crc >> (8 * i)) & 0xFF));
    return f;
}

// A 30-octet deauthentication MPDU (header, reason, FCS) from the 802.11
// management-frame layout - the short-burst case the security path adds.
std::vector<uint8_t> build_deauth_mpdu(uint16_t reason, uint16_t seq) {
    std::vector<uint8_t> f = {0xC0, 0x00, 0x3A, 0x01};             // FC deauth, duration 314us
    for (int i = 0; i < 6; ++i) f.push_back(0xFF);                 // A1 broadcast
    const uint8_t ap[6] = {0x3c, 0x52, 0xa1, 0x0b, 0xbf, 0xd7};
    f.insert(f.end(), ap, ap + 6);                                 // A2 transmitter
    f.insert(f.end(), ap, ap + 6);                                 // A3 BSSID
    f.push_back(uint8_t((seq << 4) & 0xF0)); f.push_back(uint8_t(seq >> 4));
    f.push_back(uint8_t(reason & 0xFF)); f.push_back(uint8_t(reason >> 8));
    uint32_t crc = fcs32(f.data(), f.size());
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t((crc >> (8 * i)) & 0xFF));
    return f;
}

// Full 1 Mbps long-preamble PPDU as complex baseband at 22 Msps
// (2 samples/chip).
std::vector<std::complex<float>> build_dsss_ppdu(const std::vector<uint8_t>& mpdu, double snr_db,
                                                  double cfo_hz, unsigned seed, int lead_symbols,
                                                  uint8_t signal = 0x0A) {
    // --- assemble the pre-scrambler bit stream --------------------
    std::vector<uint8_t> plain;
    for (int i = 0; i < 128; ++i) plain.push_back(1);  // SYNC: 128 ones
    // SFD 0xF3A0, MSB-first on the air.
    for (int i = 15; i >= 0; --i) plain.push_back(uint8_t((0xF3A0u >> i) & 1u));

    // PLCP header: SIGNAL, SERVICE, LENGTH, then CRC-16 MSB-first.
    // SIGNAL is 0x0A (1 Mbps) unless a test deliberately announces another
    // rate; the payload bits below are always DBPSK either way.
    uint8_t service = 0x00;
    uint16_t length_us = uint16_t(mpdu.size() * 8);  // 1 bit per us at 1 Mbps
    uint8_t hdr[4] = {signal, service, uint8_t(length_us & 0xFF), uint8_t(length_us >> 8)};
    uint16_t crc = plcp_crc16(hdr, 4);
    for (uint8_t v : hdr) push_byte_lsb_first(plain, v);
    push_byte_msb_first(plain, uint8_t(crc >> 8));
    push_byte_msb_first(plain, uint8_t(crc & 0xFF));

    for (uint8_t v : mpdu) push_byte_lsb_first(plain, v);

    // --- scramble: x^7 + x^4 + 1, feeding back the OUTPUT ----------
    std::vector<uint8_t> scr(plain.size());
    for (size_t i = 0; i < plain.size(); ++i) {
        uint8_t t4 = (i >= 4) ? scr[i - 4] : 0;
        uint8_t t7 = (i >= 7) ? scr[i - 7] : 0;
        scr[i] = uint8_t((plain[i] ^ t4 ^ t7) & 1u);
    }

    // --- DBPSK: 1 flips the carrier 180deg, 0 leaves it ------------
    std::vector<int> sym_sign;
    sym_sign.push_back(+1);  // arbitrary reference symbol
    for (uint8_t b : scr) sym_sign.push_back(b ? -sym_sign.back() : sym_sign.back());

    // --- Barker spread at 2 samples/chip ---------------------------
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<std::complex<float>> out;
    double noise_sigma = std::sqrt(0.5 / std::pow(10.0, snr_db / 10.0));
    for (int i = 0; i < lead_symbols * 22; ++i) {
        out.emplace_back(float(g(rng) * noise_sigma), float(g(rng) * noise_sigma));
    }
    for (int sgn : sym_sign) {
        for (int c = 0; c < 11; ++c) {
            for (int sp = 0; sp < 2; ++sp) {
                out.emplace_back(float(double(sgn * kBarker[c])), 0.0f);
            }
        }
    }

    // --- impairments: carrier offset, then noise -------------------
    if (cfo_hz != 0.0) {
        double phase = 0.0, inc = 2.0 * M_PI * cfo_hz / 22e6;
        for (auto& v : out) {
            v *= std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
            phase += inc;
            if (phase > M_PI) phase -= 2 * M_PI;
        }
    }
    for (size_t i = size_t(lead_symbols) * 22; i < out.size(); ++i) {
        out[i] += std::complex<float>(float(g(rng) * noise_sigma), float(g(rng) * noise_sigma));
    }
    // Trailing margin. A real burst window from detect_bursts() extends
    // past the frame, and the FCS needs the WHOLE PSDU - without some
    // slack here any resampling in the test truncates the tail and the
    // decode fails for a reason that has nothing to do with the radio.
    for (int i = 0; i < 20 * 22; ++i) {
        out.emplace_back(float(g(rng) * noise_sigma), float(g(rng) * noise_sigma));
    }
    return out;
}

}  // namespace

int main() {
    const std::string bssid_raw("\x3c\x52\xa1\x0b\xbf\xd7", 6);
    const std::string want_bssid = "3c:52:a1:0b:bf:d7";
    const std::string want_ssid = "Avgarde_airtel";

    // 1. Clean end-to-end decode.
    {
        auto mpdu = build_beacon_mpdu(bssid_raw, want_ssid, 9);
        auto iq = build_dsss_ppdu(mpdu, /*snr_db=*/30.0, /*cfo_hz=*/0.0, 1, /*lead_symbols=*/7);
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(r.preamble_found, "dsss_preamble_found", "SYNC+SFD located");
        check(r.plcp.has_value() && r.plcp->crc_valid, "dsss_plcp_header_crc_valid",
              r.plcp ? ("rate=" + std::to_string(r.plcp->rate_mbps()) + "Mbps len=" +
                         std::to_string(r.plcp->length_us))
                     : "no header");
        check(r.beacon.has_value(), "dsss_beacon_decoded",
              r.beacon ? "FCS passed" : "no beacon recovered");
        if (r.beacon) {
            check(r.beacon->bssid == want_bssid, "dsss_bssid_recovered",
                  "got '" + r.beacon->bssid + "'");
            check(r.beacon->ssid == want_ssid, "dsss_ssid_recovered",
                  "got '" + r.beacon->ssid + "'");
            check(r.beacon->channel == 9, "dsss_channel_recovered",
                  "got " + std::to_string(r.beacon->channel));
        }
    }

    // 2. Decoding from the scanner's real capture rate, which needs
    // resampling onto the chip grid.
    //
    // 22.222 Msps (200MHz master clock / decim 9) is deliberate: it
    // gives 2.02 samples/chip. The previous 20 Msps gave 1.818, below
    // the 2.0 floor a despreader needs, and this same decoder recovers
    // NOTHING at that rate - a hardware limit, not a code defect.
    {
        auto mpdu = build_beacon_mpdu(bssid_raw, want_ssid, 9);
        auto iq22 = build_dsss_ppdu(mpdu, 30.0, 0.0, 2, 7);
        // Resample 22 -> 22.222 Msps to stand in for the real capture.
        std::vector<std::complex<float>> iq20;
        double ratio = 22e6 / 22.222e6;
        size_t n_out = size_t(double(iq22.size()) / ratio);
        for (size_t i = 0; i < n_out; ++i) {
            double pos = double(i) * ratio;
            size_t i0 = size_t(pos);
            if (i0 + 1 >= iq22.size()) break;
            float frac = float(pos - double(i0));
            iq20.push_back(iq22[i0] * (1.0f - frac) + iq22[i0 + 1] * frac);
        }
        auto r = decode_dsss_burst(iq20.data(), iq20.size(), 22.222e6);
        check(r.beacon.has_value() && r.beacon->bssid == want_bssid, "dsss_decodes_at_capture_rate",
              r.beacon ? ("bssid=" + r.beacon->bssid) : "no beacon at 22.222 Msps");
    }

    // 3. Carrier frequency offset. DBPSK carries its bit in the phase
    // CHANGE, so it needs no absolute phase reference and should shrug
    // off the ~55kHz a 20ppm AP plus the X310's own TCXO can produce.
    {
        auto mpdu = build_beacon_mpdu(bssid_raw, want_ssid, 9);
        for (double cfo : {20e3, 55e3, 100e3}) {
            auto iq = build_dsss_ppdu(mpdu, 25.0, cfo, 3, 7);
            auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
            check(r.beacon.has_value(), "dsss_tolerates_cfo_" + std::to_string(int(cfo / 1e3)) + "khz",
                  r.beacon ? "decoded" : "failed");
        }
    }

    // 4. Yield versus SNR. Precision is guaranteed by the FCS, so the
    // only real question for this chain is what fraction of beacons
    // decode - report it as a curve rather than a pass/fail.
    {
        std::printf("\n=== 1 Mbps beacon decode yield vs SNR ===\n");
        auto mpdu = build_beacon_mpdu(bssid_raw, want_ssid, 9);
        int yield_at_10 = 0;
        for (double snr : {20.0, 15.0, 10.0, 5.0, 0.0, -3.0}) {
            int ok = 0, wrong = 0;
            constexpr int kTrials = 20;
            for (int t = 0; t < kTrials; ++t) {
                auto iq = build_dsss_ppdu(mpdu, snr, 10e3, unsigned(100 + t), 7);
                auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
                if (r.beacon) {
                    if (r.beacon->bssid == want_bssid && r.beacon->ssid == want_ssid) ++ok;
                    else ++wrong;
                }
            }
            if (snr == 10.0) yield_at_10 = ok;
            std::printf("  SNR %5.1f dB -> %2d/%d decoded, %d WRONG\n", snr, ok, kTrials, wrong);
            // The FCS must make a wrong decode essentially impossible at
            // any SNR - a frame either checksums or it is discarded.
            check(wrong == 0, "dsss_no_wrong_decode_at_" + std::to_string(int(snr)) + "db",
                  std::to_string(wrong) + " frames decoded to the wrong identity");
        }
        check(yield_at_10 >= 18, "dsss_yield_at_10db",
              std::to_string(yield_at_10) + "/20 at 10dB SNR");
    }

    // 5. Pure noise must never produce a beacon.
    {
        std::mt19937 rng(999);
        std::normal_distribution<float> g(0.0f, 1.0f);
        std::vector<std::complex<float>> iq(200000);
        for (auto& v : iq) v = {g(rng), g(rng)};
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(!r.beacon.has_value(), "dsss_no_beacon_from_noise",
              r.beacon ? ("fabricated " + r.beacon->bssid) : "correctly nothing");
    }

    // 6. Security path (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package A
    // step 3): a short non-beacon frame is exposed as FCS-checked bytes.
    const std::string ap_mac = "3c:52:a1:0b:bf:d7";
    {
        auto mpdu = build_deauth_mpdu(7, 291);
        check(mpdu.size() == 30, "deauth_mpdu_is_30_octets", std::to_string(mpdu.size()));
        auto iq = build_dsss_ppdu(mpdu, 25.0, 10e3, 7, 7);
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(r.fcs_valid && r.psdu_complete && r.rate_supported, "deauth_fcs_valid", r.status);
        check(r.mpdu == mpdu, "deauth_mpdu_bytes_exact", std::to_string(r.mpdu.size()) + " octets");
        check(r.mpdu_len == r.mpdu.size(), "deauth_mpdu_len_consistent", std::to_string(r.mpdu_len));
        check(!r.beacon, "deauth_not_a_beacon", r.beacon ? "beacon fabricated" : "no beacon");
        check(r.status == "FCS-valid non-beacon frame", "deauth_status", r.status);
        auto f = rfmon::wifi_security::parse_mac_frame(r.mpdu.data(), r.mpdu.size());
        check(f.fcs_valid() && f.is_management(rfmon::wifi_security::mgmt::Deauthentication) &&
                  f.management && f.management->reason_code == 7 && f.sequence_number == 291 &&
                  f.bssid && rfmon::wifi_security::format_mac(*f.bssid) == ap_mac,
              "deauth_parsed_from_decoded_bytes", rfmon::wifi_security::describe(f));
        // On-air length: 192us preamble+header plus 240 PSDU bits.
        const double on_air_s = (128 + 16 + 48 + 30 * 8) * 1e-6;
        auto p = dsss_burst_policy(on_air_s, true);
        check(p.decode && p.security_only && !p.beacon_cadence && !p.identity_and_fingerprint,
              "deauth_length_is_security_only", std::to_string(on_air_s * 1e6) + " us");
    }
    // 7. The beacon path still exposes its bytes, with beacon identity unchanged.
    {
        auto mpdu = build_beacon_mpdu(bssid_raw, want_ssid, 9);
        auto iq = build_dsss_ppdu(mpdu, 30.0, 0.0, 8, 7);
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(r.beacon && r.beacon->bssid == want_bssid && r.fcs_valid && r.mpdu == mpdu &&
                  r.status == "FCS-valid beacon/probe response",
              "beacon_bytes_and_identity", r.status);
    }
    // 8. A corrupted FCS keeps the bytes for diagnostics but never validates.
    {
        auto mpdu = build_deauth_mpdu(3, 12);
        mpdu.back() ^= 0x5A;
        auto iq = build_dsss_ppdu(mpdu, 30.0, 0.0, 9, 7);
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(r.psdu_complete && !r.fcs_valid && r.mpdu == mpdu && r.status == "FCS failed" && !r.beacon,
              "corrupted_fcs_rejected", r.status);
    }
    // 9. A header announcing an undecoded rate stops at the header.
    {
        auto mpdu = build_deauth_mpdu(3, 13);
        auto iq = build_dsss_ppdu(mpdu, 30.0, 0.0, 10, 7, /*signal=*/0x14);
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(r.plcp && r.plcp->crc_valid && !r.rate_supported && r.mpdu.empty() && !r.fcs_valid &&
                  r.status.find("2.0 Mbps") != std::string::npos,
              "unsupported_rate_reported", r.status);
    }
    // 10. A burst window that ends inside the PSDU says so.
    {
        auto mpdu = build_deauth_mpdu(3, 14);
        auto iq = build_dsss_ppdu(mpdu, 30.0, 0.0, 11, 7);
        iq.resize(size_t((7 + 128 + 16 + 48 + 120) * 22));  // stops half-way through the PSDU
        auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
        check(r.plcp && !r.psdu_complete && r.mpdu.empty() && r.status == "PSDU incomplete in burst window",
              "truncated_psdu_reported", r.status);
    }
    // 11. Short-frame yield vs SNR: the FCS must still make wrong frames impossible.
    {
        std::printf("\n=== 1 Mbps deauth (30 octets) decode yield vs SNR ===\n");
        for (double snr : {20.0, 10.0, 5.0, 0.0, -3.0}) {
            int ok = 0, wrong = 0;
            constexpr int kTrials = 20;
            for (int t = 0; t < kTrials; ++t) {
                auto mpdu = build_deauth_mpdu(uint16_t(1 + t % 7), uint16_t(t));
                auto iq = build_dsss_ppdu(mpdu, snr, 10e3, unsigned(500 + t), 7);
                auto r = decode_dsss_burst(iq.data(), iq.size(), 22e6);
                if (r.fcs_valid) (r.mpdu == mpdu ? ok : wrong)++;
            }
            std::printf("  SNR %5.1f dB -> %2d/%d decoded, %d WRONG\n", snr, ok, kTrials, wrong);
            check(wrong == 0, "deauth_no_wrong_decode_at_" + std::to_string(int(snr)) + "db",
                  std::to_string(wrong) + " wrong");
            if (snr == 10.0) check(ok >= 18, "deauth_yield_at_10db", std::to_string(ok) + "/20");
        }
    }
    // 12. Eligibility policy: security decoding never widens cadence,
    // identity or fingerprint eligibility.
    {
        auto p_beacon = dsss_burst_policy(2e-3, true);
        check(p_beacon.decode && p_beacon.beacon_cadence && p_beacon.identity_and_fingerprint &&
                  !p_beacon.security_only, "policy_beacon_length", "unchanged treatment");
        auto p_edge = dsss_burst_policy(rfmon::WIFI_BEACON_MIN_DURATION_S, true);
        check(p_edge.beacon_cadence && !p_edge.security_only, "policy_beacon_boundary", "1 ms is beacon-eligible");
        auto p_short = dsss_burst_policy(rfmon::WIFI_SECURITY_DSSS_MIN_DURATION_S, true);
        check(p_short.decode && p_short.security_only && !p_short.beacon_cadence && !p_short.identity_and_fingerprint,
              "policy_security_boundary", "250 us is security-only");
        auto p_tiny = dsss_burst_policy(100e-6, true);
        check(!p_tiny.decode && !p_tiny.beacon_cadence && !p_tiny.identity_and_fingerprint, "policy_too_short",
              "cannot hold a complete MPDU");
        auto p_off = dsss_burst_policy(432e-6, false);
        check(!p_off.decode && !p_off.security_only, "policy_disabled_restores_previous", "no extra decode");
        auto p_off_beacon = dsss_burst_policy(2e-3, false);
        check(p_off_beacon.decode && p_off_beacon.beacon_cadence && p_off_beacon.identity_and_fingerprint,
              "policy_disabled_keeps_beacon_path", "beacon path independent of the switch");
    }
    // 13. Cost of the extra work, reported rather than asserted - package B
    // measures real processing capacity against live dead time.
    {
        auto mpdu = build_deauth_mpdu(7, 1);
        auto iq = build_dsss_ppdu(mpdu, 20.0, 10e3, 42, 7);
        constexpr int kRuns = 200;
        auto t0 = std::chrono::steady_clock::now();
        int decoded = 0;
        for (int i = 0; i < kRuns; ++i) decoded += decode_dsss_burst(iq.data(), iq.size(), 22e6).fcs_valid;
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kRuns;
        std::printf("\n  short-frame decode cost: %.1f us per burst at 22 Msps (%d/%d decoded)\n", us, decoded, kRuns);
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll 802.11b DSSS receive-chain checks passed.\n");
    return 0;
}
