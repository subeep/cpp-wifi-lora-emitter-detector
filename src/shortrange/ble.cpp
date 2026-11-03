#include "shortrange/ble.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>

#include "shortrange/assigned_numbers.hpp"
#include "shortrange/ddc.hpp"

namespace rfmon::shortrange::ble {
namespace {

constexpr int kSps = 4;
constexpr double kPi = 3.14159265358979323846;

uint8_t reverse8(uint8_t v) {
    v = uint8_t((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = uint8_t((v & 0xCC) >> 2 | (v & 0x33) << 2);
    return uint8_t((v & 0xAA) >> 1 | (v & 0x55) << 1);
}
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c == 0) break;
        s += (c >= 32 && c != 127) ? char(c) : '?';  // UTF-8 bytes pass through
    }
    return s;
}
std::string uuid16_text(uint16_t u) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%04X", u);
    const char* n = uuid16_name(u);
    return n ? std::string(b) + " " + n : std::string(b);
}
std::string uuid128_text(const uint8_t* p) {  // little-endian on air
    char b[40];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", p[15], p[14], p[13],
                  p[12], p[11], p[10], p[9], p[8], p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    return b;
}
std::string company_text(uint16_t id) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%04X", id);
    const char* n = company_name(id);
    return n ? n : std::string("Unknown company ") + b;
}
void add_tag(Advert& a, const std::string& t) {
    if (std::find(a.tags.begin(), a.tags.end(), t) == a.tags.end()) a.tags.push_back(t);
}

const char* apple_type(uint8_t t) {
    switch (t) {
    case 0x01: return "Apple: unknown 0x01";
    case 0x03: return "AirPrint";
    case 0x05: return "AirDrop";
    case 0x06: return "HomeKit";
    case 0x07: return "Proximity pairing (AirPods / Beats)";
    case 0x08: return "Hey Siri";
    case 0x09: return "AirPlay target";
    case 0x0A: return "AirPlay source";
    case 0x0B: return "Magic Switch";
    case 0x0C: return "Handoff";
    case 0x0D: return "Instant Hotspot target";
    case 0x0E: return "Instant Hotspot source";
    case 0x0F: return "Nearby action";
    case 0x10: return "Nearby info";
    case 0x12: return "Find My (offline finding)";
    default: return nullptr;
    }
}

void manufacturer(Advert& a, const std::vector<uint8_t>& d) {
    if (d.size() < 2) { a.fields.push_back({"Manufacturer data", "truncated"}); return; }
    const uint16_t id = le16(d.data());
    a.company_id = id;
    a.company = company_text(id);
    a.fields.push_back({"Manufacturer", a.company});
    const uint8_t* p = d.data() + 2;
    const size_t n = d.size() - 2;
    if (id == 0x004C && n >= 23 && p[0] == 0x02 && p[1] == 0x15) {
        add_tag(a, "iBeacon");
        a.fields.push_back({"iBeacon UUID", uuid128_text(std::array<uint8_t, 16>{p[17], p[16], p[15], p[14], p[13], p[12], p[11], p[10],
                                                                                     p[9], p[8], p[7], p[6], p[5], p[4], p[3], p[2]}.data())});
        a.fields.push_back({"iBeacon major / minor", std::to_string(p[18] << 8 | p[19]) + " / " + std::to_string(p[20] << 8 | p[21])});
        a.fields.push_back({"iBeacon measured power", std::to_string(int8_t(p[22])) + " dBm at 1 m"});
        return;
    }
    if (id == 0x004C) {
        // Continuity: a sequence of [type][length][payload].
        for (size_t i = 0; i + 1 < n;) {
            const uint8_t t = p[i], len = p[i + 1];
            if (i + 2 + len > n) { a.fields.push_back({"Apple Continuity", "truncated element"}); break; }
            const char* name = apple_type(t);
            char label[48];
            std::snprintf(label, sizeof label, "Apple Continuity 0x%02X", t);
            a.fields.push_back({label, std::string(name ? name : "unrecognised type") + " | " + hex(p + i + 2, len)});
            if (name) add_tag(a, t == 0x12 ? "Find My" : name);
            i += 2 + len;
        }
        return;
    }
    if (id == 0x0006 && n >= 1) {
        if (p[0] == 0x03) add_tag(a, "Swift Pair");
        if (p[0] == 0x01) add_tag(a, "Microsoft CDP");
        a.fields.push_back({"Microsoft scenario", p[0] == 0x03 ? "Swift Pair" : p[0] == 0x01 ? "Connected Devices Platform beacon" : "type " + std::to_string(p[0])});
    }
    a.fields.push_back({"Manufacturer payload", hex(p, n)});
}

void service_data16(Advert& a, const std::vector<uint8_t>& d) {
    if (d.size() < 2) { a.fields.push_back({"Service data", "truncated"}); return; }
    const uint16_t u = le16(d.data());
    const uint8_t* p = d.data() + 2;
    const size_t n = d.size() - 2;
    a.fields.push_back({"Service data UUID", uuid16_text(u)});
    if (u == 0xFFFA) {
        add_tag(a, "Remote ID");
        // ASTM F3411: application code 0x0D, message counter, then one
        // 25-byte message (legacy advertising) or a message pack (type 0xF).
        if (n >= 2 + 25 && p[0] == 0x0D) {
            a.fields.push_back({"Remote ID message counter", std::to_string(p[1])});
            const uint8_t* m = p + 2;
            if ((m[0] >> 4) == 0xF && n >= 2 + 3) {
                const size_t size = m[1], count = m[2];
                a.fields.push_back({"Remote ID message pack", std::to_string(count) + " messages of " + std::to_string(size) + " bytes"});
                for (size_t i = 0; size == 25 && i < count && 2 + 3 + (i + 1) * 25 <= n; ++i) {
                    std::array<uint8_t, 25> msg{};
                    std::copy(m + 3 + i * 25, m + 3 + (i + 1) * 25, msg.begin());
                    a.remote_id_messages.push_back(msg);
                }
            } else {
                std::array<uint8_t, 25> msg{};
                std::copy(m, m + 25, msg.begin());
                a.remote_id_messages.push_back(msg);
            }
        } else a.fields.push_back({"Remote ID", "unexpected layout | " + hex(p, n)});
        return;
    }
    if (u == 0xFEAA && n >= 1) {
        add_tag(a, "Eddystone");
        const char* f = p[0] == 0x00 ? "UID" : p[0] == 0x10 ? "URL" : p[0] == 0x20 ? "TLM (telemetry)" : p[0] == 0x30 ? "EID" : "unknown frame";
        a.fields.push_back({"Eddystone frame", f});
        if (p[0] == 0x00 && n >= 18) a.fields.push_back({"Eddystone namespace / instance", hex(p + 2, 10) + " / " + hex(p + 12, 6)});
        if (p[0] == 0x10 && n >= 3) {
            static const char* schemes[] = {"http://www.", "https://www.", "http://", "https://"};
            static const char* tails[] = {".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/",
                                          ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov"};
            std::string url = p[2] < 4 ? schemes[p[2]] : "?";
            for (size_t i = 3; i < n; ++i) url += p[i] < 14 ? tails[p[i]] : std::string(1, char(p[i] >= 32 && p[i] < 127 ? p[i] : '?'));
            a.fields.push_back({"Eddystone URL", url});
        }
    }
    if (u == 0xFE2C) {
        add_tag(a, "Google Fast Pair");
        if (n == 3) a.fields.push_back({"Fast Pair model ID", hex(p, 3)});
    }
    if (u == 0xFD6F) add_tag(a, "Exposure Notification");
    a.fields.push_back({"Service data payload", hex(p, n)});
}

void describe_ad(Advert& a, const AdStructure& s) {
    const auto& d = s.data;
    const char* type_name = ad_type_name(s.type);
    char fallback[32];
    std::snprintf(fallback, sizeof fallback, "AD type 0x%02X", s.type);
    const std::string label = type_name ? type_name : fallback;
    switch (s.type) {
    case 0x01: {
        if (d.empty()) break;
        std::string f;
        static const char* bits[] = {"LE limited discoverable", "LE general discoverable", "BR/EDR not supported",
                                     "LE + BR/EDR (controller)", "LE + BR/EDR (host)"};
        for (int i = 0; i < 5; ++i)
            if (d[0] >> i & 1) f += std::string(f.empty() ? "" : ", ") + bits[i];
        a.fields.push_back({"Flags", f.empty() ? "none" : f});
        return;
    }
    case 0x02: case 0x03:
        for (size_t i = 0; i + 1 < d.size(); i += 2) {
            a.services.push_back(uuid16_text(le16(&d[i])));
            a.fields.push_back({label, a.services.back()});
        }
        return;
    case 0x04: case 0x05:
        for (size_t i = 0; i + 3 < d.size(); i += 4) {
            char b[16];
            std::snprintf(b, sizeof b, "0x%08X", le32(&d[i]));
            a.services.push_back(b);
            a.fields.push_back({label, b});
        }
        return;
    case 0x06: case 0x07:
        for (size_t i = 0; i + 15 < d.size(); i += 16) {
            a.services.push_back(uuid128_text(&d[i]));
            a.fields.push_back({label, a.services.back()});
        }
        return;
    case 0x08: case 0x09: {
        const auto name = printable(d.data(), d.size());
        if (s.type == 0x09 || a.name.empty()) a.name = name;
        a.fields.push_back({label, name});
        return;
    }
    case 0x0A:
        if (d.size() == 1) {
            a.tx_power_dbm = int8_t(d[0]);
            a.fields.push_back({"TX power level", std::to_string(*a.tx_power_dbm) + " dBm"});
            return;
        }
        break;
    case 0x16: service_data16(a, d); return;
    case 0x19:
        if (d.size() == 2) {
            a.appearance = le16(d.data());
            const char* n = appearance_name(uint16_t(*a.appearance));
            a.fields.push_back({"Appearance", (n ? std::string(n) : "unlisted") + " (" + std::to_string(*a.appearance) + ")"});
            return;
        }
        break;
    case 0x1A:
        if (d.size() == 2) {
            char b[32];
            std::snprintf(b, sizeof b, "%.3f ms", le16(d.data()) * 0.625);
            a.fields.push_back({"Advertising interval", b});
            return;
        }
        break;
    case 0x20: case 0x21: {
        const size_t u = s.type == 0x20 ? 4 : 16;
        if (d.size() >= u) {
            char b[16];
            std::snprintf(b, sizeof b, "0x%08X", u == 4 ? le32(d.data()) : 0);
            a.fields.push_back({"Service data UUID", u == 4 ? std::string(b) : uuid128_text(d.data())});
            a.fields.push_back({"Service data payload", hex(d.data() + u, d.size() - u)});
            return;
        }
        break;
    }
    case 0x24:
        a.fields.push_back({"URI", printable(d.data(), d.size())});
        return;
    case 0xFF: manufacturer(a, d); return;
    default: break;
    }
    a.fields.push_back({label, hex(d.data(), d.size())});
}

Address address(const uint8_t* p, bool random) {
    Address a;
    std::copy(p, p + 6, a.b.begin());
    a.random = random;
    return a;
}

void parse_ad(Advert& a, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        const uint8_t len = p[i];
        if (len == 0) break;  // significant part ends; the rest is padding
        if (i + 1 + len > n) { a.fields.push_back({"Advertising data", "truncated structure | " + hex(p + i, n - i)}); return; }
        AdStructure s;
        s.type = p[i + 1];
        s.data.assign(p + i + 2, p + i + 1 + len);
        describe_ad(a, s);
        a.ad.push_back(std::move(s));
        i += 1 + len;
    }
}

void parse_extended(Advert& a, const uint8_t* p, size_t n, bool tx_random, bool rx_random) {
    if (n < 1) return;
    const size_t ext = p[0] & 0x3F;
    static const char* modes[] = {"Non-connectable, non-scannable", "Connectable", "Scannable", "Reserved"};
    a.fields.push_back({"Advertising mode", modes[p[0] >> 6]});
    if (1 + ext > n) { a.fields.push_back({"Extended header", "truncated"}); return; }
    if (ext) {
        const uint8_t flags = p[1];
        size_t i = 2;
        auto take = [&](size_t k) { const bool ok = i + k <= 1 + ext; const size_t at = i; i += k; return ok ? int(at) : -1; };
        if (flags & 0x01) { int at = take(6); if (at >= 0) a.advertiser = address(p + at, tx_random); }
        if (flags & 0x02) { int at = take(6); if (at >= 0) a.target = address(p + at, rx_random); }
        if (flags & 0x04) { int at = take(1); if (at >= 0) a.fields.push_back({"CTE info", hex(p + at, 1)}); }
        if (flags & 0x08) {
            int at = take(2);
            if (at >= 0) a.fields.push_back({"ADI (data ID / set ID)", std::to_string(le16(p + at) & 0x0FFF) + " / " + std::to_string(p[at + 1] >> 4)});
        }
        if (flags & 0x10) {
            int at = take(3);
            if (at >= 0) {
                const uint32_t v = uint32_t(p[at]) | uint32_t(p[at + 1]) << 8 | uint32_t(p[at + 2]) << 16;
                static const char* phys[] = {"LE 1M", "LE 2M", "LE Coded", "reserved"};
                const unsigned units = (v >> 7 & 1) ? 300 : 30;
                a.fields.push_back({"Aux pointer", "channel " + std::to_string(v & 0x3F) + ", offset " +
                                                       std::to_string(((v >> 8) & 0x1FFF) * units) + " us, PHY " +
                                                       phys[std::min<unsigned>((v >> 21) & 7, 3)] + " (secondary channel not received)"});
            }
        }
        if (flags & 0x20) take(18);
        if (flags & 0x40) { int at = take(1); if (at >= 0) { a.tx_power_dbm = int8_t(p[at]); a.fields.push_back({"TX power", std::to_string(*a.tx_power_dbm) + " dBm"}); } }
        if (i < 1 + ext) a.fields.push_back({"Additional controller data", hex(p + i, 1 + ext - i)});
    }
    parse_ad(a, p + 1 + ext, n - 1 - ext);
}

}  // namespace

std::string hex(const uint8_t* p, size_t n, const char* sep) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i && *sep) s += sep;
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

int adv_channel_index(double f) {
    for (int c : {37, 38, 39})
        if (std::abs(f - adv_channel_frequency(c)) <= 0.5e6) return c;
    return -1;
}
double adv_channel_frequency(int c) { return c == 37 ? 2402e6 : c == 38 ? 2426e6 : c == 39 ? 2480e6 : 0; }

std::vector<uint8_t> bytes_to_bits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; ++i) bits.push_back(b >> i & 1);
    return bits;
}
std::vector<uint8_t> bits_to_bytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out(bits.size() / 8, 0);
    for (size_t i = 0; i < out.size() * 8; ++i) out[i / 8] |= uint8_t((bits[i] & 1) << (i % 8));
    return out;
}
void whiten_bits(std::vector<uint8_t>& bits, int channel) {
    uint8_t lfsr = uint8_t(reverse8(uint8_t(channel)) | 2);
    for (auto& b : bits) {
        if (lfsr & 0x80) {
            lfsr ^= 0x11;
            b ^= 1;
        }
        lfsr = uint8_t(lfsr << 1);
    }
}
std::vector<uint8_t> crc_bits(const std::vector<uint8_t>& pdu_bits) {
    uint32_t r = 0x555555;
    for (uint8_t b : pdu_bits) {
        const uint32_t fb = ((r >> 23) & 1) ^ (b & 1);
        r = (r << 1) & 0xFFFFFF;
        if (fb) r ^= 0x00065B;
    }
    std::vector<uint8_t> out(24);
    for (int i = 0; i < 24; ++i) out[size_t(i)] = uint8_t(r >> (23 - i) & 1);
    return out;
}

std::vector<Packet> demodulate(const std::vector<std::complex<float>>& y, int channel, DemodStats* stats) {
    std::vector<Packet> packets;
    if (y.size() < 200 * kSps) return packets;
    const auto d = discriminate(y, 1);
    std::vector<double> P(d.size() + 1, 0.0);
    for (size_t i = 0; i < d.size(); ++i) P[i + 1] = P[i] + d[i];
    const size_t nsym = (d.size() - kSps) / kSps;
    struct Attempt { size_t sample; bool ok; Packet packet; double amplitude; };
    std::vector<Attempt> attempts;
    std::vector<float> s(nsym);
    std::vector<double> S(nsym + 1);
    for (int phase = 0; phase < kSps; ++phase) {
        // Phase change across each symbol, and a +/-16-symbol running mean
        // as the slicing threshold (it absorbs carrier offset).
        for (size_t k = 0; k < nsym; ++k) s[k] = float(P[phase + kSps * k + kSps] - P[phase + kSps * k]);
        S[0] = 0;
        for (size_t k = 0; k < nsym; ++k) S[k + 1] = S[k] + s[k];
        uint32_t reg = 0;
        for (size_t k = 0; k < nsym; ++k) {
            const size_t lo = k >= 16 ? k - 16 : 0, hi = std::min(nsym, k + 16);
            const double mean = (S[hi] - S[lo]) / double(hi - lo);
            reg = (reg >> 1) | (uint32_t(s[k] > mean) << 31);
            if (k < 40 || std::bitset<32>(reg ^ kAdvAccessAddress).count() > 1) continue;
            // Fit s = a*t + c over the access address (t = +/-1 per known bit).
            double st = 0, ss = 0, tt = 0, t1 = 0;
            for (int i = 0; i < 32; ++i) {
                const double t = (kAdvAccessAddress >> i & 1) ? 1 : -1, v = s[k - 31 + size_t(i)];
                st += v * t; ss += v; tt += t * t; t1 += t;
            }
            const double a = (st - ss * t1 / 32) / (tt - t1 * t1 / 32), c = (ss - a * t1) / 32;
            const size_t start = size_t(phase) + kSps * (k - 39);
            Attempt at{start, false, {}, a};
            if (a > 0.3) {
                std::vector<uint8_t> bits;
                auto slice = [&](size_t count) {
                    for (size_t i = 0; i < count && k + 1 + bits.size() < nsym; ++i) bits.push_back(s[k + 1 + bits.size()] > c);
                };
                slice(16);
                if (bits.size() == 16) {
                    auto header = bits;
                    whiten_bits(header, channel);
                    const auto hb = bits_to_bytes(header);
                    const size_t len = hb[1];
                    slice(8 * len + 24);
                    if (bits.size() == 16 + 8 * len + 24) {
                        const auto on_air = bits;  // as transmitted (whitened)
                        whiten_bits(bits, channel);
                        const std::vector<uint8_t> pdu_bits(bits.begin(), bits.end() - 24);
                        if (crc_bits(pdu_bits) == std::vector<uint8_t>(bits.end() - 24, bits.end())) {
                            // CRC-valid: every bit is known, so refit the carrier offset over the
                            // access address and the whole packet (far less noisy than 32 bits).
                            double fst = 0, fss = 0, ftt = 0, ft1 = 0;
                            const size_t nfit = 32 + on_air.size();
                            for (size_t i = 0; i < nfit; ++i) {
                                const double t = i < 32 ? ((kAdvAccessAddress >> i & 1) ? 1 : -1) : (on_air[i - 32] ? 1 : -1);
                                const double v = s[k - 31 + i];
                                fst += v * t; fss += v; ftt += t * t; ft1 += t;
                            }
                            const double fa = (fst - fss * ft1 / double(nfit)) / (ftt - ft1 * ft1 / double(nfit));
                            const double fc = (fss - fa * ft1) / double(nfit);
                            at.ok = true;
                            at.packet.channel = channel;
                            at.packet.frequency_hz = adv_channel_frequency(channel);
                            at.packet.sample = start;
                            at.packet.pdu = bits_to_bytes(pdu_bits);
                            at.packet.cfo_hz = fc / (2 * kPi) * kSymbolRate;
                            at.packet.power_db = power_db(y, start, start + kSps * (40 + bits.size()));
                        }
                    }
                }
            }
            attempts.push_back(std::move(at));
        }
    }
    // One packet per location: the 4 sampling phases usually all sync.
    std::sort(attempts.begin(), attempts.end(), [](const Attempt& a, const Attempt& b) { return a.sample < b.sample; });
    for (size_t i = 0; i < attempts.size();) {
        size_t j = i;
        const Attempt* best = nullptr;
        while (j < attempts.size() && attempts[j].sample <= attempts[i].sample + 2 * kSps) {
            if (attempts[j].ok && (!best || attempts[j].amplitude > best->amplitude)) best = &attempts[j];
            ++j;
        }
        if (stats) {
            ++stats->syncs;
            if (!best) ++stats->crc_failures;
        }
        if (best) packets.push_back(best->packet);
        i = j;
    }
    return packets;
}

std::string Address::text() const {
    char b[24];
    std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", this->b[5], this->b[4], this->b[3], this->b[2], this->b[1], this->b[0]);
    return b;
}
std::string Address::kind() const {
    if (!random) return "Public";
    switch (b[5] >> 6) {
    case 3: return "Random static";
    case 1: return "Random resolvable (rotating)";
    case 0: return "Random non-resolvable";
    default: return "Random (reserved type)";
    }
}

std::string pdu_type_name(uint8_t t) {
    static const char* names[] = {"ADV_IND", "ADV_DIRECT_IND", "ADV_NONCONN_IND", "SCAN_REQ",
                                  "SCAN_RSP", "CONNECT_IND", "ADV_SCAN_IND", "ADV_EXT_IND"};
    return t < 8 ? names[t] : "Reserved PDU type " + std::to_string(t);
}

Advert parse(const std::vector<uint8_t>& pdu) {
    Advert a;
    if (pdu.size() < 2 || pdu[1] + 2u != pdu.size()) return a;
    a.type = pdu[0] & 0x0F;
    a.type_name = pdu_type_name(a.type);
    const bool tx = pdu[0] >> 6 & 1, rx = pdu[0] >> 7 & 1;
    const uint8_t* p = pdu.data() + 2;
    const size_t n = pdu[1];
    a.fields.push_back({"PDU type", a.type_name});
    a.fields.push_back({"Channel selection algorithm #2", pdu[0] >> 5 & 1 ? "supported" : "not indicated"});
    switch (a.type) {
    case 0: case 2: case 4: case 6:
        if (n < 6) return a;
        a.advertiser = address(p, tx);
        parse_ad(a, p + 6, n - 6);
        break;
    case 1:
        if (n != 12) return a;
        a.advertiser = address(p, tx);
        a.target = address(p + 6, rx);
        break;
    case 3:
        if (n != 12) return a;
        a.scanner = address(p, tx);
        a.advertiser = address(p + 6, rx);
        break;
    case 5:
        if (n != 34) return a;
        a.initiator = address(p, tx);
        a.advertiser = address(p + 6, rx);
        {
            const uint8_t* l = p + 12;
            char b[200];
            std::snprintf(b, sizeof b, "access address 0x%08X, interval %.2f ms, latency %u, timeout %u ms, hop %u",
                          le32(l), le16(l + 10) * 1.25, le16(l + 12), le16(l + 14) * 10u, l[21] & 0x1F);
            a.fields.push_back({"Connection request", b});
            a.fields.push_back({"Channel map", hex(l + 16, 5)});
        }
        break;
    case 7:
        parse_extended(a, p, n, tx, rx);
        break;
    default:
        a.fields.push_back({"Payload", hex(p, n)});
        break;
    }
    if (a.advertiser) a.fields.insert(a.fields.begin() + 1, {"Advertiser address", a.advertiser->text() + " (" + a.advertiser->kind() + ")"});
    if (a.target) a.fields.push_back({"Target address", a.target->text() + " (" + a.target->kind() + ")"});
    if (a.scanner) a.fields.push_back({"Scanner address", a.scanner->text() + " (" + a.scanner->kind() + ")"});
    if (a.initiator) a.fields.push_back({"Initiator address", a.initiator->text() + " (" + a.initiator->kind() + ")"});
    a.valid = true;
    return a;
}

std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& pdu, int channel) {
    std::vector<uint8_t> bits = {0, 1, 0, 1, 0, 1, 0, 1};  // preamble ends opposite to AA bit 0 (0)
    for (int i = 0; i < 32; ++i) bits.push_back(kAdvAccessAddress >> i & 1);
    auto body = bytes_to_bits(pdu);
    const auto crc = crc_bits(body);
    body.insert(body.end(), crc.begin(), crc.end());
    whiten_bits(body, channel);
    bits.insert(bits.end(), body.begin(), body.end());
    // Gaussian frequency pulse, BT = 0.5, three symbols long.
    const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5) * kSps;
    std::vector<double> g;
    for (int i = -3 * kSps / 2; i <= 3 * kSps / 2; ++i) g.push_back(std::exp(-i * i / (2 * sigma * sigma)));
    double gs = 0;
    for (double v : g) gs += v;
    for (auto& v : g) v /= gs;
    std::vector<double> nrz;
    for (int i = 0; i < 8 * kSps; ++i) nrz.push_back(0);  // idle carrier before
    for (uint8_t b : bits)
        for (int i = 0; i < kSps; ++i) nrz.push_back(b ? 1 : -1);
    for (int i = 0; i < 8 * kSps; ++i) nrz.push_back(0);
    std::vector<std::complex<float>> out;
    double phase = 0;
    const int half = int(g.size() / 2);
    for (size_t n = 0; n < nrz.size(); ++n) {
        double f = 0;
        for (size_t j = 0; j < g.size(); ++j) {
            const ptrdiff_t at = ptrdiff_t(n) + ptrdiff_t(j) - half;
            if (at >= 0 && size_t(at) < nrz.size()) f += g[j] * nrz[size_t(at)];
        }
        phase += kPi / 2 / kSps * f;
        out.emplace_back(float(std::cos(phase)), float(std::sin(phase)));
    }
    return out;
}

}  // namespace rfmon::shortrange::ble
