// Minimal ASTM F3411 (OpenDroneID) message encoder and Wi-Fi frame builders.
// Written from the wire layout, independently of the receiver in src/drones/.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ODID_MSG_SIZE 25
#define ODID_MAX_PACK_MSGS 9

typedef struct {
    double latitude, longitude;      // degrees
    float alt_baro, alt_geo, height; // metres; height above takeoff
    float direction;                 // degrees, 0..359
    float speed_h, speed_v;          // m/s
    uint8_t status;                  // 2 = airborne
    float seconds_past_hour;         // 0..3599.9
} odid_location_t;

typedef struct {
    double operator_lat, operator_lon;
    float operator_alt_geo;
    uint8_t operator_location_type;  // 0 takeoff, 1 live GNSS, 2 fixed
    uint8_t classification_type;     // 0 undeclared, 1 EU
    uint8_t category_eu, class_eu;
    uint16_t area_count;
    uint16_t area_radius_m;
    float area_ceiling, area_floor;  // metres; -1000 = unknown
    uint32_t timestamp_2019;         // seconds since 2019-01-01T00:00:00Z
} odid_system_t;

void odid_basic_id(uint8_t out[ODID_MSG_SIZE], uint8_t version, uint8_t id_type, uint8_t ua_type, const char *id);
void odid_location(uint8_t out[ODID_MSG_SIZE], uint8_t version, const odid_location_t *loc);
void odid_self_id(uint8_t out[ODID_MSG_SIZE], uint8_t version, uint8_t desc_type, const char *desc);
void odid_system(uint8_t out[ODID_MSG_SIZE], uint8_t version, const odid_system_t *sys);
void odid_operator_id(uint8_t out[ODID_MSG_SIZE], uint8_t version, uint8_t id_type, const char *id);

// Message pack: header, size, count, then count * 25 bytes. Returns length.
size_t odid_pack(uint8_t *out, uint8_t version, const uint8_t msgs[][ODID_MSG_SIZE], uint8_t count);

// Complete 802.11 frames without FCS (the radio appends it). Return length or 0.
size_t odid_build_beacon(uint8_t *out, size_t cap, const uint8_t mac[6], uint16_t seq, const char *ssid,
                         uint8_t channel, uint64_t tsf_us, uint8_t counter, const uint8_t *pack, size_t pack_len);
size_t odid_build_nan_action(uint8_t *out, size_t cap, const uint8_t mac[6], uint16_t seq, uint8_t counter,
                             const uint8_t *pack, size_t pack_len);

// IEEE 802.11 FCS (CRC-32), little-endian on air.
uint32_t odid_fcs(const uint8_t *data, size_t len);
