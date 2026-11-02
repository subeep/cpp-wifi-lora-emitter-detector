// src/proto/frame.cpp — parse a CRC-valid DJI DroneID payload into telemetry.
//
// V2 field boundaries verified against the CRC-valid RUB-SysSec mini2_sm capture and
// its packed record layout (github.com/RUB-SysSec/DroneSecurity/src/droneid_packet.py).
// Motion/altitude units remain provisional until checked against a local flight.
// GPL-3.0-or-later.
#include <cctype>
#include <cstring>
#include <algorithm>

#include "droneid/dji.h"

namespace droneid::dji {
namespace {

int32_t rd_i32(const uint8_t* p) {
  return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24));
}
int16_t rd_i16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
uint16_t rd_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

uint16_t packet_crc16(const uint8_t* bytes, size_t size) {
  uint16_t checksum = 0x3692;
  for (size_t index = 0; index < size; ++index) {
    checksum ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit)
      checksum = static_cast<uint16_t>((checksum >> 1) ^ ((checksum & 1) ? 0x8408 : 0));
  }
  return checksum;
}

PacketEnvelope inspect_packet(const std::vector<uint8_t>& payload) {
  PacketEnvelope result;
  if (payload.empty()) return result;
  result.logical_bytes = static_cast<size_t>(payload[0]) + 3;
  result.length_valid = result.logical_bytes >= 5 &&
                        result.logical_bytes <= std::min<size_t>(payload.size(), 173);
  if (!result.length_valid) return result;
  result.crc16_ok = packet_crc16(payload.data(), result.logical_bytes) == 0;
  if (payload[1] == 0x10 && payload[2] == 2) result.format = "v2";
  else if (payload[1] == 0x10 && payload[2] == 3) result.format = "v3";
  else if (result.logical_bytes >= 8 && payload[1] == 0x10 &&
           std::memcmp(payload.data() + 2, "INFP", 4) == 0) result.format = "infp";
  else if (result.logical_bytes >= 8 && payload[1] == 0x13 &&
           std::memcmp(payload.data() + 2, "CRYP", 4) == 0) result.format = "cryp";
  else if (payload[1] == 0x11) result.format = "auxiliary_11";
  return result;
}

bool parse_frame(const std::vector<uint8_t>& f, Telemetry& t) {
  t = Telemetry{};
  const auto envelope = inspect_packet(f);
  if (!envelope.crc16_ok || envelope.format != "v2" ||
      (envelope.logical_bytes != 79 && envelope.logical_bytes != 91)) return false;

  constexpr double kRadScale = 1e7;              // stored as radians * 1e7
  constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
  auto coord = [&](const uint8_t* p) { return rd_i32(p) / kRadScale * kRadToDeg; };

  t.version = f[2];
  t.sequence = rd_u16(&f[3]);

  // Serial: take the 16-byte field, trimmed to printable ASCII.
  std::string serial;
  for (int i = 7; i < 23; ++i) {
    const char c = static_cast<char>(f[i]);
    if (c == 0) break;
    if (!std::isprint(static_cast<unsigned char>(c))) return false;
    serial.push_back(c);
  }
  t.serial = serial;

  t.drone_lon = coord(&f[23]);
  t.drone_lat = coord(&f[27]);
  t.altitude_m = rd_i16(&f[31]);
  t.height_m = rd_i16(&f[33]);
  t.v_north = rd_i16(&f[35]) / 100.0;
  t.v_east = rd_i16(&f[37]) / 100.0;
  t.v_up = rd_i16(&f[39]) / 100.0;
  t.yaw_deg = rd_i16(&f[41]) / 100.0;
  for (int byte = 0; byte < 8; ++byte)
    t.gps_time_ms |= uint64_t(f[43 + byte]) << (8 * byte);
  t.pilot_lat = coord(&f[51]);
  t.pilot_lon = coord(&f[55]);
  t.home_lon = coord(&f[59]);
  t.home_lat = coord(&f[63]);
  t.pilot_valid = (t.pilot_lat != 0.0 || t.pilot_lon != 0.0);
  t.home_valid = (t.home_lat != 0.0 || t.home_lon != 0.0);
  if (f[67] == 63) t.type_name = "Mini 2";
  return !t.serial.empty();
}

}  // namespace droneid::dji
