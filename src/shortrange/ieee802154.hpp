// IEEE 802.15.4 2.4 GHz O-QPSK receiver (Zigbee, Thread and other
// 802.15.4 networks) for a 4 MSps channel, plus MAC, beacon and Zigbee NWK
// header parsing. O-QPSK with half-sine chips is demodulated as MSK: the
// phase change across each chip is correlated with the 16 symbol templates.
// Passive only. Encrypted NWK/MAC payloads stay opaque; no key handling.
#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfmon::shortrange::ieee802154 {

constexpr double kChannelRate = 4e6;  // 2 samples per chip
constexpr double kChipRate = 2e6;

int channel_index(double frequency_hz);  // 11..26 within +/-0.5 MHz, else -1
double channel_frequency(int channel);

uint16_t fcs(const uint8_t* p, size_t n);  // CRC-16/KERMIT, transmitted LSB first

struct Frame {
    int channel = -1;
    double frequency_hz = 0;
    size_t sample = 0;        // SFD end / PHR start, at kChannelRate
    double power_db = -200;   // relative dBFS
    double cfo_hz = 0;
    std::vector<uint8_t> psdu;  // MAC frame including the 2-byte FCS
};
struct DemodStats {
    uint64_t syncs = 0;          // preamble + SFD found
    uint64_t fcs_failures = 0;   // frames whose FCS did not verify
};
// Only FCS-valid frames are returned.
std::vector<Frame> demodulate(const std::vector<std::complex<float>>& baseband, int channel, DemodStats* stats = nullptr);

struct Address {
    int mode = 0;            // 0 none, 2 short, 3 extended
    uint16_t short_addr = 0;
    uint64_t ext = 0;
    std::string text() const;
};
struct Field { std::string name, value; };
struct Mac {
    bool valid = false;
    int type = 0;
    std::string type_name, summary;   // summary: e.g. "Beacon", "MAC command: Data request"
    bool security = false, pending = false, ack_request = false, pan_compression = false;
    int version = 0;
    std::optional<uint8_t> sequence;
    std::optional<uint16_t> dst_pan, src_pan;
    Address dst, src;
    std::string network;      // "Zigbee", "6LoWPAN / Thread", "" (unknown)
    // Zigbee network layer (unencrypted header fields only).
    std::optional<uint16_t> nwk_src, nwk_dst;
    std::optional<uint64_t> nwk_src_ext, nwk_dst_ext;
    bool nwk_security = false;
    // Beacon / association details.
    bool pan_coordinator = false, association_permit = false, is_beacon = false;
    std::optional<uint64_t> zigbee_ext_pan;
    std::optional<int> stack_profile;
    std::string role_hint;    // e.g. "PAN coordinator", "Router (beacon)", "Joining device"
    std::vector<Field> fields;
};
Mac parse(const std::vector<uint8_t>& psdu);  // psdu includes the FCS
std::string eui64_text(uint64_t v);           // AA:BB:...:HH, most significant first

// Test/validation helper: ideal O-QPSK half-sine baseband at kChannelRate
// for a PSDU (FCS included), with preamble, SFD and PHR.
std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& psdu);

}  // namespace rfmon::shortrange::ieee802154
