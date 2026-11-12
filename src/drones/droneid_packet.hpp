#pragma once
// DJI DroneID packet classification shared by the native worker and the app.
// Pure byte/JSON handling: no DSP, no threads, no dependency on the vendored decoder.
// DroneID is self-reported and unauthenticated; nothing here confirms an identity.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace drone::droneid {

inline constexpr char contract_version[] = "droneid-packet-v1";

// Documented 2.4 GHz DroneID channel centres (upstream reference README; the same list as
// droneid::dji::known_channels_24()).
inline constexpr double channels_24[] = {2399.5e6, 2414.5e6, 2429.5e6, 2444.5e6, 2459.5e6};
// 5.8 GHz DroneID channel centres on which checksum-valid packets were decoded here (DRFF-R2
// hover recordings, 5745 MHz centre, 100 MSps: 32 packets at 5756.5 MHz, 14 at 5776.5 MHz;
// docs/DRONE_RF_DRONEID_ON_DRONERFB_2026-11-11.md). Those recordings cover 5695-5795 MHz only,
// so channels above that are not listed; captures there keep the decoder's automatic choice.
inline constexpr double channels_58[] = {5756.5e6, 5776.5e6};

// What a checksum-valid payload tells us, without trusting it.
struct Info {
    std::string format = "unknown";   // v2, v3, infp, cryp, auxiliary_11, unknown
    int payload_version = 0;          // 2 or 3 for the 0x10 message family, otherwise 0
    bool identity_readable = false;   // a version 2 or 3 telemetry record whose own checksum passed
    bool identity_encrypted = false;  // INFP/CRYP wrapper: a DroneID packet whose record is ciphertext
    int product_code = -1;            // product-type byte of a readable record
    std::string serial;               // serial field of a readable record (self-reported)
    int payload_shift = 0;            // bytes of leading padding before the record (0 normally)
    std::string model;                // table name, empty when the code is not in the table
    std::string model_source;         // "table", "unknown code", or empty when not readable
};

// `format` is the decoder's envelope label for the payload (the raw 176-byte frame). Readability
// is decided here from the record itself: message family 0x10, version 2 or 3, and a passing
// record checksum, at the start of the payload or after up to 8 bytes of leading padding.
// Version 3 records (112 bytes) carry the serial and product-type byte at the same offsets as
// version 2 and are plain text; the vendored parser only accepts version 2.
Info classify(const std::vector<uint8_t>& payload, const std::string& format);

// The record checksum used by DroneID telemetry (matches droneid::dji::packet_crc16): zero over
// a whole record including its trailing checksum bytes.
uint16_t record_crc16(const uint8_t* bytes, std::size_t size);

// Product-type code -> model name, or nullptr. Entries and their source are in the .cpp.
const char* product_name(int code);
std::size_t product_table_size();

// Typed view of one native packet row (Detector::Snapshot::native_packets holds the JSON text).
struct NativePacket {
    std::string protocol;             // "DJI DroneID"
    std::string format;
    int payload_version = 0;
    bool identity_readable = false;
    bool identity_encrypted = false;
    int product_code = -1;
    std::string model, model_source, serial;
    double center_hz = 0, channel_hz = 0, timestamp = 0, cfo_hz = 0, correlation = 0, acquired_steady_s = 0;
    bool inner_crc16 = false;
    int payload_shift = 0;
    std::string channel_selection, receiver_source;
};
// False when the text is not a native packet row. Missing optional fields keep their defaults,
// so rows written before this contract still parse (as not readable).
bool parse_native_packet(const std::string& json, NativePacket& out);

}  // namespace drone::droneid
