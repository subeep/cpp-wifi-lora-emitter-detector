#include "wifi_frame.hpp"

#include <cstdio>

namespace rfmon::wifi {

namespace {

// 802.11 management frame layout (all little-endian multi-octet fields).
constexpr size_t kMacHeaderLen = 24;   // FC(2) Dur(2) A1(6) A2(6) A3(6) Seq(2)
constexpr size_t kFixedParamsLen = 12; // Timestamp(8) BeaconInterval(2) Capability(2)
constexpr size_t kFcsLen = 4;
constexpr size_t kAddr3Offset = 16;    // FC(2) + Dur(2) + A1(6) + A2(6)

constexpr uint8_t kIeSsid = 0;
constexpr uint8_t kIeDsParameterSet = 3;

// Beacon (subtype 8) and Probe Response (subtype 5), both management
// (type 0). Probe responses carry the identical body layout and reveal
// hidden-SSID networks that beacon with a null SSID.
constexpr uint8_t kSubtypeBeacon = 8;
constexpr uint8_t kSubtypeProbeResponse = 5;

std::string format_mac(const uint8_t* p) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4],
                  p[5]);
    return std::string(buf);
}

}  // namespace

uint32_t fcs32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            // Reflected form: 0x04C11DB7 bit-reversed is 0xEDB88320.
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint16_t plcp_crc16(const uint8_t* data, size_t len) {
    // The subtlety that makes or breaks this: 802.11 transmits the PLCP
    // header fields LSB-first, but the CRC register itself is MSB-first
    // (non-reflected 0x1021). So bits are fed in LSB-first order into a
    // most-significant-bit-aligned register - it is neither a plain
    // "reflected" nor a plain "non-reflected" CRC in the usual
    // table-driven sense.
    //
    // Solved against the spec's worked example rather than assumed:
    // SIGNAL 0x0A / SERVICE 0x00 / LENGTH 192 must give 0x5B57. Two
    // neighbouring parameterisations are worth knowing because they look
    // just as plausible and are silently wrong:
    //   - reflecting the OUTPUT instead of complementing it gives 0x1525
    //     (this is the reference prototype's bug, reproduced exactly)
    //   - feeding bits MSB-first gives 0x05C0
    // Either decodes synthetic frames happily and rejects 100% of real
    // ones, which reads as an RF problem rather than a logic bug.
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        for (int b = 0; b < 8; ++b) {
            uint16_t bit = uint16_t((data[i] >> b) & 1u);  // LSB-first
            uint16_t fb = uint16_t(((crc >> 15) & 1u) ^ bit);
            crc = uint16_t(crc << 1);
            if (fb) crc ^= 0x1021u;
        }
    }
    return uint16_t(~crc);
}

std::vector<uint8_t> descramble_bits(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        uint8_t t4 = (i >= 4) ? bits[i - 4] : 0;
        uint8_t t7 = (i >= 7) ? bits[i - 7] : 0;
        out[i] = uint8_t((bits[i] ^ t4 ^ t7) & 1u);
    }
    return out;
}

std::vector<uint8_t> bits_to_bytes_lsb_first(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> bytes(bits.size() / 8);
    for (size_t i = 0; i < bytes.size(); ++i) {
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) {
            if (bits[i * 8 + size_t(b)] & 1u) v |= uint8_t(1u << b);
        }
        bytes[i] = v;
    }
    return bytes;
}

namespace {
// The 802.11b PLCP header's SIGNAL/SERVICE/LENGTH octets are
// transmitted LSB-first (matching bits_to_bytes_lsb_first()'s own
// packing, which is what `bytes` here already went through), but the
// CRC-16 field specifically is transmitted MSB-first PER OCTET - a
// genuine, documented asymmetry in the standard (the CRC shift
// register naturally shifts its own MSB out first), not a receiver
// quirk. bits_to_bytes_lsb_first() has no way to know a given byte
// needs the opposite convention, so bytes[4]/bytes[5] arrive with
// each byte's own bit order backwards relative to every other field -
// reversing them here undoes exactly that, and only that: byte ORDER
// (bytes[4] as the high byte) was already correct, confirmed by
// real-hardware capture (see tests/test_wifi_frame.cpp's
// plcp_header_crc_byte_order_confirmed_against_real_capture - this
// was invisible in the original synthetic round-trip test because
// that test built its own "transmitted" CRC bytes using this same
// convention, rather than the spec's, so it could only ever validate
// self-consistency, not correctness - live capture against 72
// consecutive real DSSS headers (differing SIGNAL/LENGTH values,
// so no risk of a single coincidental match) confirmed this bit
// reversal, and only this, makes plcp_crc16()'s own already-spec-
// validated computation match the transmitted CRC exactly, every
// time bar one plausibly-corrupted frame).
uint8_t reverse_bits8(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & (1u << i)) r |= uint8_t(1u << (7 - i));
    }
    return r;
}
}  // namespace

std::optional<PlcpHeader> parse_plcp_header(const uint8_t* bytes, size_t len) {
    if (len < 6) return std::nullopt;
    PlcpHeader h;
    h.signal = bytes[0];
    h.service = bytes[1];
    h.length_us = uint16_t(uint16_t(bytes[2]) | (uint16_t(bytes[3]) << 8));
    // Byte order MSB-first (bytes[4] is the high byte) as before, but
    // each byte's own bits reversed first - see the comment above.
    h.crc = uint16_t((uint16_t(reverse_bits8(bytes[4])) << 8) | uint16_t(reverse_bits8(bytes[5])));
    h.crc_valid = (plcp_crc16(bytes, 4) == h.crc);
    return h;
}

std::optional<BeaconInfo> parse_beacon(const uint8_t* mpdu, size_t len) {
    if (len < kMacHeaderLen + kFixedParamsLen + kFcsLen) return std::nullopt;

    uint8_t fc0 = mpdu[0];
    uint8_t type = uint8_t((fc0 >> 2) & 0x3u);
    uint8_t subtype = uint8_t((fc0 >> 4) & 0xFu);
    if (type != 0) return std::nullopt;  // management frames only
    if (subtype != kSubtypeBeacon && subtype != kSubtypeProbeResponse) return std::nullopt;

    BeaconInfo info;
    // The FCS covers the whole MPDU except itself, and is stored
    // little-endian at the very end.
    size_t body_len = len - kFcsLen;
    uint32_t want = uint32_t(mpdu[body_len]) | (uint32_t(mpdu[body_len + 1]) << 8) |
                    (uint32_t(mpdu[body_len + 2]) << 16) | (uint32_t(mpdu[body_len + 3]) << 24);
    info.fcs_valid = (fcs32(mpdu, body_len) == want);

    // Address 3 is the BSSID - see the header comment for why Address 2
    // is the wrong choice on multi-BSSID radios.
    info.bssid = format_mac(mpdu + kAddr3Offset);

    const uint8_t* fixed = mpdu + kMacHeaderLen;
    info.beacon_interval_tu = uint16_t(uint16_t(fixed[8]) | (uint16_t(fixed[9]) << 8));
    info.capability = uint16_t(uint16_t(fixed[10]) | (uint16_t(fixed[11]) << 8));

    // Tagged information elements: [tag][len][payload...]
    size_t pos = kMacHeaderLen + kFixedParamsLen;
    while (pos + 2 <= body_len) {
        uint8_t tag = mpdu[pos];
        uint8_t tag_len = mpdu[pos + 1];
        size_t payload = pos + 2;
        if (payload + tag_len > body_len) break;  // truncated / malformed
        if (tag == kIeSsid) {
            // A zero-length or all-zero SSID is a hidden network, which
            // is legitimate - leave the field empty rather than
            // inventing a name.
            bool all_zero = true;
            for (uint8_t i = 0; i < tag_len; ++i) {
                if (mpdu[payload + i] != 0) all_zero = false;
            }
            if (!all_zero) {
                info.ssid.assign(reinterpret_cast<const char*>(mpdu + payload), tag_len);
            }
        } else if (tag == kIeDsParameterSet && tag_len >= 1) {
            info.channel = int(mpdu[payload]);
        }
        pos = payload + tag_len;
    }
    return info;
}

}  // namespace rfmon::wifi
