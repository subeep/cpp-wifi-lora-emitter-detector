#include "security/wifi_mac_frame.hpp"

#include <algorithm>
#include <cstdio>

namespace rfmon::wifi_security {

namespace {

// Bounds-checked little/big-endian cursor. Every read reports failure
// instead of touching memory past `end`; nothing below indexes raw pointers.
struct Cursor {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    size_t left() const { return pos <= n ? n - pos : 0; }
    bool has(size_t k) const { return left() >= k; }
    bool u8(uint8_t& v) { if (!has(1)) return false; v = p[pos++]; return true; }
    bool le16(uint16_t& v) {
        if (!has(2)) return false;
        v = uint16_t(p[pos] | (uint16_t(p[pos + 1]) << 8)); pos += 2; return true;
    }
    bool be16(uint16_t& v) {
        if (!has(2)) return false;
        v = uint16_t((uint16_t(p[pos]) << 8) | p[pos + 1]); pos += 2; return true;
    }
    bool le32(uint32_t& v) {
        if (!has(4)) return false;
        v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[pos + size_t(i)];
        pos += 4; return true;
    }
    bool le48(uint64_t& v) {
        if (!has(6)) return false;
        v = 0; for (int i = 5; i >= 0; --i) v = (v << 8) | p[pos + size_t(i)];
        pos += 6; return true;
    }
    bool le64(uint64_t& v) {
        if (!has(8)) return false;
        v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[pos + size_t(i)];
        pos += 8; return true;
    }
    bool be64(uint64_t& v) {
        if (!has(8)) return false;
        v = 0; for (size_t i = 0; i < 8; ++i) v = (v << 8) | p[pos + i];
        pos += 8; return true;
    }
    bool mac(MacAddress& a) {
        if (!has(6)) return false;
        for (size_t i = 0; i < 6; ++i) a[i] = p[pos + i];
        pos += 6; return true;
    }
    bool skip(size_t k) { if (!has(k)) return false; pos += k; return true; }
    const uint8_t* here() const { return p + pos; }
};

uint64_t fnv1a64(const uint8_t* p, size_t n, int masked_index = -1, uint8_t mask = 0xFF) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = int(i) == masked_index ? uint8_t(p[i] & mask) : p[i];
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t selector(const uint8_t* s) {
    return (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | s[3];
}

void set_problem(MacFrame& f, const std::string& why) {
    if (f.problem.empty()) f.problem = why;
}

// RSN element body (802.11-2020 9.4.2.24). Every field after the version is
// optional on the wire; absence is recorded as absence, never defaulted.
RsnSummary parse_rsn(const uint8_t* p, size_t n) {
    RsnSummary r;
    Cursor c{p, n};
    if (!c.le16(r.version)) return r;
    auto suite_list = [&](std::vector<uint32_t>& out) {
        uint16_t count = 0;
        if (!c.le16(count) || count == 0 || size_t(count) * 4 > c.left()) return false;
        for (uint16_t i = 0; i < count; ++i) { out.push_back(selector(c.here())); c.skip(4); }
        return true;
    };
    if (c.left() == 0) { r.well_formed = true; return r; }
    if (!c.has(4)) return r;
    r.group_cipher = selector(c.here()); c.skip(4);
    if (c.left() == 0) { r.well_formed = true; return r; }
    if (!suite_list(r.pairwise_ciphers)) return r;
    if (c.left() == 0) { r.well_formed = true; return r; }
    if (!suite_list(r.akm_suites)) return r;
    if (c.left() == 0) { r.well_formed = true; return r; }
    uint16_t caps = 0;
    if (!c.le16(caps)) return r;
    r.capabilities = caps;
    r.mfp_required = caps & 0x40;
    r.mfp_capable = caps & 0x80;
    if (c.left() == 0) { r.well_formed = true; return r; }
    if (!c.le16(r.pmkid_count) || size_t(r.pmkid_count) * 16 > c.left()) return r;
    c.skip(size_t(r.pmkid_count) * 16);
    if (c.left() == 0) { r.well_formed = true; return r; }
    if (!c.has(4)) return r;
    r.group_mgmt_cipher = selector(c.here()); c.skip(4);
    r.well_formed = c.left() == 0;
    return r;
}

// Robust management action categories are protected once PMF is negotiated.
// Explicitly non-robust categories per 802.11-2020 Table 9-51; categories
// >= 128 are error returns of the category (value - 128). Unknown values
// default to robust, the conservative choice for "could this be protected".
bool robust_category(uint8_t cat) {
    switch (cat) {
        case 4: case 7: case 11: case 15: case 20: case 21: case 22: case 30: case 127:
            return false;
        default:
            return cat < 128;
    }
}

// The cipher header precedes every encrypted body (802.11-2020 12.3.2.2,
// 12.5.2.2 TKIP, 12.5.3.2 CCMP, 12.5.5.2 GCMP). CCMP and GCMP share a
// layout; TKIP differs only in octet 1 (WEPSeed) and octet 2 (TSC0), so the
// two can coincide. When they do, no packet number is reported - a wrong
// PN would poison any replay rule that consumed it.
std::optional<CipherHeader> parse_cipher_header(const uint8_t* p, size_t n) {
    if (n < 4) return std::nullopt;
    CipherHeader h;
    h.key_id = uint8_t(p[3] >> 6);
    h.ext_iv = p[3] & 0x20;
    if (!h.ext_iv) {
        h.kind = CipherHeader::Kind::Wep;
        h.wep_iv = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);  // octets as transmitted
        h.header_len = 4;
        return h;
    }
    if (n < 8) return std::nullopt;
    h.header_len = 8;
    const bool tkip_seed = p[1] == uint8_t((p[0] | 0x20) & 0x7F);
    const bool ccmp_reserved = p[2] == 0;
    uint64_t high = uint64_t(p[4]) | (uint64_t(p[5]) << 8) | (uint64_t(p[6]) << 16) | (uint64_t(p[7]) << 24);
    if (ccmp_reserved && !tkip_seed) {
        h.kind = CipherHeader::Kind::CcmpOrGcmp;
        h.packet_number = uint64_t(p[0]) | (uint64_t(p[1]) << 8) | (high << 16);
    } else if (tkip_seed && !ccmp_reserved) {
        h.kind = CipherHeader::Kind::Tkip;
        h.packet_number = uint64_t(p[2]) | (uint64_t(p[0]) << 8) | (high << 16);  // TSC0, TSC1, TSC2..5
    } else {
        h.kind = CipherHeader::Kind::AmbiguousExtIv;
    }
    return h;
}

void parse_eapol(Cursor c, DataPayload& d) {
    uint8_t version = 0, type = 0;
    uint16_t length = 0;
    if (!c.u8(version) || !c.u8(type) || !c.be16(length)) return;
    d.eapol_packet_type = type;
    if (type != 3) return;
    // Descriptor(1) KeyInfo(2) KeyLen(2) Replay(8) Nonce(32) IV(16) RSC(8)
    // Reserved(8) MIC(16 for the common AKMs) KeyDataLen(2).
    constexpr size_t kFixed16 = 95;
    size_t body = std::min<size_t>(length, c.left());
    if (body < kFixed16) return;
    EapolKey k;
    k.protocol_version = version;
    Cursor b{c.here(), body};
    b.u8(k.descriptor_type);
    b.be16(k.key_info);
    b.be16(k.key_length);
    b.be64(k.replay_counter);
    for (size_t i = 0; i < 32; ++i) { k.nonce[i] = b.here()[i]; if (k.nonce[i]) k.nonce_zero = false; }
    b.skip(32 + 16 + 8 + 8);
    // MIC length depends on the negotiated AKM, which a passive observer may
    // not know. Accept the 16-octet layout only when the key-data length is
    // consistent with it; otherwise try 24 (Suite B 192); otherwise leave it.
    for (size_t mic : {size_t(16), size_t(24)}) {
        Cursor t{b.here(), b.left()};
        uint16_t kd = 0;
        if (t.skip(mic) && t.be16(kd) && kd == t.left()) { k.key_data_length = kd; break; }
    }
    const uint16_t ki = k.key_info;
    k.descriptor_version = uint8_t(ki & 0x7);
    k.pairwise = ki & 0x0008;
    k.install = ki & 0x0040;
    k.ack = ki & 0x0080;
    k.mic = ki & 0x0100;
    k.secure = ki & 0x0200;
    k.error = ki & 0x0400;
    k.request = ki & 0x0800;
    k.encrypted_key_data = ki & 0x1000;
    // Message inference from flags only (12.7.6). M2/M4 are separated by the
    // Secure bit, which some older supplicants set differently - an inference.
    if (!k.request) {
        if (k.pairwise) {
            if (k.ack && !k.mic) k.message = EapolKeyMessage::M1;
            else if (k.ack && k.mic && k.install) k.message = EapolKeyMessage::M3;
            else if (!k.ack && k.mic) k.message = k.secure ? EapolKeyMessage::M4 : EapolKeyMessage::M2;
        } else if (k.mic && k.secure) {
            k.message = k.ack ? EapolKeyMessage::GroupM1 : EapolKeyMessage::GroupM2;
        }
    }
    d.eapol_key = k;
}

void parse_data_payload(Cursor c, bool amsdu, DataPayload& d) {
    if (amsdu) {
        // First A-MSDU subframe header: DA(6) SA(6) Length(2, big-endian).
        if (!c.skip(12)) return;
        uint16_t sub_len = 0;
        if (!c.be16(sub_len)) return;
        c.n = std::min(c.n, c.pos + sub_len);
    }
    if (!c.has(8)) return;
    const uint8_t* l = c.here();
    const bool snap = l[0] == 0xAA && l[1] == 0xAA && l[2] == 0x03 &&
                      ((l[3] == 0 && l[4] == 0 && l[5] == 0) || (l[3] == 0 && l[4] == 0 && l[5] == 0xF8));
    if (!snap) return;
    c.skip(6);
    uint16_t et = 0;
    c.be16(et);
    d.ethertype = et;
    if (et == 0x888E) parse_eapol(c, d);
}

void parse_management_body(MacFrame& f, Cursor c) {
    ManagementBody m;
    const uint8_t st = f.fc.subtype;
    bool truncated = false;
    auto need = [&](bool ok) { if (!ok) truncated = true; return ok; };
    bool elements_follow = false;

    switch (st) {
        case mgmt::Beacon:
        case mgmt::ProbeResponse: {
            uint64_t tsf = 0; uint16_t bi = 0, cap = 0;
            if (need(c.le64(tsf) && c.le16(bi) && c.le16(cap))) {
                m.timestamp_tsf = tsf; m.beacon_interval_tu = bi; m.capability = cap;
                elements_follow = true;
            }
            break;
        }
        case mgmt::ProbeRequest:
            elements_follow = true;
            break;
        case mgmt::AssocRequest:
        case mgmt::ReassocRequest: {
            uint16_t cap = 0, li = 0;
            if (!need(c.le16(cap) && c.le16(li))) break;
            m.capability = cap; m.listen_interval = li;
            if (st == mgmt::ReassocRequest) {
                MacAddress ap{};
                if (!need(c.mac(ap))) break;
                m.current_ap = ap;
            }
            elements_follow = true;
            break;
        }
        case mgmt::AssocResponse:
        case mgmt::ReassocResponse: {
            uint16_t cap = 0, status = 0, aid = 0;
            if (!need(c.le16(cap) && c.le16(status) && c.le16(aid))) break;
            m.capability = cap; m.status_code = status; m.association_id = uint16_t(aid & 0x3FFF);
            elements_follow = true;
            break;
        }
        case mgmt::Disassociation:
        case mgmt::Deauthentication: {
            uint16_t reason = 0;
            if (!need(c.le16(reason))) break;
            m.reason_code = reason;
            elements_follow = true;  // optional vendor elements / MME
            break;
        }
        case mgmt::Authentication: {
            uint16_t alg = 0, seq = 0, status = 0;
            if (!need(c.le16(alg) && c.le16(seq) && c.le16(status))) break;
            m.auth_algorithm = alg; m.auth_transaction = seq; m.status_code = status;
            if (alg == 3) {
                // SAE fields are not elements. Commit (transaction 1) with
                // success, H2E (126) or anti-clogging (76) starts with the group.
                if (seq == 1 && (status == 0 || status == 126 || status == 127 || status == 76)) {
                    uint16_t group = 0;
                    if (c.le16(group)) m.sae_group = group;
                }
            } else {
                elements_follow = true;  // challenge text, FT/FILS elements
            }
            break;
        }
        case mgmt::Atim:
            break;
        case mgmt::Action:
        case mgmt::ActionNoAck: {
            uint8_t cat = 0, code = 0;
            if (!need(c.u8(cat))) break;
            m.action_category = cat;
            m.robust_action = robust_category(cat);
            if (!need(c.u8(code))) break;
            m.action_code = code;
            if (cat == 8 && (code == 0 || code == 1)) {  // SA Query request/response
                uint16_t tid = 0;
                if (need(c.le16(tid))) m.sa_query_transaction = tid;
            }
            // Channel-switch action frames are decoded because a spoofed switch
            // is a known way to push stations off a channel. Spectrum Management
            // CSA (0/4) carries elements; Public extended CSA (4/4) starts with
            // mode, operating class, channel and count as fixed octets.
            if (cat == 0 && code == 4) {
                ElementSummary es = parse_elements(c.here(), c.left());
                if (!es.complete) { f.body_state = BodyState::Malformed; set_problem(f, "CSA action elements malformed"); }
                m.elements = es;
                break;
            }
            if (cat == 4 && code == 4) {
                uint8_t mode = 0, op = 0, ch = 0, cnt = 0;
                if (!need(c.u8(mode) && c.u8(op) && c.u8(ch) && c.u8(cnt))) break;
                ElementSummary es = parse_elements(c.here(), c.left());
                ChannelSwitch cs; cs.extended = true; cs.mode = mode; cs.new_operating_class = op;
                cs.new_channel = ch; cs.count = cnt;
                es.channel_switch = cs;
                if (!es.complete) { f.body_state = BodyState::Malformed; set_problem(f, "extended CSA action elements malformed"); }
                m.elements = es;
                break;
            }
            // Other action bodies are category-specific, not a plain element
            // list. A group-addressed robust action frame ends in an MME; look
            // for one in the last 18/26 octets only, not by walking the body.
            for (size_t mme_len : {size_t(18), size_t(26)}) {
                if (c.left() >= mme_len) {
                    const uint8_t* t = c.p + c.n - mme_len;
                    if (t[0] == 76 && t[1] == mme_len - 2) {
                        ManagementMic mic;
                        Cursor mc{t + 2, mme_len - 2};
                        mc.le16(mic.key_id); mc.le48(mic.ipn);
                        mic.mic_len = mme_len - 10;
                        ElementSummary es; es.mme = mic; es.ids.push_back(76);
                        m.elements = es;
                        break;
                    }
                }
            }
            break;
        }
        default:
            f.body_state = BodyState::Unsupported;
            set_problem(f, "management subtype not decoded");
            f.management = m;
            return;
    }

    if (truncated) {
        f.body_state = BodyState::Truncated;
        set_problem(f, "management fixed fields truncated");
    } else if (elements_follow) {
        ElementSummary es = parse_elements(c.here(), c.left());
        if (!es.complete) {
            f.body_state = BodyState::Malformed;
            set_problem(f, "element list overruns or has trailing octets");
        } else {
            f.body_state = BodyState::Readable;
        }
        m.elements = es;
    } else if (f.body_state != BodyState::Malformed) {
        f.body_state = (c.n == 0) ? BodyState::Absent : BodyState::Readable;
        if (st == mgmt::Atim && c.left() != 0) {
            f.body_state = BodyState::Malformed;
            set_problem(f, "ATIM carries an unexpected body");
        }
    }
    if (f.body_state == BodyState::Readable && m.elements && m.elements->mme)
        f.body_state = BodyState::IntegrityProtected;
    f.management = m;
}

}  // namespace

std::string format_mac(const MacAddress& a) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
    return buf;
}
bool is_group_address(const MacAddress& a) { return a[0] & 0x01; }
bool is_broadcast_address(const MacAddress& a) {
    for (uint8_t b : a) if (b != 0xFF) return false;
    return true;
}
bool is_locally_administered(const MacAddress& a) { return a[0] & 0x02; }

ElementSummary parse_elements(const uint8_t* p, size_t n) {
    ElementSummary s;
    size_t pos = 0;
    while (pos < n) {
        if (n - pos < 2) { s.complete = false; break; }
        const uint8_t id = p[pos], len = p[pos + 1];
        const uint8_t* v = p + pos + 2;
        if (len > n - pos - 2) {
            // Keep the overrunning element's ID as evidence of what was sent.
            s.ids.push_back(id == 255 && n - pos > 2 ? uint16_t((255u << 8) | p[pos + 2]) : uint16_t(id));
            s.complete = false;
            break;
        }
        pos += 2 + size_t(len);
        uint16_t key = id;
        if (id == 255) {
            if (len == 0) { s.complete = false; s.ids.push_back(uint16_t(255u << 8)); continue; }
            key = uint16_t((255u << 8) | v[0]);
        }
        s.ids.push_back(key);
        auto bad = [&] { s.complete = false; };
        switch (id) {
            case 0:
                if (len > 32) { bad(); break; }
                s.ssid_present = true;
                s.ssid_wildcard = len == 0;
                s.ssid = std::string(reinterpret_cast<const char*>(v), len);
                s.ssid_all_zero = len > 0;
                for (uint8_t i = 0; i < len; ++i) if (v[i]) s.ssid_all_zero = false;
                break;
            case 1: s.supported_rates.assign(v, v + len); break;
            case 50: s.extended_rates.assign(v, v + len); break;
            case 3: if (len == 1) s.ds_channel = v[0]; else bad(); break;
            case 5:
                if (len >= 4) {
                    Tim t{v[0], v[1], v[2], bool(v[2] & 1)};
                    s.tim = t;
                } else bad();
                break;
            case 7:
                if (len >= 3) s.country = std::string(reinterpret_cast<const char*>(v), 2); else bad();
                break;
            case 11:
                if (len == 5) {
                    BssLoad b;
                    b.station_count = uint16_t(v[0] | (v[1] << 8));
                    b.channel_utilization = v[2];
                    b.admission_capacity = uint16_t(v[3] | (v[4] << 8));
                    s.bss_load = b;
                } else bad();
                break;
            case 16: s.challenge_text_len = len; break;
            case 32: if (len == 1) s.power_constraint_db = v[0]; else bad(); break;
            case 37:
                if (len == 3) { ChannelSwitch c; c.mode = v[0]; c.new_channel = v[1]; c.count = v[2]; s.channel_switch = c; }
                else bad();
                break;
            case 60:
                if (len == 4) {
                    ChannelSwitch c; c.extended = true; c.mode = v[0]; c.new_operating_class = v[1];
                    c.new_channel = v[2]; c.count = v[3]; s.channel_switch = c;
                } else bad();
                break;
            case 40:
                if (len == 6) {
                    QuietElement q; q.count = v[0]; q.period = v[1];
                    q.duration_tu = uint16_t(v[2] | (v[3] << 8)); q.offset_tu = uint16_t(v[4] | (v[5] << 8));
                    s.quiet = q;
                } else bad();
                break;
            case 45:
                if (len == 26) s.ht_capabilities_info = uint16_t(v[0] | (v[1] << 8)); else bad();
                break;
            case 48: {
                RsnSummary r = parse_rsn(v, len);
                if (!r.well_formed) bad();
                s.rsn = r;
                break;
            }
            case 61: if (len == 22) s.ht_primary_channel = v[0]; else bad(); break;
            case 71: s.multiple_bssid = true; break;
            case 76:
                if (len == 16 || len == 24) {
                    ManagementMic m;
                    Cursor c{v, len};
                    c.le16(m.key_id); c.le48(m.ipn);
                    m.mic_len = size_t(len) - 8;
                    s.mme = m;
                } else bad();
                break;
            case 127: s.extended_capabilities.assign(v, v + len); break;
            case 191: if (len == 12) s.vht_capabilities = true; else bad(); break;
            case 221:
                if (len >= 3) {
                    uint32_t oui = (uint32_t(v[0]) << 24) | (uint32_t(v[1]) << 16) | (uint32_t(v[2]) << 8) |
                                   (len >= 4 ? v[3] : 0);
                    s.vendor_ouis.push_back(oui);
                    if (len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2) {
                        if (v[3] == 1) s.wpa_vendor_ie = true;
                        if (v[3] == 4) s.wps_present = true;
                    }
                } else bad();
                break;
            case 255:
                if (v[0] == 35) s.he_capabilities = true;
                if (v[0] == 108) s.eht_capabilities = true;
                break;
            default: break;
        }
    }
    return s;
}

MacFrame parse_mac_frame(const uint8_t* mpdu, size_t len, bool has_fcs) {
    MacFrame f;
    size_t n = len;
    if (has_fcs) {
        if (len < 4) {
            f.fcs = FcsStatus::TooShort;
            n = 0;
        } else {
            n = len - 4;
            f.fcs_value = uint32_t(mpdu[n]) | (uint32_t(mpdu[n + 1]) << 8) |
                          (uint32_t(mpdu[n + 2]) << 16) | (uint32_t(mpdu[n + 3]) << 24);
            f.fcs = wifi::fcs32(mpdu, n) == f.fcs_value ? FcsStatus::Valid : FcsStatus::Invalid;
        }
    }
    f.content_hash = fnv1a64(mpdu, n);
    f.content_hash_retry_invariant = fnv1a64(mpdu, n, 1, uint8_t(~0x08));

    Cursor c{mpdu, n};
    uint16_t fc = 0;
    if (!c.le16(fc)) { set_problem(f, "shorter than frame control"); return f; }
    f.fc.raw = fc;
    f.fc.protocol_version = uint8_t(fc & 0x3);
    f.fc.type = FrameType((fc >> 2) & 0x3);
    f.fc.subtype = uint8_t((fc >> 4) & 0xF);
    f.fc.to_ds = fc & 0x0100;
    f.fc.from_ds = fc & 0x0200;
    f.fc.more_fragments = fc & 0x0400;
    f.fc.retry = fc & 0x0800;
    f.fc.power_management = fc & 0x1000;
    f.fc.more_data = fc & 0x2000;
    f.fc.protected_frame = fc & 0x4000;
    f.fc.order_or_htc = fc & 0x8000;

    if (f.fc.protocol_version != 0) {
        // PV1 (802.11ah) uses a different header entirely.
        f.header_state = ParseState::Unsupported;
        set_problem(f, "protocol version " + std::to_string(f.fc.protocol_version));
        f.header_len = 2;
        return f;
    }
    if (f.fc.type == FrameType::Extension) {
        f.header_state = ParseState::Unsupported;
        set_problem(f, "extension frame type (DMG/S1G) not decoded");
        f.header_len = 2;
        return f;
    }
    if (!c.le16(f.duration_id)) { set_problem(f, "truncated before Duration/ID"); f.header_len = c.pos; return f; }

    const bool ps_poll = f.fc.type == FrameType::Control && f.fc.subtype == ctrl::PsPoll;
    if (ps_poll) {
        if ((f.duration_id & 0xC000) == 0xC000) {
            f.duration_kind = DurationKind::AssociationId;
            f.aid_from_duration = uint16_t(f.duration_id & 0x3FFF);
        } else {
            f.duration_kind = DurationKind::Reserved;
        }
    } else if (!(f.duration_id & 0x8000)) {
        f.duration_kind = DurationKind::Nav;
        f.nav_us = f.duration_id;
    } else {
        f.duration_kind = f.duration_id == 0x8000 ? DurationKind::CfpFixed : DurationKind::Reserved;
    }

    auto truncated = [&](const char* where) {
        f.header_state = ParseState::Truncated;
        f.header_len = c.pos;
        set_problem(f, std::string("header truncated at ") + where);
        return f;
    };

    MacAddress a1{}, a2{}, a3{}, a4{};

    if (f.fc.type == FrameType::Control) {
        const uint8_t st = f.fc.subtype;
        const bool ra_only = st == ctrl::Cts || st == ctrl::Ack;
        const bool ra_ta = st == ctrl::Rts || st == ctrl::PsPoll || st == ctrl::CfEnd || st == ctrl::CfEndCfAck ||
                           st == ctrl::BlockAckRequest || st == ctrl::BlockAck || st == ctrl::Trigger ||
                           st == ctrl::BeamformingReportPoll || st == ctrl::VhtNdpAnnouncement;
        if (!ra_only && !ra_ta) {
            f.header_state = ParseState::Unsupported;
            f.header_len = c.pos;
            set_problem(f, "control subtype not decoded");
            return f;
        }
        if (!c.mac(a1)) return truncated("Address 1");
        f.addr1 = a1;
        f.receiver = a1;
        if (ra_ta) {
            if (!c.mac(a2)) return truncated("Address 2");
            f.addr2 = a2;
            MacAddress ta = a2;
            // A set Individual/Group bit in the TA of these subtypes signals
            // bandwidth, not a group transmitter (9.3.1.2): clear it for the role.
            if (st != ctrl::PsPoll && st != ctrl::CfEnd && st != ctrl::CfEndCfAck) ta[0] &= 0xFE;
            f.transmitter = ta;
            if (st == ctrl::PsPoll) f.bssid = a1;
            if (st == ctrl::CfEnd || st == ctrl::CfEndCfAck) f.bssid = a2;
        }
        f.header_len = c.pos;
        f.header_state = ParseState::Ok;
        f.body_len = c.left();
        const bool bodiless = ra_only || st == ctrl::Rts || st == ctrl::PsPoll || st == ctrl::CfEnd || st == ctrl::CfEndCfAck;
        if (bodiless) {
            f.body_state = f.body_len ? BodyState::Malformed : BodyState::Absent;
            if (f.body_len) set_problem(f, "control frame carries unexpected octets");
        } else {
            f.body_state = BodyState::Unsupported;  // BAR/BA/Trigger/NDPA bodies not decoded
        }
        return f;
    }

    // Management and data share the 24-octet base header. Address 1 is
    // always the receiver and Address 2 the transmitter, so those roles are
    // set as soon as each is read; the rest depend on the complete header.
    if (!c.mac(a1)) return truncated("Address 1");
    f.addr1 = a1;
    f.receiver = a1;
    if (!c.mac(a2)) return truncated("Address 2");
    f.addr2 = a2;
    f.transmitter = a2;
    if (!c.mac(a3)) return truncated("Address 3");
    f.addr3 = a3;
    uint16_t sc = 0;
    if (!c.le16(sc)) return truncated("Sequence Control");
    f.sequence_control = sc;
    f.sequence_number = uint16_t(sc >> 4);
    f.fragment_number = uint8_t(sc & 0xF);

    bool amsdu = false;
    if (f.fc.type == FrameType::Management) {
        f.receiver = a1; f.destination = a1;
        f.transmitter = a2; f.source = a2;
        f.bssid = a3;
        if (f.fc.to_ds || f.fc.from_ds) set_problem(f, "management frame with DS bits set");
        if (f.fc.order_or_htc) {
            uint32_t htc = 0;
            if (!c.le32(htc)) return truncated("HT Control");
            f.ht_control = htc;
        }
    } else {  // Data
        const bool four = f.fc.to_ds && f.fc.from_ds;
        if (four) {
            if (!c.mac(a4)) return truncated("Address 4");
            f.addr4 = a4;
        }
        const bool qos = f.fc.subtype & 0x8;
        if (qos) {
            uint16_t q = 0;
            if (!c.le16(q)) return truncated("QoS Control");
            f.qos_control = q;
            f.tid = uint8_t(q & 0xF);
            amsdu = q & 0x80;
            if (f.fc.order_or_htc) {  // +HTC only has this meaning on QoS data
                uint32_t htc = 0;
                if (!c.le32(htc)) return truncated("HT Control");
                f.ht_control = htc;
            }
        }
        // Table 9-26. With an A-MSDU, Address 3 carries the BSSID instead of
        // SA/DA; the four-address A-MSDU case is mesh-specific and left unassigned.
        f.receiver = a1; f.transmitter = a2;
        if (!f.fc.to_ds && !f.fc.from_ds) { f.destination = a1; f.source = a2; f.bssid = a3; }
        else if (!f.fc.to_ds && f.fc.from_ds) { f.destination = a1; f.bssid = a2; if (!amsdu) f.source = a3; }
        else if (f.fc.to_ds && !f.fc.from_ds) { f.bssid = a1; f.source = a2; if (!amsdu) f.destination = a3; }
        else if (!amsdu) { f.destination = a3; f.source = a4; }
    }
    f.header_len = c.pos;
    f.header_state = ParseState::Ok;
    f.body_len = c.left();
    Cursor body{c.here(), c.left()};

    if (f.fc.protected_frame) {
        f.body_state = BodyState::Encrypted;
        f.cipher = parse_cipher_header(body.p, body.n);
        if (!f.cipher) { f.body_state = BodyState::Truncated; set_problem(f, "cipher header truncated"); }
        if (f.fc.type == FrameType::Management) f.management = ManagementBody{};
        return f;
    }

    if (f.fc.type == FrameType::Management) {
        parse_management_body(f, body);
        if (has_fcs && (f.fc.subtype == mgmt::Beacon || f.fc.subtype == mgmt::ProbeResponse)) {
            f.beacon_identity = wifi::parse_beacon(mpdu, len);
        }
    } else {
        DataPayload d;
        d.null_function = f.fc.subtype & 0x4;
        d.amsdu = amsdu;
        if (d.null_function) {
            f.body_state = f.body_len ? BodyState::Malformed : BodyState::Absent;
            if (f.body_len) set_problem(f, "null-function data frame carries a body");
        } else if (f.body_len == 0) {
            f.body_state = BodyState::Absent;
        } else {
            f.body_state = BodyState::Readable;
            parse_data_payload(body, amsdu, d);
        }
        f.data = d;
    }
    return f;
}

const char* frame_type_name(FrameType t) {
    switch (t) {
        case FrameType::Management: return "Mgmt";
        case FrameType::Control: return "Ctrl";
        case FrameType::Data: return "Data";
        case FrameType::Extension: return "Ext";
    }
    return "?";
}

std::string subtype_name(FrameType t, uint8_t st) {
    static const char* m[16] = {"Association request", "Association response", "Reassociation request",
                                "Reassociation response", "Probe request", "Probe response",
                                "Timing advertisement", "Reserved", "Beacon", "ATIM", "Disassociation",
                                "Authentication", "Deauthentication", "Action", "Action no ack", "Reserved"};
    static const char* c[16] = {"Reserved", "Reserved", "Trigger", "TACK", "Beamforming report poll",
                                "VHT/HE NDP announcement", "Control frame extension", "Control wrapper",
                                "Block ack request", "Block ack", "PS-Poll", "RTS", "CTS", "Ack", "CF-End",
                                "CF-End+CF-Ack"};
    static const char* d[16] = {"Data", "Reserved", "Reserved", "Reserved", "Null", "Reserved", "Reserved",
                                "Reserved", "QoS data", "QoS data+CF-Ack", "QoS data+CF-Poll",
                                "QoS data+CF-Ack+CF-Poll", "QoS null", "Reserved", "QoS CF-Poll",
                                "QoS CF-Ack+CF-Poll"};
    st &= 0xF;
    switch (t) {
        case FrameType::Management: return m[st];
        case FrameType::Control: return c[st];
        case FrameType::Data: return d[st];
        default: return "Extension " + std::to_string(st);
    }
}

const char* fcs_status_name(FcsStatus s) {
    switch (s) {
        case FcsStatus::NotPresent: return "not present";
        case FcsStatus::TooShort: return "too short";
        case FcsStatus::Valid: return "valid";
        case FcsStatus::Invalid: return "invalid";
    }
    return "?";
}
const char* parse_state_name(ParseState s) {
    switch (s) {
        case ParseState::Ok: return "ok";
        case ParseState::Truncated: return "truncated";
        case ParseState::Malformed: return "malformed";
        case ParseState::Unsupported: return "unsupported";
    }
    return "?";
}
const char* body_state_name(BodyState s) {
    switch (s) {
        case BodyState::Absent: return "absent";
        case BodyState::Readable: return "readable";
        case BodyState::Encrypted: return "encrypted";
        case BodyState::IntegrityProtected: return "integrity-protected";
        case BodyState::Truncated: return "truncated";
        case BodyState::Malformed: return "malformed";
        case BodyState::Unsupported: return "unsupported";
    }
    return "?";
}
const char* eapol_message_name(EapolKeyMessage m) {
    switch (m) {
        case EapolKeyMessage::M1: return "4-way M1";
        case EapolKeyMessage::M2: return "4-way M2";
        case EapolKeyMessage::M3: return "4-way M3";
        case EapolKeyMessage::M4: return "4-way M4";
        case EapolKeyMessage::GroupM1: return "group M1";
        case EapolKeyMessage::GroupM2: return "group M2";
        default: return "unknown";
    }
}

std::string reason_code_name(uint16_t code) {
    switch (code) {
        case 1: return "Unspecified";
        case 2: return "Previous authentication no longer valid";
        case 3: return "Sending STA is leaving (deauthenticated)";
        case 4: return "Inactivity";
        case 5: return "AP unable to handle all associated STAs";
        case 6: return "Class 2 frame from nonauthenticated STA";
        case 7: return "Class 3 frame from nonassociated STA";
        case 8: return "Sending STA is leaving BSS (disassociated)";
        case 9: return "Not authenticated for (re)association";
        case 10: return "Power capability unacceptable";
        case 11: return "Supported channels unacceptable";
        case 12: return "BSS transition management";
        case 13: return "Invalid element";
        case 14: return "MIC failure";
        case 15: return "4-way handshake timeout";
        case 16: return "Group key handshake timeout";
        case 17: return "Element in 4-way handshake differs";
        case 18: return "Invalid group cipher";
        case 19: return "Invalid pairwise cipher";
        case 20: return "Invalid AKMP";
        case 21: return "Unsupported RSNE version";
        case 22: return "Invalid RSNE capabilities";
        case 23: return "IEEE 802.1X authentication failed";
        case 24: return "Cipher suite rejected by policy";
        case 34: return "Excessive unacknowledged frames";
        default: return "Reason " + std::to_string(code);
    }
}

std::string status_code_name(uint16_t code) {
    switch (code) {
        case 0: return "Success";
        case 1: return "Unspecified failure";
        case 10: return "Cannot support all requested capabilities";
        case 12: return "Denied, other reason";
        case 13: return "Unsupported authentication algorithm";
        case 14: return "Authentication transaction out of sequence";
        case 15: return "Challenge failure";
        case 16: return "Authentication timeout";
        case 17: return "AP unable to handle additional STAs";
        case 18: return "Basic rates not supported";
        case 30: return "Rejected temporarily (try again later)";
        case 31: return "Robust management frame policy violation";
        case 37: return "Request declined";
        case 40: return "Invalid element";
        case 53: return "Invalid PMKID";
        case 76: return "Anti-clogging token required";
        case 77: return "Finite cyclic group not supported";
        case 126: return "SAE hash-to-element";
        case 127: return "SAE-PK";
        default: return "Status " + std::to_string(code);
    }
}

std::string auth_algorithm_name(uint16_t alg) {
    switch (alg) {
        case 0: return "Open System";
        case 1: return "Shared Key";
        case 2: return "Fast BSS Transition";
        case 3: return "SAE";
        case 4: return "FILS shared key";
        case 5: return "FILS shared key + PFS";
        case 6: return "FILS public key";
        case 7: return "PASN";
        case 0xFFFF: return "Vendor specific";
        default: return "Algorithm " + std::to_string(alg);
    }
}

std::string frame_type_label(const MacFrame& f) {
    if (f.fc.protocol_version != 0) return "PV" + std::to_string(f.fc.protocol_version) + "/not decoded";
    return std::string(frame_type_name(f.fc.type)) + "/" + subtype_name(f.fc.type, f.fc.subtype);
}

std::string describe(const MacFrame& f) {
    std::string s = frame_type_label(f);
    auto add = [&](const std::string& k, const std::string& v) { s += " " + k + "=" + v; };
    if (f.transmitter) add("TA", format_mac(*f.transmitter));
    if (f.receiver) add("RA", format_mac(*f.receiver));
    if (f.bssid) add("BSSID", format_mac(*f.bssid));
    if (f.sequence_number) add("seq", std::to_string(*f.sequence_number) + "/" + std::to_string(*f.fragment_number));
    if (f.fc.retry) s += " retry";
    if (f.fc.protected_frame) s += " protected";
    if (f.nav_us) add("nav_us", std::to_string(*f.nav_us));
    if (f.aid_from_duration) add("aid", std::to_string(*f.aid_from_duration));
    if (f.tid) add("tid", std::to_string(*f.tid));
    if (f.management) {
        const auto& m = *f.management;
        if (m.timestamp_tsf) add("tsf", std::to_string(*m.timestamp_tsf));
        if (m.beacon_interval_tu) add("bi_tu", std::to_string(*m.beacon_interval_tu));
        if (m.reason_code) add("reason", std::to_string(*m.reason_code) + "(" + reason_code_name(*m.reason_code) + ")");
        if (m.auth_algorithm) add("auth", auth_algorithm_name(*m.auth_algorithm) + "#" + std::to_string(*m.auth_transaction));
        if (m.status_code) add("status", std::to_string(*m.status_code) + "(" + status_code_name(*m.status_code) + ")");
        if (m.association_id) add("aid", std::to_string(*m.association_id));
        if (m.sae_group) add("sae_group", std::to_string(*m.sae_group));
        if (m.action_category) add("action", std::to_string(*m.action_category) + "/" +
                                   (m.action_code ? std::to_string(*m.action_code) : std::string("?")) +
                                   (m.robust_action ? "(robust)" : ""));
        if (m.sa_query_transaction) add("sa_query_tid", std::to_string(*m.sa_query_transaction));
        if (m.elements) {
            const auto& e = *m.elements;
            if (e.ssid) add("ssid", e.ssid_wildcard ? std::string("<wildcard>") : wifi::display_text(*e.ssid));
            if (e.ds_channel) add("ds_ch", std::to_string(*e.ds_channel));
            if (e.channel_switch) add("csa", std::to_string(e.channel_switch->new_channel) + "@" +
                                      std::to_string(e.channel_switch->count));
            if (e.quiet) add("quiet_tu", std::to_string(e.quiet->duration_tu));
            if (e.bss_load) add("bss_load", std::to_string(e.bss_load->station_count) + "sta/" +
                                std::to_string(e.bss_load->channel_utilization));
            if (e.rsn) add("rsn_mfp", e.rsn->mfp_required ? "required" : e.rsn->mfp_capable ? "capable" : "off");
            if (e.mme) add("mme_ipn", std::to_string(e.mme->ipn));
        }
    }
    if (f.cipher) {
        const char* kinds[] = {"WEP", "CCMP/GCMP", "TKIP", "ambiguous"};
        add("cipher", kinds[int(f.cipher->kind)]);
        add("key_id", std::to_string(f.cipher->key_id));
        if (f.cipher->packet_number) add("pn", std::to_string(*f.cipher->packet_number));
    }
    if (f.data) {
        if (f.data->ethertype) { char b[8]; std::snprintf(b, sizeof(b), "0x%04x", *f.data->ethertype); add("ethertype", b); }
        if (f.data->eapol_key) {
            add("eapol", eapol_message_name(f.data->eapol_key->message));
            add("replay_ctr", std::to_string(f.data->eapol_key->replay_counter));
        }
    }
    add("fcs", fcs_status_name(f.fcs));
    add("header", parse_state_name(f.header_state));
    add("body", body_state_name(f.body_state));
    if (!f.problem.empty()) add("problem", "\"" + f.problem + "\"");
    return s;
}

}  // namespace rfmon::wifi_security
