#include "odid_encode.h"
#include <math.h>
#include <string.h>

enum { TYPE_BASIC_ID = 0, TYPE_LOCATION = 1, TYPE_SELF_ID = 3, TYPE_SYSTEM = 4, TYPE_OPERATOR_ID = 5, TYPE_PACK = 15 };

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void put_text(uint8_t *p, size_t n, const char *s)
{
    memset(p, 0, n);
    size_t len = strlen(s);
    memcpy(p, s, len < n ? len : n);
}

static void put_latlon(uint8_t *p, double lat, double lon)
{
    put32(p, (uint32_t)(int32_t)lround(lat * 1e7));
    put32(p + 4, (uint32_t)(int32_t)lround(lon * 1e7));
}

// Altitudes: (metres + 1000) / 0.5; 0 means unknown.
static uint16_t enc_alt(float m)
{
    if (m <= -1000.0f) return 0;
    float v = (m + 1000.0f) / 0.5f;
    return v > 65535.0f ? 65535 : (uint16_t)lroundf(v);
}

static void header(uint8_t out[ODID_MSG_SIZE], uint8_t type, uint8_t version)
{
    memset(out, 0, ODID_MSG_SIZE);
    out[0] = (uint8_t)((type << 4) | (version & 15));
}

void odid_basic_id(uint8_t out[ODID_MSG_SIZE], uint8_t version, uint8_t id_type, uint8_t ua_type, const char *id)
{
    header(out, TYPE_BASIC_ID, version);
    out[1] = (uint8_t)((id_type << 4) | (ua_type & 15));
    put_text(out + 2, 20, id);
}

void odid_location(uint8_t out[ODID_MSG_SIZE], uint8_t version, const odid_location_t *loc)
{
    header(out, TYPE_LOCATION, version);
    int dir = (int)lroundf(loc->direction) % 360;
    if (dir < 0) dir += 360;
    uint8_t flags = 0;
    if (dir >= 180) { flags |= 0x02; dir -= 180; }
    // Speed: 0.25 m/s steps up to 63.75, then 0.75 m/s steps with the multiplier flag.
    uint8_t speed;
    if (loc->speed_h <= 63.75f) {
        speed = (uint8_t)lroundf(loc->speed_h / 0.25f);
    } else {
        flags |= 0x01;
        float v = (loc->speed_h - 63.75f) / 0.75f;
        speed = v > 254.0f ? 254 : (uint8_t)lroundf(v);
    }
    out[1] = (uint8_t)((loc->status << 4) | flags); // height type bit (0x04) clear: above takeoff
    out[2] = (uint8_t)dir;
    out[3] = speed;
    out[4] = (uint8_t)(int8_t)lroundf(loc->speed_v / 0.5f);
    put_latlon(out + 5, loc->latitude, loc->longitude);
    put16(out + 13, enc_alt(loc->alt_baro));
    put16(out + 15, enc_alt(loc->alt_geo));
    put16(out + 17, enc_alt(loc->height));
    out[19] = (4 << 4) | 10; // vertical < 10 m, horizontal < 10 m
    out[20] = (4 << 4) | 3;  // baro < 10 m, speed < 1 m/s
    put16(out + 21, (uint16_t)lroundf(fmodf(loc->seconds_past_hour, 3600.0f) * 10.0f));
    out[23] = 1;             // timestamp accuracy 0.1 s
}

void odid_self_id(uint8_t out[ODID_MSG_SIZE], uint8_t version, uint8_t desc_type, const char *desc)
{
    header(out, TYPE_SELF_ID, version);
    out[1] = desc_type;
    put_text(out + 2, 23, desc);
}

void odid_system(uint8_t out[ODID_MSG_SIZE], uint8_t version, const odid_system_t *sys)
{
    header(out, TYPE_SYSTEM, version);
    out[1] = (uint8_t)(((sys->classification_type & 7) << 2) | (sys->operator_location_type & 3));
    put_latlon(out + 2, sys->operator_lat, sys->operator_lon);
    put16(out + 10, sys->area_count);
    out[12] = (uint8_t)(sys->area_radius_m / 10 > 255 ? 255 : sys->area_radius_m / 10);
    put16(out + 13, enc_alt(sys->area_ceiling));
    put16(out + 15, enc_alt(sys->area_floor));
    out[17] = (uint8_t)((sys->category_eu << 4) | (sys->class_eu & 15));
    if (version >= 1) put16(out + 18, enc_alt(sys->operator_alt_geo));
    if (version >= 2) put32(out + 20, sys->timestamp_2019);
}

void odid_operator_id(uint8_t out[ODID_MSG_SIZE], uint8_t version, uint8_t id_type, const char *id)
{
    header(out, TYPE_OPERATOR_ID, version);
    out[1] = id_type;
    put_text(out + 2, 20, id);
}

size_t odid_pack(uint8_t *out, uint8_t version, const uint8_t msgs[][ODID_MSG_SIZE], uint8_t count)
{
    out[0] = (uint8_t)((TYPE_PACK << 4) | (version & 15));
    out[1] = ODID_MSG_SIZE;
    out[2] = count;
    for (uint8_t i = 0; i < count; ++i) memcpy(out + 3 + i * ODID_MSG_SIZE, msgs[i], ODID_MSG_SIZE);
    return 3 + (size_t)count * ODID_MSG_SIZE;
}

static size_t mgmt_header(uint8_t *p, uint8_t subtype, const uint8_t da[6], const uint8_t sa[6],
                          const uint8_t bssid[6], uint16_t seq)
{
    p[0] = subtype; p[1] = 0;       // frame control: no DS bits, no protection, no fragments
    p[2] = 0; p[3] = 0;             // duration
    memcpy(p + 4, da, 6);
    memcpy(p + 10, sa, 6);
    memcpy(p + 16, bssid, 6);
    put16(p + 22, (uint16_t)(seq << 4)); // fragment number 0
    return 24;
}

size_t odid_build_beacon(uint8_t *out, size_t cap, const uint8_t mac[6], uint16_t seq, const char *ssid,
                         uint8_t channel, uint64_t tsf_us, uint8_t counter, const uint8_t *pack, size_t pack_len)
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32 || pack_len + 5 > 255) return 0;
    if (cap < 36 + 2 + ssid_len + 2 + sizeof rates + 3 + 2 + 5 + pack_len) return 0;

    size_t n = mgmt_header(out, 0x80, broadcast, mac, mac, seq);
    for (int i = 0; i < 8; ++i) out[n++] = (uint8_t)(tsf_us >> (8 * i));
    put16(out + n, 100); n += 2;    // beacon interval, TU
    put16(out + n, 0x0001); n += 2; // capability: ESS
    out[n++] = 0; out[n++] = (uint8_t)ssid_len;
    memcpy(out + n, ssid, ssid_len); n += ssid_len;
    out[n++] = 1; out[n++] = sizeof rates;
    memcpy(out + n, rates, sizeof rates); n += sizeof rates;
    out[n++] = 3; out[n++] = 1; out[n++] = channel;
    // Vendor specific element: ASD-STAN OUI fa:0b:bc, OUI type 0x0d, message counter, pack.
    out[n++] = 0xdd; out[n++] = (uint8_t)(5 + pack_len);
    out[n++] = 0xfa; out[n++] = 0x0b; out[n++] = 0xbc; out[n++] = 0x0d;
    out[n++] = counter;
    memcpy(out + n, pack, pack_len); n += pack_len;
    return n;
}

size_t odid_build_nan_action(uint8_t *out, size_t cap, const uint8_t mac[6], uint16_t seq, uint8_t counter,
                             const uint8_t *pack, size_t pack_len)
{
    static const uint8_t nan_da[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
    static const uint8_t nan_cluster[6] = {0x50, 0x6f, 0x9a, 0x01, 0x00, 0xff};
    static const uint8_t service_id[6] = {0x88, 0x69, 0x19, 0x9d, 0x92, 0x09}; // SHA-256("org.opendroneid.remoteid")[0:6]
    size_t info_len = 1 + pack_len;
    if (info_len > 255 || cap < 30 + 3 + 10 + info_len) return 0;

    size_t n = mgmt_header(out, 0xd0, nan_da, mac, nan_cluster, seq);
    // Public action, vendor specific, Wi-Fi Alliance OUI, NAN.
    out[n++] = 0x04; out[n++] = 0x09; out[n++] = 0x50; out[n++] = 0x6f; out[n++] = 0x9a; out[n++] = 0x13;
    // Service Descriptor Attribute carrying a follow-up with the message pack as service info.
    out[n++] = 0x03;
    put16(out + n, (uint16_t)(10 + info_len)); n += 2;
    memcpy(out + n, service_id, 6); n += 6;
    out[n++] = 0x01;                // instance ID
    out[n++] = 0x00;                // requestor instance ID
    out[n++] = 0x10;                // service control: follow-up
    out[n++] = (uint8_t)info_len;
    out[n++] = counter;
    memcpy(out + n, pack, pack_len); n += pack_len;
    return n;
}

uint32_t odid_fcs(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return crc ^ 0xffffffff;
}
