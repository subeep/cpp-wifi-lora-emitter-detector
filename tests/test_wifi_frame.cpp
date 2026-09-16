// Correctness tests for src/wifi_frame.cpp - the 802.11 frame layer.
//
// Every checksum test here asserts against a PUBLISHED vector, not
// against anything this codebase generates. That is the whole point:
// the reference prototype this work draws on had tests that round-
// tripped through the same tables on both the encode and decode side,
// so they passed regardless of whether those tables matched the actual
// standard - and two of them did not. A golden vector cannot be fooled
// that way.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

std::string hex16(uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%04X", v);
    return b;
}

std::string hex32(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

}  // namespace

int main() {
    // 1. FCS-32 against the universally published CRC-32 check value.
    {
        const char* s = "123456789";
        uint32_t got = fcs32(reinterpret_cast<const uint8_t*>(s), 9);
        check(got == 0xCBF43926u, "fcs32_matches_published_check_value",
              "got " + hex32(got) + ", want 0xCBF43926");
    }
    {
        uint32_t got = fcs32(nullptr, 0);
        check(got == 0x00000000u, "fcs32_empty_input", "got " + hex32(got));
    }

    // 2. THE decisive one: the PLCP CRC-16 against the 802.11b spec's
    // own worked example - SIGNAL 0x0A (1 Mbps), SERVICE 0x00,
    // LENGTH 192 -> 0x5B57.
    //
    // A reflected-0x8408 CRC with no final complement (a far more
    // commonly written CRC-16, and what the reference prototype used)
    // produces 0x1525 here instead. Both "work" in a round-trip test;
    // only one decodes a real access point.
    {
        const uint8_t hdr[4] = {0x0A, 0x00, 0xC0, 0x00};  // LENGTH 192 little-endian
        uint16_t got = plcp_crc16(hdr, 4);
        check(got == 0x5B57u, "plcp_crc16_matches_spec_worked_example",
              "got " + hex16(got) + ", want 0x5B57");
    }

    // 3. A PLCP header round-trip: the parser must accept a header whose
    // CRC we computed independently, and must REJECT one with a flipped
    // bit rather than passing it through.
    //
    // This used to build hdr[4]/hdr[5] as `crc>>8, crc&0xFF` directly -
    // exactly the same MSB-first-per-OCTET convention parse_plcp_header()
    // itself used to assume, so the round-trip passed regardless of
    // whether that assumption was correct. It happened to be wrong (see
    // parse_plcp_header()'s own comment in wifi_frame.cpp): the CRC's
    // two octets are transmitted MSB-first internally, meaning each
    // byte's OWN bit order is reversed relative to a plain big-endian
    // split - bit_reverse8() here mirrors that explicitly, so this test
    // now models actual transmission instead of the decoder's own prior
    // (mistaken) assumption. Confirmed necessary and sufficient by live
    // capture in test 3b below, not by this synthetic test alone.
    {
        auto bit_reverse8 = [](uint16_t v) {
            uint8_t r = 0;
            for (int i = 0; i < 8; ++i) {
                if (v & (1u << i)) r |= uint8_t(1u << (7 - i));
            }
            return r;
        };
        uint8_t hdr[6] = {0x0A, 0x00, 0xC0, 0x00, 0x00, 0x00};
        uint16_t crc = plcp_crc16(hdr, 4);
        hdr[4] = bit_reverse8(uint8_t(crc >> 8));
        hdr[5] = bit_reverse8(uint8_t(crc & 0xFF));
        auto h = parse_plcp_header(hdr, 6);
        check(h.has_value() && h->crc_valid, "plcp_header_accepts_valid_crc",
              h ? ("crc_valid=" + std::to_string(h->crc_valid)) : "no header");
        if (h) {
            check(h->length_us == 192, "plcp_header_length",
                  "got " + std::to_string(h->length_us));
            check(h->rate_mbps() == 1.0, "plcp_header_rate",
                  "got " + std::to_string(h->rate_mbps()) + " Mbps");
        }
        hdr[2] ^= 0x01;  // corrupt the LENGTH field
        auto bad = parse_plcp_header(hdr, 6);
        check(bad.has_value() && !bad->crc_valid, "plcp_header_rejects_corrupted",
              "a single flipped bit must invalidate the CRC");
    }

    // 3b. plcp_header_crc_byte_order_confirmed_against_real_capture -
    // the golden-vector discipline this file's own header describes,
    // applied to the exact bug test 3 could not have caught: a real
    // 802.11b beacon header captured over the air (X310 + real
    // traffic, 2.4GHz), BEFORE this fix, with SIGNAL/SERVICE/LENGTH
    // already decoding perfectly and consistently (confirmed the same
    // three fields on 72 consecutive real captures, several different
    // real frames by their differing LENGTH values) but crc_valid
    // false. bytes[4]/bytes[5] here are the RAW extraction (what
    // bits_to_bytes_lsb_first() actually produced from the real
    // capture - the same convention correctly used for bytes 0-3),
    // not a value constructed to fit any particular theory.
    {
        const uint8_t hdr[6] = {0x0A, 0x04, 0x30, 0x06, 0x85, 0x90};
        auto h = parse_plcp_header(hdr, 6);
        check(h.has_value() && h->crc_valid,
              "plcp_header_crc_byte_order_confirmed_against_real_capture",
              h ? ("crc_valid=" + std::to_string(h->crc_valid) + " crc=" + hex16(h->crc))
                : "no header");
        if (h) {
            check(h->length_us == 1584, "real_capture_length_us",
                  "got " + std::to_string(h->length_us));
            check(h->rate_mbps() == 1.0, "real_capture_rate", "");
        }
    }

    // 4. The descrambler is self-synchronising: scrambling then
    // descrambling must round-trip, and it must converge WITHOUT any
    // seed search. Scrambling here is written independently of
    // descramble_bits() (it feeds back its own OUTPUT, where the
    // descrambler keys off its INPUT) so this is not the same function
    // checked against itself.
    {
        std::vector<uint8_t> plain;
        for (int i = 0; i < 200; ++i) plain.push_back(uint8_t((i * 7 + 3) & 1));

        std::vector<uint8_t> scrambled(plain.size());
        std::vector<uint8_t> hist;
        for (size_t i = 0; i < plain.size(); ++i) {
            uint8_t t4 = (i >= 4) ? scrambled[i - 4] : 0;
            uint8_t t7 = (i >= 7) ? scrambled[i - 7] : 0;
            scrambled[i] = uint8_t((plain[i] ^ t4 ^ t7) & 1);
        }
        auto recovered = descramble_bits(scrambled);

        // The first 7 bits are the convergence window; everything after
        // must match exactly.
        bool ok = true;
        for (size_t i = 7; i < plain.size(); ++i) {
            if (recovered[i] != plain[i]) ok = false;
        }
        check(ok, "descrambler_roundtrip_self_synchronising",
              "converged within 7 bits with no seed search");
    }

    // 5. Error multiplication: one channel bit error must corrupt about
    // three output bits (the bit itself plus its taps at 4 and 7). This
    // is why a decoder is judged on frames-per-second and not BER.
    {
        std::vector<uint8_t> bits(100, 0);
        auto clean = descramble_bits(bits);
        bits[50] ^= 1;
        auto dirty = descramble_bits(bits);
        int diffs = 0;
        for (size_t i = 0; i < bits.size(); ++i) {
            if (clean[i] != dirty[i]) ++diffs;
        }
        check(diffs == 3, "descrambler_multiplies_one_error_into_three",
              "one flipped channel bit produced " + std::to_string(diffs) + " output errors");
    }

    // 6. Beacon parsing: BSSID from Address 3, SSID from IE tag 0,
    // channel from the DS Parameter Set (tag 3), and a valid FCS.
    {
        std::vector<uint8_t> f;
        auto push = [&](std::initializer_list<uint8_t> v) { f.insert(f.end(), v); };
        push({0x80, 0x00});                                      // FC: beacon
        push({0x00, 0x00});                                      // duration
        push({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});              // A1 broadcast
        push({0x5E, 0x52, 0xA1, 0x0B, 0xBF, 0xD7});              // A2 (transmitter)
        push({0x3C, 0x52, 0xA1, 0x0B, 0xBF, 0xD7});              // A3 = BSSID
        push({0x10, 0x00});                                      // sequence control
        push({1, 2, 3, 4, 5, 6, 7, 8});                          // timestamp
        push({0x64, 0x00});                                      // beacon interval 100 TU
        push({0x11, 0x04});                                      // capability
        // IE: SSID
        const char* ssid = "Avgarde_airtel";
        f.push_back(0x00);
        f.push_back(uint8_t(std::strlen(ssid)));
        for (const char* p = ssid; *p; ++p) f.push_back(uint8_t(*p));
        // IE: DS Parameter Set -> channel 9
        push({0x03, 0x01, 0x09});

        uint32_t crc = fcs32(f.data(), f.size());
        f.push_back(uint8_t(crc & 0xFF));
        f.push_back(uint8_t((crc >> 8) & 0xFF));
        f.push_back(uint8_t((crc >> 16) & 0xFF));
        f.push_back(uint8_t((crc >> 24) & 0xFF));

        auto info = parse_beacon(f.data(), f.size());
        check(info.has_value(), "beacon_parses", "");
        if (info) {
            check(info->fcs_valid, "beacon_fcs_valid", "");
            check(info->bssid == "3c:52:a1:0b:bf:d7", "beacon_bssid_is_address3",
                  "got '" + info->bssid + "' (must be A3, not A2 5e:52:...)");
            check(info->ssid == "Avgarde_airtel", "beacon_ssid", "got '" + info->ssid + "'");
            check(info->channel == 9, "beacon_channel_from_ds_param",
                  "got " + std::to_string(info->channel));
            check(info->beacon_interval_tu == 100, "beacon_interval",
                  "got " + std::to_string(info->beacon_interval_tu) + " TU");
        }

        // A corrupted body must fail the FCS rather than yield a
        // plausible-looking wrong answer.
        f[30] ^= 0x01;
        auto bad = parse_beacon(f.data(), f.size());
        check(bad.has_value() && !bad->fcs_valid, "beacon_fcs_catches_corruption",
              "a single flipped body bit must fail the FCS");
    }

    // 7. A hidden network beacons with an all-zero SSID - that must read
    // as empty rather than as a name made of NUL bytes. This is exactly
    // the case that makes virtual BSSIDs hard to tell apart, so it has
    // to be handled honestly.
    {
        std::vector<uint8_t> f;
        auto push = [&](std::initializer_list<uint8_t> v) { f.insert(f.end(), v); };
        push({0x80, 0x00, 0x00, 0x00});
        for (int i = 0; i < 6; ++i) f.push_back(0xFF);
        for (int i = 0; i < 6; ++i) f.push_back(0xAA);
        push({0x9E, 0xBA, 0x5F, 0x37, 0x57, 0x4A});  // A3 = BSSID
        push({0x00, 0x00});
        for (int i = 0; i < 8; ++i) f.push_back(0);
        push({0x64, 0x00, 0x11, 0x04});
        push({0x00, 0x04, 0x00, 0x00, 0x00, 0x00});  // zero-length-equivalent SSID
        push({0x03, 0x01, 0x09});
        uint32_t crc = fcs32(f.data(), f.size());
        for (int i = 0; i < 4; ++i) f.push_back(uint8_t((crc >> (8 * i)) & 0xFF));

        auto info = parse_beacon(f.data(), f.size());
        check(info.has_value() && info->ssid.empty() && info->fcs_valid,
              "hidden_ssid_reads_as_empty",
              info ? ("ssid='" + info->ssid + "'") : "no parse");
        if (info) {
            check(info->bssid == "9e:ba:5f:37:57:4a", "hidden_beacon_bssid",
                  "got '" + info->bssid + "'");
        }
    }

    // 8. Non-management frames must be rejected outright rather than
    // parsed into garbage identity.
    {
        std::vector<uint8_t> f(64, 0);
        f[0] = 0x08;  // type 2 (data), subtype 0
        auto info = parse_beacon(f.data(), f.size());
        check(!info.has_value(), "data_frame_rejected", "a data frame is not a beacon");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll Wi-Fi frame-layer checks passed.\n");
    return 0;
}
