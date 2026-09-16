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

namespace {
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
void add_label(std::string& out, const std::string& value) {
    if (!out.empty()) out += ", ";
    out += value;
}
std::string selector_name(const uint8_t* p, bool akm, bool wpa) {
    bool known_oui = p[0] == 0 && p[1] == (wpa ? 0x50 : 0x0f) && p[2] == (wpa ? 0xf2 : 0xac);
    if (known_oui) {
        if (akm) {
            switch (p[3]) {
                case 1: return "802.1X";
                case 2: return "PSK";
                case 3: if (!wpa) return "FT-802.1X"; break;
                case 4: if (!wpa) return "FT-PSK"; break;
                case 5: if (!wpa) return "802.1X-SHA256"; break;
                case 6: if (!wpa) return "PSK-SHA256"; break;
                case 8: if (!wpa) return "SAE (WPA3-Personal)"; break;
                case 9: if (!wpa) return "FT-SAE (WPA3-Personal)"; break;
                case 11: if (!wpa) return "802.1X Suite-B"; break;
                case 12: if (!wpa) return "802.1X Suite-B-192"; break;
                case 13: if (!wpa) return "FT-802.1X-SHA384"; break;
                case 18: if (!wpa) return "OWE (Enhanced Open)"; break;
            }
        } else {
            switch (p[3]) {
                case 0: return "Use group cipher";
                case 1: return "WEP-40";
                case 2: return "TKIP";
                case 4: return "CCMP-128";
                case 5: return "WEP-104";
                case 8: if (!wpa) return "GCMP-128"; break;
                case 9: if (!wpa) return "GCMP-256"; break;
                case 10: if (!wpa) return "CCMP-256"; break;
            }
        }
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "Unknown %02x:%02x:%02x:%u", p[0], p[1], p[2], p[3]);
    return buf;
}
// WPA/RSN common mandatory fields. Optional RSN capabilities, PMKID list and
// group-management suite are length checked. No inference from the privacy bit alone.
bool parse_security(const uint8_t* p, size_t n, bool wpa, std::string& auth,
                    std::string& cipher, std::string& pmf) {
    if (n < 8 || le16(p) != 1) return false;
    std::string group = selector_name(p + 2, false, wpa);
    size_t pos = 6;
    auto suites = [&](bool akm, std::string& out) {
        if (n - pos < 2) return false;
        size_t count = le16(p + pos); pos += 2;
        if (!count || count > (n - pos) / 4) return false;
        for (size_t i = 0; i < count; ++i, pos += 4) add_label(out, selector_name(p + pos, akm, wpa));
        return true;
    };
    std::string pairwise, keys;
    if (!suites(false, pairwise) || !suites(true, keys)) return false;
    uint16_t caps = 0;
    if (pos < n) {
        if (n - pos < 2) return false;
        caps = le16(p + pos); pos += 2;
    }
    if (!wpa && pos < n) {
        if (n - pos < 2) return false;
        size_t count = le16(p + pos); pos += 2;
        if (count > (n - pos) / 16) return false;
        pos += count * 16;
        if (pos < n) {
            if (n - pos != 4) return false;
            pos += 4;
        }
    }
    if (pos != n) return false;
    auth = (wpa ? "WPA: " : "RSN: ") + keys;
    cipher = pairwise + " (group: " + group + ")";
    if (!wpa) {
        pmf = (caps & 0x40) ? ((caps & 0x80) ? "Required" : "Invalid PMF flags")
                            : ((caps & 0x80) ? "Capable" : "Not advertised");
    }
    return true;
}
void parse_wps(const std::vector<uint8_t>& bytes, BeaconInfo& info) {
    size_t pos = 0;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 4) { info.ies_complete = false; break; }
        uint16_t tag = be16(bytes.data() + pos), len = be16(bytes.data() + pos + 2);
        pos += 4;
        if (len > bytes.size() - pos) { info.ies_complete = false; break; }
        std::string* target = nullptr;
        size_t limit = 32;
        switch (tag) {
            case 0x1021: target = &info.wps_manufacturer; limit = 64; break;
            case 0x1023: target = &info.wps_model_name; break;
            case 0x1024: target = &info.wps_model_number; break;
            case 0x1011: target = &info.wps_device_name; break;
        }
        if (target) {
            if (len <= limit) target->assign(reinterpret_cast<const char*>(bytes.data() + pos), len);
            else info.ies_complete = false;
        }
        pos += len;
    }
}
} // namespace

std::string display_text(const std::string& bytes) {
    std::string out;
    for (unsigned char c : bytes) {
        if (c >= 32 && c < 127 && c != '\\') out += char(c);
        else {
            char escaped[5];
            std::snprintf(escaped, sizeof(escaped), "\\x%02X", c);
            out += escaped;
        }
    }
    return out;
}

std::optional<BeaconInfo> parse_beacon(const uint8_t* mpdu, size_t len) {
    if (len < kMacHeaderLen + kFixedParamsLen + kFcsLen) return std::nullopt;

    uint8_t fc0 = mpdu[0];
    uint8_t type = uint8_t((fc0 >> 2) & 0x3u);
    uint8_t subtype = uint8_t((fc0 >> 4) & 0xFu);
    if (type != 0) return std::nullopt;  // management frames only
    if (subtype != kSubtypeBeacon && subtype != kSubtypeProbeResponse) return std::nullopt;

    BeaconInfo info;
    info.frame_source = subtype == kSubtypeBeacon ? "Beacon" : "Probe response";
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
    std::vector<uint8_t> wps_bytes;
    bool security_seen = false;
    int ht_channel = 0;
    size_t pos = kMacHeaderLen + kFixedParamsLen;
    while (pos + 2 <= body_len) {
        uint8_t tag = mpdu[pos];
        uint8_t tag_len = mpdu[pos + 1];
        size_t payload = pos + 2;
        if (payload + tag_len > body_len) { info.ies_complete = false; break; }
        if (tag == kIeSsid) {
            if (tag_len > 32) { info.ies_complete = false; pos = payload + tag_len; continue; }
            info.ssid_present = true;
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
        } else if (tag == kIeDsParameterSet) {
            if (tag_len == 1 && mpdu[payload] >= 1 && mpdu[payload] <= 14) {
                info.channel = int(mpdu[payload]);
                info.channel_source = "DS Parameter Set";
            } else info.ies_complete = false;
        } else if (tag == 61) {
            if (tag_len == 22 && mpdu[payload] != 0) ht_channel = mpdu[payload];
            else info.ies_complete = false;
        } else if (tag == 45 || tag == 191) {
            if (tag_len == (tag == 45 ? 26 : 12)) add_label(info.standards, tag == 45 ? "HT (802.11n)" : "VHT (802.11ac)");
            else info.ies_complete = false;
        } else if (tag == 255 && tag_len > 0 && mpdu[payload] == 35) {
            // HE MAC(6), PHY(11), and minimum MCS/NSS(4), plus extension ID.
            if (tag_len >= 22) add_label(info.standards, "HE (802.11ax)");
            else info.ies_complete = false;
        } else if (tag == 48 || (tag == 221 && tag_len >= 4 &&
                   mpdu[payload] == 0 && mpdu[payload+1] == 0x50 && mpdu[payload+2] == 0xf2 && mpdu[payload+3] == 1)) {
            bool wpa = tag == 221;
            security_seen = true;
            std::string auth, cipher, pmf;
            size_t skip = wpa ? 4 : 0;
            if (parse_security(mpdu + payload + skip, tag_len - skip, wpa, auth, cipher, pmf)) {
                add_label(info.security, auth);
                add_label(info.ciphers, cipher);
                if (!pmf.empty()) info.pmf = pmf;
            } else {
                info.ies_complete = false;
                add_label(info.security, wpa ? "WPA (malformed)" : "RSN (malformed)");
            }
        } else if (tag == 221 && tag_len >= 4 && mpdu[payload] == 0 &&
                   mpdu[payload+1] == 0x50 && mpdu[payload+2] == 0xf2 && mpdu[payload+3] == 4) {
            info.wps_present = true;
            // WPS attributes can span consecutive vendor IEs; concatenate before TLV parsing.
            wps_bytes.insert(wps_bytes.end(), mpdu + payload + 4, mpdu + payload + tag_len);
        }
        pos = payload + tag_len;
    }
    if (pos != body_len) info.ies_complete = false;
    if (!info.channel && ht_channel) { info.channel = ht_channel; info.channel_source = "HT Operation"; }
    if (info.wps_present) parse_wps(wps_bytes, info);
    if (!security_seen) {
        info.security = !info.ies_complete ? "Unknown (incomplete IEs)" :
                        ((info.capability & 0x10) ? "Privacy set (legacy/unknown)" : "Open (no privacy advertised)");
    }
    return info;
}

}  // namespace rfmon::wifi
