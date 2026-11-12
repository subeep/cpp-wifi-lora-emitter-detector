#include "droneid_packet.hpp"

#include <nlohmann/json.hpp>

namespace drone::droneid {
namespace {
struct Product { int code; const char* name; };
// Source: the product-type list in the repository's Python reference decoder
// (src/drones/python/reference/droneid_packet.py, from RUB-SysSec/DroneSecurity).
// Confirmed against recordings so far: 58 (tests/fixtures/drones/dji_mavic_air_2.fc32),
// 63 (dji_mini2.fc32); 58 and 66 again on several aircraft each in DRFF-R2 (Air 2, Air 2S).
// 73 is not in that list: it is the code sent by both DRFF-R2 Mini 3 Pro aircraft (version 3
// records, 11 packets). 87 is the code sent in the RFUAV "DJI MINI 3" recording (version 2
// records, 6 packets, one serial; data/drone_ml/2026-11-12/rfuav_decode_01). Codes seen but not
// tied to a labelled recording, e.g. 84, 88, 90 and 93 in DroneRFb-DIR, are deliberately absent
// and reported as "unknown code".
constexpr Product products[] = {
    {1, "Inspire 1"}, {2, "Phantom 3 Series"}, {3, "Phantom 3 Series"}, {4, "Phantom 3 Std"},
    {5, "M100"}, {6, "ACEONE"}, {7, "WKM"}, {8, "NAZA"}, {9, "A2"}, {10, "A3"},
    {11, "Phantom 4"}, {12, "MG1"}, {14, "M600"}, {15, "Phantom 3 4k"}, {16, "Mavic Pro"},
    {17, "Inspire 2"}, {18, "Phantom 4 Pro"}, {20, "N2"}, {21, "Spark"}, {23, "M600 Pro"},
    {24, "Mavic Air"}, {25, "M200"}, {26, "Phantom 4 Series"}, {27, "Phantom 4 Adv"},
    {28, "M210"}, {30, "M210RTK"}, {31, "A3_AG"}, {32, "MG2"}, {34, "MG1A"},
    {35, "Phantom 4 RTK"}, {36, "Phantom 4 Pro V2.0"}, {38, "MG1P"}, {40, "MG1P-RTK"},
    {41, "Mavic 2"}, {44, "M200 V2 Series"}, {51, "Mavic 2 Enterprise"}, {53, "Mavic Mini"},
    {58, "Mavic Air 2"}, {59, "P4M"}, {60, "M300 RTK"}, {61, "DJI FPV"}, {63, "Mini 2"},
    {64, "AGRAS T10"}, {65, "AGRAS T30"}, {66, "Air 2S"}, {67, "M30"}, {68, "DJI Mavic 3"},
    {69, "Mavic 2 Enterprise Advanced"}, {70, "Mini SE"}, {73, "Mini 3 Pro"}, {87, "Mini 3"}};
constexpr std::size_t product_type_offset = 67;   // version-2 record, after the home position
}  // namespace

const char* product_name(int code) {
    for (const auto& p : products)
        if (p.code == code) return p.name;
    return nullptr;
}

std::size_t product_table_size() { return std::size(products); }

uint16_t record_crc16(const uint8_t* bytes, std::size_t size) {
    uint16_t checksum = 0x3692;
    for (std::size_t i = 0; i < size; ++i) {
        checksum ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            checksum = static_cast<uint16_t>((checksum >> 1) ^ ((checksum & 1) ? 0x8408 : 0));
    }
    return checksum;
}

Info classify(const std::vector<uint8_t>& payload, const std::string& format) {
    Info info;
    info.format = format.empty() ? "unknown" : format;
    // Newer aircraft/firmware wrap the record ("INFP"/"CRYP" after the two header bytes) and its
    // body differs completely between packets of one aircraft: no serial or product code to read.
    info.identity_encrypted = info.format == "infp" || info.format == "cryp";
    if (payload.size() < 3) return info;            // nothing to interpret
    if (payload[1] == 0x10 && (payload[2] == 2 || payload[2] == 3)) info.payload_version = payload[2];
    constexpr std::size_t serial_offset = 7, serial_bytes = 16, min_record = product_type_offset + 3;
    for (std::size_t shift = 0; shift <= 8; ++shift) {
        if (payload.size() < shift + min_record) break;
        const uint8_t* record = payload.data() + shift;
        const std::size_t logical = static_cast<std::size_t>(record[0]) + 3;
        if (record[1] != 0x10 || (record[2] != 2 && record[2] != 3) || logical < min_record ||
            shift + logical > payload.size() || record_crc16(record, logical) != 0) continue;
        std::string serial;
        bool printable = true;
        for (std::size_t i = serial_offset; i < serial_offset + serial_bytes && record[i]; ++i) {
            const char c = static_cast<char>(record[i]);
            if (c < 0x20 || c > 0x7e) {             // printable ASCII, as the vendored parser requires
                printable = false;
                break;
            }
            serial.push_back(c);
        }
        if (!printable || serial.empty()) continue;
        info.identity_readable = true;
        info.payload_version = record[2];
        info.payload_shift = static_cast<int>(shift);
        info.serial = serial;
        info.product_code = record[product_type_offset];
        if (const char* name = product_name(info.product_code)) {
            info.model = name;
            info.model_source = "table";
        } else {
            info.model_source = "unknown code";
        }
        if (shift) info.format = record[2] == 2 ? "v2" : "v3";
        break;
    }
    return info;
}

bool parse_native_packet(const std::string& text, NativePacket& out) {
    out = NativePacket{};
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("protocol") || !j["protocol"].is_string()) return false;
    // Rows pass through a property-tree round trip in the detector, which writes every value as
    // a string, so accept both native JSON types and their string forms.
    const auto text_of = [&](const char* key) -> std::string {
        if (!j.contains(key)) return {};
        const auto& v = j[key];
        return v.is_string() ? v.get<std::string>() : v.is_null() ? std::string{} : v.dump();
    };
    const auto number = [&](const char* key, double fallback) {
        const auto s = text_of(key);
        if (s.empty()) return fallback;
        try { return std::stod(s); } catch (...) { return fallback; }
    };
    const auto flag = [&](const char* key) { return text_of(key) == "true"; };
    out.protocol = text_of("protocol");
    out.format = text_of("envelope");
    out.payload_version = static_cast<int>(number("payload_version", 0));
    out.identity_readable = flag("identity_readable");
    out.identity_encrypted = flag("identity_encrypted");
    out.product_code = static_cast<int>(number("product_code", -1));
    out.model = text_of("model");
    out.model_source = text_of("model_source");
    out.serial = text_of("reported_serial");
    out.center_hz = number("center_hz", 0);
    out.channel_hz = number("channel_hz", 0);
    out.timestamp = number("timestamp", 0);
    out.cfo_hz = number("cfo_hz", 0);
    out.correlation = number("correlation", 0);
    out.acquired_steady_s = number("acquired_steady_s", 0);
    out.inner_crc16 = flag("inner_crc16");
    out.payload_shift = static_cast<int>(number("payload_shift", 0));
    out.channel_selection = text_of("channel_selection");
    out.receiver_source = text_of("receiver_source");
    return out.protocol == "DJI DroneID";
}

}  // namespace drone::droneid
