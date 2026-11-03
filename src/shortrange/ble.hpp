// Bluetooth Low Energy advertising-channel receiver (LE 1M PHY only):
// GFSK demodulation of a 4 MSps channel, access-address sync, de-whitening,
// CRC-24 check and advertising PDU parsing. Passive: nothing is transmitted,
// connections are not followed and LE 2M / Coded / secondary-channel
// extended advertising are not received.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfmon::shortrange::ble {

constexpr uint32_t kAdvAccessAddress = 0x8E89BED6;
constexpr double kChannelRate = 4e6;   // extracted channel rate: 4 samples / symbol
constexpr double kSymbolRate = 1e6;

// 37 / 38 / 39 for the advertising channel nearest `frequency_hz` (+/-0.5 MHz), else -1.
int adv_channel_index(double frequency_hz);
double adv_channel_frequency(int index);

// Bits are in transmission order (LSB of each byte first).
std::vector<uint8_t> bytes_to_bits(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> bits_to_bytes(const std::vector<uint8_t>& bits);
void whiten_bits(std::vector<uint8_t>& bits, int channel);       // self-inverse
std::vector<uint8_t> crc_bits(const std::vector<uint8_t>& pdu_bits);  // 24 bits as transmitted

struct Packet {
    int channel = -1;
    double frequency_hz = 0;
    size_t sample = 0;          // preamble start, at kChannelRate
    double power_db = -200;     // dBFS over the packet, relative only
    double cfo_hz = 0;          // carrier offset estimated from the access address
    std::vector<uint8_t> pdu;   // header + payload, de-whitened
};
struct DemodStats {
    uint64_t syncs = 0;         // access-address matches (grouped per location)
    uint64_t crc_failures = 0;  // matches whose CRC never verified
};
// Only CRC-valid advertising packets are returned.
std::vector<Packet> demodulate(const std::vector<std::complex<float>>& baseband, int channel, DemodStats* stats = nullptr);

struct Address {
    std::array<uint8_t, 6> b{};  // as received (least significant byte first)
    bool random = false;
    std::string text() const;    // AA:BB:CC:DD:EE:FF, most significant first
    std::string kind() const;    // Public / Random static / Random resolvable / Random non-resolvable
};
struct Field { std::string name, value; };
struct AdStructure { uint8_t type = 0; std::vector<uint8_t> data; };

struct Advert {
    bool valid = false;
    uint8_t type = 0;
    std::string type_name;
    std::optional<Address> advertiser, target, scanner, initiator;
    std::vector<AdStructure> ad;
    std::vector<Field> fields;   // every decoded field in display order
    std::string name;            // advertised local name (complete or shortened)
    std::string company;         // manufacturer data company, if any
    int company_id = -1;
    std::optional<int> tx_power_dbm, appearance;
    std::vector<std::string> services;   // service UUIDs, labelled where known
    std::vector<std::string> tags;       // e.g. iBeacon, Find My, Fast Pair, Remote ID
    std::vector<std::array<uint8_t, 25>> remote_id_messages;  // ASTM F3411 messages carried in service data
};
Advert parse(const std::vector<uint8_t>& pdu);
std::string pdu_type_name(uint8_t type);
std::string hex(const uint8_t* p, size_t n, const char* sep = "");

// Test/validation helper: ideal GFSK (BT 0.5, h 0.5) baseband at
// kChannelRate for a PDU on `channel`, with preamble, access address and CRC.
std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& pdu, int channel);

}  // namespace rfmon::shortrange::ble
