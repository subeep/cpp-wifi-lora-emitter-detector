// DroneID packet contract: classification of checksum-valid payloads and the typed row parser.
#include "drones/droneid_packet.hpp"
#include "drones/evidence_claims.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace drone::droneid;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// A 176-byte frame holding one telemetry record with a valid record checksum.
static std::vector<uint8_t> frame(uint8_t version, int product, const char* serial, std::size_t logical, std::size_t shift = 0,
                                  uint8_t family = 0x10) {
    std::vector<uint8_t> p(176, 0);
    uint8_t* r = p.data() + shift;
    r[0] = static_cast<uint8_t>(logical - 3); r[1] = family; r[2] = version;
    std::memcpy(r + 7, serial, std::min<std::size_t>(std::strlen(serial), 16));
    r[67] = static_cast<uint8_t>(product);
    const uint16_t crc = record_crc16(r, logical - 2);
    r[logical - 2] = static_cast<uint8_t>(crc & 0xff); r[logical - 1] = static_cast<uint8_t>(crc >> 8);
    return p;
}

int main() try {
    // Every named product code has a row in the model/link table (evidence_claims.hpp).
    for (int code = 0; code < 256; ++code)
        if (const char* name = product_name(code))
            require(rfmon::drone_claims::model_link(name), (std::string("no link-table row for ") + name).c_str());
    require(std::strcmp(product_name(66), "Air 2S") == 0 && std::strcmp(product_name(58), "Mavic Air 2") == 0 &&
            std::strcmp(product_name(63), "Mini 2") == 0 && std::strcmp(product_name(73), "Mini 3 Pro") == 0 &&
            std::strcmp(product_name(87), "Mini 3") == 0, "known product codes");
    require(!product_name(84) && !product_name(88) && !product_name(90) && !product_name(93) &&
            !product_name(-1) && !product_name(0), "unlisted codes must stay unnamed");
    require(product_table_size() == 51, "product table size changed: update its provenance note");

    auto good = frame(2, 66, "3YTBK0000000AB", 91);
    require(record_crc16(good.data(), 91) == 0, "test frame checksum");
    auto info = classify(good, "v2");
    require(info.identity_readable && info.payload_version == 2 && info.product_code == 66 && info.model == "Air 2S" &&
            info.model_source == "table" && info.serial == "3YTBK0000000AB" && info.payload_shift == 0 && info.format == "v2",
            "readable version 2 with a listed code");
    info = classify(frame(2, 88, "6C5CL0000000CD", 91), "v2");
    require(info.identity_readable && info.product_code == 88 && info.model.empty() && info.model_source == "unknown code",
            "an unlisted code is reported, not guessed");
    // Version 3: a 112-byte record with the serial and product byte at the version-2 offsets.
    info = classify(frame(3, 84, "F67QC0000000EFGH", 112), "v3");
    require(info.identity_readable && info.payload_version == 3 && info.product_code == 84 && info.model.empty() &&
            info.model_source == "unknown code" && info.serial == "F67QC0000000EFGH" && info.format == "v3", "version 3 is readable");
    // The same record after leading padding, which the decoder labels "unknown".
    info = classify(frame(3, 93, "F6Z9C0000000IJ", 112, 6), "unknown");
    require(info.identity_readable && info.payload_shift == 6 && info.payload_version == 3 && info.product_code == 93 &&
            info.serial == "F6Z9C0000000IJ" && info.format == "v3", "padded record is found by its checksum");
    // Anything whose record checksum fails is not readable, whatever the label says.
    auto bad = good; bad[40] ^= 1;
    info = classify(bad, "v2");
    require(!info.identity_readable && info.product_code == -1 && info.serial.empty() && info.model.empty() &&
            info.payload_version == 2, "a failed record checksum is never readable");
    auto binary = frame(2, 66, "\x01\x02\x03\x04\x05\x06\x07\x08", 91);
    require(!classify(binary, "v2").identity_readable, "a non-text serial field is not accepted");
    info = classify(frame(70, 66, "ABCDEFGH", 91, 0, 0x11), "auxiliary_11");
    require(!info.identity_readable && info.payload_version == 0 && info.format == "auxiliary_11", "auxiliary message");
    require(!classify(frame(2, 66, "ABCDEFGH", 40), "v2").identity_readable, "a record too short for a product byte");
    // Encrypted wrappers are DroneID packets with nothing readable in them.
    auto wrapped = std::vector<uint8_t>(176, 0x5a); wrapped[0] = 0x80; wrapped[1] = 0x10; std::memcpy(wrapped.data() + 2, "INFP", 4);
    info = classify(wrapped, "infp");
    require(info.identity_encrypted && !info.identity_readable && info.product_code == -1 && info.serial.empty(), "INFP is encrypted");
    require(classify(wrapped, "cryp").identity_encrypted && !classify(good, "v2").identity_encrypted &&
            !classify(frame(70, 66, "ABCDEFGH", 91, 0, 0x11), "auxiliary_11").identity_encrypted, "only wrappers are encrypted");
    require(!classify({}, "v2").identity_readable && classify({}, "").format == "unknown", "an empty payload is never readable");
    require(!classify(std::vector<uint8_t>(176, 0), "unknown").identity_readable, "all zeros is not a record");

    NativePacket p;
    // As emitted by the worker (native JSON types).
    require(parse_native_packet(R"({"protocol":"DJI DroneID","envelope":"v2","payload_version":2,"identity_readable":true,
        "product_code":66,"model":"Air 2S","model_source":"table","reported_serial":"ABC123","center_hz":2440000000.0,
        "channel_hz":2444500000.0,"timestamp":1.5,"inner_crc16":true,"channel_selection":"documented_droneid_channel"})", p),
        "native row");
    require(p.identity_readable && p.payload_version == 2 && p.product_code == 66 && p.model == "Air 2S" &&
            p.model_source == "table" && p.serial == "ABC123" && p.channel_hz == 2444.5e6 && p.center_hz == 2440e6 &&
            p.timestamp == 1.5 && p.inner_crc16 && p.format == "v2", "native row fields");
    // After the detector's property-tree round trip every value is a string.
    require(parse_native_packet(R"({"protocol":"DJI DroneID","envelope":"v3","payload_version":"3","identity_readable":"false",
        "product_code":"-1","model":"","model_source":"","center_hz":"2440000000","timestamp":"2.25","inner_crc16":"true",
        "receiver_source":"LIVE / run x","acquired_steady_s":"12.5"})", p), "stringified row");
    require(!p.identity_readable && p.payload_version == 3 && p.product_code == -1 && p.model.empty() && p.serial.empty() &&
            p.channel_hz == 0 && p.timestamp == 2.25 && p.acquired_steady_s == 12.5 && p.receiver_source == "LIVE / run x",
            "stringified row fields");
    // A row written before the contract existed is still a packet, just not readable.
    require(parse_native_packet(R"({"protocol":"DJI DroneID","envelope":"v2","reported_serial":"X"})", p) &&
            !p.identity_readable && p.product_code == -1 && p.serial == "X", "pre-contract row");
    require(!parse_native_packet("not json", p) && !parse_native_packet(R"({"candidate":true})", p) &&
            !parse_native_packet(R"({"protocol":"Other"})", p), "non-packet text rejected");
    std::cout << "droneid packet contract passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
