#include "shortrange/ieee802154.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "shortrange/ble.hpp"
#include "shortrange/ddc.hpp"

namespace rfmon::shortrange::ieee802154 {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSps = 2;  // samples per chip

// Symbol 0 chips c0..c31 (802.15.4-2020 Table 12-1); symbols 1-7 rotate it
// right by 4 chips each, 8-15 repeat 0-7 with odd-indexed chips inverted.
constexpr const char* kSymbol0 = "11011001110000110101001000101110";
int chip(int symbol, int i) {
    int c = kSymbol0[(i - 4 * (symbol & 7) + 128) % 32] - '0';
    return symbol >= 8 && (i & 1) ? c ^ 1 : c;
}

std::vector<std::complex<float>> modulate_chips(const std::vector<uint8_t>& chips) {
    // Half-sine pulses two chips long; even chips on I, odd chips on Q
    // delayed by one chip. Sample 2i is the chip boundary t = i*Tc.
    std::vector<std::complex<float>> x(chips.size() * kSps + 4, {0, 0});
    const float pulse[4] = {0.f, 0.70710678f, 1.f, 0.70710678f};
    for (size_t i = 0; i < chips.size(); ++i) {
        const float a = chips[i] ? 1.f : -1.f;
        for (int k = 0; k < 4; ++k) {
            auto& v = x[i * kSps + size_t(k)];
            if (i % 2 == 0) v += std::complex<float>(a * pulse[k], 0);
            else v += std::complex<float>(0, a * pulse[k]);
        }
    }
    return x;
}

// Phase change across chip interval m (samples S+2m -> S+2m+2); interval m
// depends on chips m-1 and m, so m = 0 (previous symbol's last chip) is unused.
struct Templates {
    std::array<std::array<float, 32>, 16> t{};  // centred, m = 0 zero
    std::array<std::array<float, 32>, 16> ideal{};  // +/-pi/2 (m = 0 zero)
    float norm = 0;
    Templates() {
        for (int s = 0; s < 16; ++s) {
            std::vector<uint8_t> chips;
            for (int rep = 0; rep < 3; ++rep)
                for (int i = 0; i < 32; ++i) chips.push_back(uint8_t(chip(s, i)));
            const auto x = modulate_chips(chips);
            double mean = 0;
            for (int m = 1; m < 32; ++m) {
                const size_t a = size_t(2 * (32 + m)), b = a + 2;
                const float v = std::arg(x[b] * std::conj(x[a]));
                ideal[size_t(s)][size_t(m)] = v > 0 ? float(kPi / 2) : float(-kPi / 2);
                mean += ideal[size_t(s)][size_t(m)];
            }
            mean /= 31;
            double n2 = 0;
            for (int m = 1; m < 32; ++m) {
                t[size_t(s)][size_t(m)] = float(ideal[size_t(s)][size_t(m)] - mean);
                n2 += double(t[size_t(s)][size_t(m)]) * t[size_t(s)][size_t(m)];
            }
            norm = float(std::sqrt(n2));  // equal for every symbol (same chip statistics)
        }
    }
};
const Templates& templates() {
    static const Templates t;
    return t;
}

// Normalised correlation of the 31 usable intervals at sample S with symbol s.
struct Best { int symbol = -1; float score = -1; size_t start = 0; };
Best best_symbol(const std::vector<std::complex<float>>& x, size_t S) {
    Best best;
    if (S + 66 >= x.size()) return best;
    float v[32];
    double mean = 0;
    for (int m = 1; m < 32; ++m) {
        const auto p = mul_conj(x[S + size_t(2 * m + 2)], x[S + size_t(2 * m)]);
        v[m] = fast_atan2(p.imag(), p.real());
        mean += v[m];
    }
    mean /= 31;
    double var = 0;
    for (int m = 1; m < 32; ++m) var += (v[m] - mean) * (v[m] - mean);
    const double denom = std::sqrt(var) * templates().norm;
    if (!(denom > 0)) return best;
    for (int s = 0; s < 16; ++s) {
        double c = 0;
        for (int m = 1; m < 32; ++m) c += templates().t[size_t(s)][size_t(m)] * v[m];
        const float score = float(c / denom);
        if (score > best.score) best = {s, score, S};
    }
    return best;
}
// Best over start offsets -1/0/+1 sample (tracks chip-timing drift).
Best track(const std::vector<std::complex<float>>& x, size_t S) {
    Best best;
    for (int d = -1; d <= 1; ++d) {
        if (S == 0 && d < 0) continue;
        const auto b = best_symbol(x, S + size_t(ptrdiff_t(d)));
        if (b.score > best.score) best = b;
    }
    return best;
}

std::string hex16(uint16_t v) { char b[8]; std::snprintf(b, sizeof b, "0x%04X", v); return b; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint64_t le64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; }

const char* command_name(uint8_t id) {
    static const char* names[] = {"", "Association request", "Association response", "Disassociation notification",
                                  "Data request", "PAN ID conflict notification", "Orphan notification",
                                  "Beacon request", "Coordinator realignment", "GTS request"};
    return id >= 1 && id <= 9 ? names[id] : nullptr;
}
const char* nwk_command_name(uint8_t id) {
    static const char* names[] = {"", "Route request", "Route reply", "Network status", "Leave", "Route record",
                                  "Rejoin request", "Rejoin response", "Link status", "Network report",
                                  "Network update", "End device timeout request", "End device timeout response"};
    return id >= 1 && id <= 12 ? names[id] : nullptr;
}

void parse_zigbee_nwk(Mac& f, const uint8_t* p, size_t n) {
    const uint16_t fc = le16(p);
    const int type = fc & 3, version = fc >> 2 & 0xF;
    f.network = "Zigbee";
    f.fields.push_back({"NWK frame type", type == 0 ? "Data" : type == 1 ? "NWK command" : "Inter-PAN"});
    f.fields.push_back({"NWK protocol version", std::to_string(version) + (version == 2 ? " (Zigbee PRO)" : "")});
    if (type == 3) return;  // inter-PAN carries no NWK addressing
    size_t i = 2;
    if (n < i + 6) { f.fields.push_back({"NWK header", "truncated"}); return; }
    f.nwk_dst = le16(p + i);
    f.nwk_src = le16(p + i + 2);
    f.fields.push_back({"NWK destination", hex16(*f.nwk_dst) + (*f.nwk_dst >= 0xFFFC ? " (broadcast)" : "")});
    f.fields.push_back({"NWK source", hex16(*f.nwk_src)});
    f.fields.push_back({"NWK radius / sequence", std::to_string(p[i + 4]) + " / " + std::to_string(p[i + 5])});
    i += 6;
    if (fc >> 11 & 1) { if (n < i + 8) return; f.nwk_dst_ext = le64(p + i); f.fields.push_back({"NWK destination IEEE", eui64_text(*f.nwk_dst_ext)}); i += 8; }
    if (fc >> 12 & 1) { if (n < i + 8) return; f.nwk_src_ext = le64(p + i); f.fields.push_back({"NWK source IEEE", eui64_text(*f.nwk_src_ext)}); i += 8; }
    if (fc >> 8 & 1) { if (n < i + 1) return; f.fields.push_back({"Multicast control", std::to_string(p[i])}); i += 1; }
    if (fc >> 10 & 1) {
        if (n < i + 2) return;
        const size_t relays = p[i];
        f.fields.push_back({"Source route", std::to_string(relays) + " relays"});
        i += 2 + 2 * relays;
        if (i > n) return;
    }
    f.nwk_security = fc >> 9 & 1;
    if (f.nwk_security) {
        if (n < i + 5) { f.fields.push_back({"NWK security header", "truncated"}); return; }
        const uint8_t sc = p[i];
        static const char* keys[] = {"Data key", "Network key", "Key-transport key", "Key-load key"};
        f.fields.push_back({"NWK security", std::string(keys[sc >> 3 & 3]) + ", frame counter " +
                                                std::to_string(uint32_t(p[i + 1]) | uint32_t(p[i + 2]) << 8 |
                                                               uint32_t(p[i + 3]) << 16 | uint32_t(p[i + 4]) << 24)});
        i += 5;
        if (sc >> 5 & 1) {
            if (n < i + 8) return;
            f.nwk_src_ext = le64(p + i);
            f.fields.push_back({"Security source IEEE", eui64_text(*f.nwk_src_ext)});
            i += 8;
        }
        if ((sc >> 3 & 3) == 1 && n > i) { f.fields.push_back({"Key sequence", std::to_string(p[i])}); ++i; }
        f.fields.push_back({"NWK payload", "encrypted (" + std::to_string(n - std::min(n, i)) + " bytes incl. MIC); not decrypted"});
        return;
    }
    if (type == 1 && n > i) {
        const char* name = nwk_command_name(p[i]);
        f.fields.push_back({"NWK command", name ? name : "id " + std::to_string(p[i])});
        if (p[i] == 8) f.role_hint = "Router (link status)";
    }
    if (n > i) f.fields.push_back({"NWK payload (unencrypted)", ble::hex(p + i, n - i)});
}

bool looks_like_zigbee(const uint8_t* p, size_t n) {
    if (n < 2) return false;
    const uint16_t fc = le16(p);
    const int type = fc & 3, version = fc >> 2 & 0xF, discover = fc >> 6 & 3;
    return (type == 0 || type == 1 || type == 3) && version >= 1 && version <= 3 && discover <= 1;
}
const char* lowpan_dispatch(uint8_t b) {
    if (b == 0x41) return "IPv6 (uncompressed)";
    if ((b & 0xE0) == 0x60) return "IPHC compressed IPv6";
    if ((b & 0xC0) == 0x80) return "Mesh header";
    if ((b & 0xF8) == 0xC0) return "First fragment";
    if ((b & 0xF8) == 0xE0) return "Subsequent fragment";
    return nullptr;
}

}  // namespace

int channel_index(double f) {
    for (int c = 11; c <= 26; ++c)
        if (std::abs(f - channel_frequency(c)) <= 0.5e6) return c;
    return -1;
}
double channel_frequency(int c) { return c >= 11 && c <= 26 ? 2405e6 + 5e6 * (c - 11) : 0; }

uint16_t fcs(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0x8408) : uint16_t(crc >> 1);
    }
    return crc;
}

std::string eui64_text(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", unsigned(v >> 56 & 255), unsigned(v >> 48 & 255),
                  unsigned(v >> 40 & 255), unsigned(v >> 32 & 255), unsigned(v >> 24 & 255), unsigned(v >> 16 & 255),
                  unsigned(v >> 8 & 255), unsigned(v & 255));
    return b;
}
std::string Address::text() const {
    if (mode == 2) return hex16(short_addr);
    if (mode == 3) return eui64_text(ext);
    return "none";
}

std::vector<Frame> demodulate(const std::vector<std::complex<float>>& x, int channel, DemodStats* stats) {
    std::vector<Frame> frames;
    if (x.size() < 64 * 12) return frames;
    const auto& T = templates();
    // Candidate preamble starts: symbol 0 correlation above 0.8 on two
    // consecutive symbols, scanned at both chip-sampling phases.
    // Phase change across every chip interval, for both chip-sampling
    // phases: v[ph][i] covers samples ph+2i -> ph+2i+2.
    const size_t nint = (x.size() - 4) / kSps;
    std::vector<float> v[2];
    std::vector<double> sum[2], sum2[2];
    for (int ph = 0; ph < 2; ++ph) {
        v[ph].resize(nint);
        sum[ph].assign(nint + 1, 0);
        sum2[ph].assign(nint + 1, 0);
        for (size_t i = 0; i < nint; ++i) {
            const auto p = mul_conj(x[size_t(ph) + 2 * i + 2], x[size_t(ph) + 2 * i]);
            v[ph][i] = fast_atan2(p.imag(), p.real());
            sum[ph][i + 1] = sum[ph][i] + v[ph][i];
            sum2[ph][i + 1] = sum2[ph][i] + double(v[ph][i]) * v[ph][i];
        }
    }
    // Normalised correlation with symbol 0 for the symbol starting at chip i.
    auto score0 = [&](int ph, size_t i) {
        if (i + 32 > nint) return 0.f;
        const float* w = v[ph].data() + i;
        double c = 0;
        for (int m = 1; m < 32; ++m) c += T.t[0][size_t(m)] * w[m];
        const double s1 = sum[ph][i + 32] - sum[ph][i + 1], s2 = sum2[ph][i + 32] - sum2[ph][i + 1];
        const double var = s2 - s1 * s1 / 31;
        return var > 0 ? float(c / (std::sqrt(var) * T.norm)) : 0.f;
    };
    size_t resume = 0;  // never re-detect inside a frame already handled
    for (size_t i = 0; 2 * i + 64 * 12 < x.size(); ++i) {
        if (2 * i < resume) continue;
        size_t found = SIZE_MAX;
        for (int ph = 0; ph < 2 && found == SIZE_MAX; ++ph)
            if (score0(ph, i) > 0.8f && score0(ph, i + 32) > 0.8f) found = size_t(ph) + 2 * i;
        if (found == SIZE_MAX) continue;
        // Walk the preamble to the SFD (symbols 7 then 10).
        size_t S = found;
        size_t preamble_start = found;
        bool sfd = false;
        std::vector<std::pair<size_t, int>> preamble;
        for (int k = 0; k < 12 && S + 64 * 4 < x.size(); ++k) {
            const auto b = track(x, S);
            if (b.score < 0.5f) break;
            if (b.symbol == 0) { preamble.push_back({b.start, 0}); S = b.start + 64; continue; }
            if (b.symbol == 7) {
                const auto b2 = track(x, b.start + 64);
                if (b2.symbol == 10 && b2.score >= 0.5f) { sfd = true; S = b2.start + 64; }
            }
            break;
        }
        if (!sfd) { resume = found + 64; continue; }
        if (stats) ++stats->syncs;
        // PHR then PSDU, low nibble first.
        auto read_byte = [&](bool& ok) {
            int nib[2];
            for (int h = 0; h < 2; ++h) {
                const auto b = track(x, S);
                if (b.symbol < 0 || b.score < 0.3f) { ok = false; return uint8_t(0); }
                nib[h] = b.symbol;
                S = b.start + 64;
            }
            return uint8_t(nib[0] | nib[1] << 4);
        };
        bool ok = true;
        const size_t phr_at = S;
        const size_t len = read_byte(ok) & 0x7F;
        std::vector<uint8_t> psdu;
        if (ok && len >= 5)
            for (size_t i = 0; i < len && ok; ++i) {
                if (S + 70 >= x.size()) { ok = false; break; }
                psdu.push_back(read_byte(ok));
            }
        if (!ok || len < 5 || psdu.size() != len || fcs(psdu.data(), len - 2) != le16(psdu.data() + len - 2)) {
            if (stats) ++stats->fcs_failures;
            resume = phr_at;
            continue;
        }
        Frame f;
        f.channel = channel;
        f.frequency_hz = channel_frequency(channel);
        f.sample = phr_at;
        f.psdu = std::move(psdu);
        f.power_db = power_db(x, preamble_start, S);
        // Carrier offset: mean phase-change error over the preamble symbols.
        double err = 0;
        int count = 0;
        for (const auto& [at, sym] : preamble)
            for (int m = 1; m < 32; ++m) {
                const auto p = x[at + size_t(2 * m + 2)] * std::conj(x[at + size_t(2 * m)]);
                err += std::arg(p) - T.ideal[size_t(sym)][size_t(m)];
                ++count;
            }
        f.cfo_hz = count ? err / count / (2 * kPi) * kChipRate : 0;
        frames.push_back(std::move(f));
        resume = S;
    }
    return frames;
}

Mac parse(const std::vector<uint8_t>& psdu) {
    Mac f;
    if (psdu.size() < 5) return f;
    const uint8_t* p = psdu.data();
    const size_t n = psdu.size() - 2;  // without FCS
    const uint16_t fc = le16(p);
    f.type = fc & 7;
    static const char* types[] = {"Beacon", "Data", "Acknowledgment", "MAC command", "Reserved", "Multipurpose", "Fragment", "Extended"};
    f.type_name = types[f.type];
    f.summary = f.type_name;
    f.security = fc >> 3 & 1;
    f.pending = fc >> 4 & 1;
    f.ack_request = fc >> 5 & 1;
    f.pan_compression = fc >> 6 & 1;
    const bool seq_suppressed = fc >> 8 & 1, ie_present = fc >> 9 & 1;
    f.dst.mode = fc >> 10 & 3;
    f.version = fc >> 12 & 3;
    f.src.mode = fc >> 14 & 3;
    f.fields.push_back({"Frame type", f.type_name});
    f.fields.push_back({"Frame version", std::to_string(f.version) + (f.version == 0 ? " (2003)" : f.version == 1 ? " (2006)" : f.version == 2 ? " (2015+)" : "")});
    std::string flags;
    if (f.security) flags += "security ";
    if (f.pending) flags += "frame-pending ";
    if (f.ack_request) flags += "ack-request ";
    if (f.pan_compression) flags += "PAN-ID-compression ";
    if (ie_present) flags += "IEs ";
    f.fields.push_back({"Flags", flags.empty() ? "none" : flags});
    if (f.type > 3) { f.valid = true; return f; }
    size_t i = 2;
    if (!(f.version == 2 && seq_suppressed)) {
        if (n < i + 1) return f;
        f.sequence = p[i++];
        f.fields.push_back({"Sequence number", std::to_string(*f.sequence)});
    }
    if (f.type == 2) { f.valid = true; return f; }  // ack: nothing more
    // PAN ID presence: 2003/2006 rules, or the 2015 table.
    bool dst_pan = f.dst.mode != 0, src_pan = f.src.mode != 0 && !f.pan_compression;
    if (f.version == 2) {
        const int d = f.dst.mode, s = f.src.mode;
        if (d == 0 && s == 0) { dst_pan = f.pan_compression; src_pan = false; }
        else if (s == 0) { dst_pan = !f.pan_compression; src_pan = false; }
        else if (d == 0) { dst_pan = false; src_pan = !f.pan_compression; }
        else if (d == 3 && s == 3) { dst_pan = !f.pan_compression; src_pan = false; }
        else { dst_pan = true; src_pan = !f.pan_compression; }
    }
    auto read_addr = [&](Address& a) {
        if (a.mode == 2) { if (n < i + 2) return false; a.short_addr = le16(p + i); i += 2; }
        else if (a.mode == 3) { if (n < i + 8) return false; a.ext = le64(p + i); i += 8; }
        return true;
    };
    if (dst_pan) { if (n < i + 2) return f; f.dst_pan = le16(p + i); i += 2; }
    if (!read_addr(f.dst)) return f;
    if (src_pan) { if (n < i + 2) return f; f.src_pan = le16(p + i); i += 2; }
    else if (f.src.mode && f.dst_pan) f.src_pan = f.dst_pan;
    if (!read_addr(f.src)) return f;
    if (f.dst_pan) f.fields.push_back({"Destination PAN", hex16(*f.dst_pan) + (*f.dst_pan == 0xFFFF ? " (broadcast)" : "")});
    if (f.dst.mode) f.fields.push_back({"Destination", f.dst.text() + (f.dst.mode == 2 && f.dst.short_addr == 0xFFFF ? " (broadcast)" : "")});
    if (f.src_pan) f.fields.push_back({"Source PAN", hex16(*f.src_pan)});
    if (f.src.mode) f.fields.push_back({"Source", f.src.text()});
    if (f.security) {
        if (n < i + 1) return f;
        const uint8_t sc = p[i++];
        const int level = sc & 7, key_mode = sc >> 3 & 3;
        std::string s = "level " + std::to_string(level) + ", key id mode " + std::to_string(key_mode);
        if (!(f.version == 2 && (sc >> 5 & 1))) {
            if (n < i + 4) return f;
            s += ", frame counter " + std::to_string(uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16 | uint32_t(p[i + 3]) << 24);
            i += 4;
        }
        i += key_mode == 1 ? 1 : key_mode == 2 ? 5 : key_mode == 3 ? 9 : 0;
        f.fields.push_back({"MAC security", s});
        f.fields.push_back({"MAC payload", "secured (" + std::to_string(n - std::min(n, i)) + " bytes incl. MIC); not decrypted"});
        f.valid = i <= n;
        return f;
    }
    if (ie_present) {
        f.fields.push_back({"Information elements", "present; payload not parsed | " + ble::hex(p + std::min(i, n), n - std::min(i, n))});
        f.valid = true;
        return f;
    }
    const uint8_t* q = p + i;
    const size_t m = n - std::min(i, n);
    if (f.type == 0) {
        f.is_beacon = true;
        if (m < 4) return f;
        const uint16_t sf = le16(q);
        f.pan_coordinator = sf >> 14 & 1;
        f.association_permit = sf >> 15 & 1;
        f.fields.push_back({"Superframe", "beacon order " + std::to_string(sf & 15) + ", superframe order " + std::to_string(sf >> 4 & 15) +
                                              (f.pan_coordinator ? ", PAN coordinator" : "") +
                                              (f.association_permit ? ", association permitted" : ", association not permitted")});
        size_t j = 2;
        const size_t gts = q[j] & 7;
        j += 1 + (gts ? 1 + 3 * gts : 0);
        if (m < j + 1) return f;
        const size_t shorts = q[j] & 7, exts = q[j] >> 4 & 7;
        j += 1 + 2 * shorts + 8 * exts;
        if (shorts + exts) f.fields.push_back({"Pending addresses", std::to_string(shorts) + " short, " + std::to_string(exts) + " extended"});
        if (j > m) return f;
        f.role_hint = f.pan_coordinator ? "PAN coordinator" : "Router (sends beacons)";
        if (m - j >= 15 && q[j] == 0) {
            f.network = "Zigbee";
            f.stack_profile = q[j + 1] & 15;
            f.zigbee_ext_pan = le64(q + j + 3);
            f.fields.push_back({"Zigbee stack profile / protocol", std::to_string(*f.stack_profile) +
                                                                     (*f.stack_profile == 2 ? " (PRO)" : "") + " / " + std::to_string(q[j + 1] >> 4)});
            f.fields.push_back({"Router / end-device capacity", std::string(q[j + 2] >> 2 & 1 ? "yes" : "no") + " / " + (q[j + 2] >> 7 & 1 ? "yes" : "no")});
            f.fields.push_back({"Device depth", std::to_string(q[j + 2] >> 3 & 15)});
            f.fields.push_back({"Extended PAN ID", eui64_text(*f.zigbee_ext_pan)});
        } else if (m > j) f.fields.push_back({"Beacon payload", ble::hex(q + j, m - j)});
        f.summary = std::string("Beacon") + (f.network.empty() ? "" : " (" + f.network + ")");
        f.valid = true;
        return f;
    }
    if (f.type == 3) {
        if (m < 1) return f;
        const char* name = command_name(q[0]);
        f.summary = std::string("MAC command: ") + (name ? name : "id " + std::to_string(q[0]));
        f.fields.push_back({"MAC command", name ? name : "id " + std::to_string(q[0])});
        if (q[0] == 1 && m >= 2) {
            const uint8_t cap = q[1];
            f.fields.push_back({"Capability", std::string(cap >> 1 & 1 ? "full-function device" : "reduced-function device") +
                                                  (cap >> 2 & 1 ? ", mains powered" : ", battery") + (cap >> 3 & 1 ? ", receiver on when idle" : "") +
                                                  (cap >> 6 & 1 ? ", security capable" : "") + (cap >> 7 & 1 ? ", requests short address" : "")});
            f.role_hint = cap >> 1 & 1 ? "Joining router-capable device" : "Joining end device";
        }
        if (q[0] == 2 && m >= 4) f.fields.push_back({"Assigned short address / status", hex16(le16(q + 1)) + " / " + std::to_string(q[3])});
        if (q[0] == 4) f.role_hint = "End device (polls its parent)";
        if (m > 1) f.fields.push_back({"Command payload", ble::hex(q + 1, m - 1)});
        f.valid = true;
        return f;
    }
    // Data frame: classify the network layer.
    if (m >= 2 && looks_like_zigbee(q, m)) parse_zigbee_nwk(f, q, m);
    else if (m >= 1 && lowpan_dispatch(q[0])) {
        f.network = "6LoWPAN / Thread";
        f.fields.push_back({"6LoWPAN dispatch", lowpan_dispatch(q[0])});
        f.fields.push_back({"Payload", ble::hex(q, m)});
    } else if (m) f.fields.push_back({"Payload", ble::hex(q, m)});
    f.summary = "Data" + (f.network.empty() ? std::string() : " (" + f.network + ")");
    f.valid = true;
    return f;
}

std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& psdu) {
    std::vector<int> symbols(8, 0);  // preamble: four zero bytes
    symbols.push_back(0x7);          // SFD 0xA7, low nibble first
    symbols.push_back(0xA);
    const uint8_t phr = uint8_t(psdu.size() & 0x7F);
    for (uint8_t b : std::vector<uint8_t>{phr}) { symbols.push_back(b & 15); symbols.push_back(b >> 4); }
    for (uint8_t b : psdu) { symbols.push_back(b & 15); symbols.push_back(b >> 4); }
    std::vector<uint8_t> chips;
    for (int s : symbols)
        for (int i = 0; i < 32; ++i) chips.push_back(uint8_t(chip(s, i)));
    auto x = modulate_chips(chips);
    std::vector<std::complex<float>> out(64, {0, 0});
    out.insert(out.end(), x.begin(), x.end());
    out.insert(out.end(), 64, {0, 0});
    return out;
}

}  // namespace rfmon::shortrange::ieee802154
