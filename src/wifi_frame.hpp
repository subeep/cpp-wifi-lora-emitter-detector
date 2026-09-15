// 802.11 frame-layer decode: PLCP header, FCS, descrambling, and beacon
// MAC/IE parsing. Byte-level logic only - no DSP, no IQ. The PHY side
// (Barker despread, DBPSK demodulation, timing recovery) feeds this.
//
// Split out from wifi_phy.cpp deliberately: everything here is exactly
// verifiable against the published standard with fixed test vectors and
// no radio, which is a very different confidence level from the
// correlators next door. That distinction matters for this project,
// where a prototype's "all tests pass" turned out to mean only that its
// encoder and decoder shared the same wrong tables.
//
// WHY THIS EXISTS: the classifier can say "an OFDM burst happened on
// channel 9", but it cannot say WHICH of the six co-channel networks
// sent it. Beacon-cadence clustering infers a source count but cannot
// name anything. A decoded beacon carries the BSSID and SSID directly -
// real identity rather than inference - and the FCS makes it
// self-validating: if the checksum passes, the decode is correct.
//
// SCOPE: 2.4GHz 802.11b beacons at the 1 Mbps DBPSK basic rate. That is
// deliberately narrow. 5GHz beacons are 6 Mbps OFDM and need an entirely
// different receive chain (64-point FFT, channel estimation,
// deinterleaving, Viterbi) - scoping this as "decode beacons" generally
// would roughly triple it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfmon::wifi {

// CRC-32 as used for the 802.11 FCS: polynomial 0x04C11DB7, reflected
// (LSB-first), init and final XOR 0xFFFFFFFF - the same parameterisation
// as zlib's crc32, so it is checkable against the standard "123456789"
// -> 0xCBF43926 vector.
//
// This is what makes a decoded frame trustworthy. Koopman's tables give
// this polynomial HD=4 out to 2974-bit datawords; a ~232-octet beacon is
// 1856 bits, comfortably inside, so EVERY 1-, 2- and 3-bit error pattern
// is guaranteed caught. Random garbage passes with probability 2^-32
// (2.3e-10). A beacon that checksums is therefore almost certainly
// decoded correctly - precision is not the open question, yield is.
uint32_t fcs32(const uint8_t* data, size_t len);

// The 802.11b PLCP header CRC-16: x^16 + x^12 + x^5 + 1 (0x1021),
// init 0xFFFF, NOT reflected, result ones-complemented.
//
// Note the parameterisation carefully - a reflected 0x8408 with no final
// complement is a different and much more commonly written CRC-16, and
// getting this wrong silently rejects every real frame while still
// passing any round-trip test that shares the same mistake. The spec's
// own worked example (SIGNAL 0x0A, SERVICE 0x00, LENGTH 192) gives
// 0x5B57, which the test suite asserts directly rather than through any
// generator of ours.
uint16_t plcp_crc16(const uint8_t* data, size_t len);

// The 802.11 data scrambler, x^7 + x^4 + 1, in its self-synchronising
// (DSSS) form: each output bit is the input bit XOR the received bits 4
// and 7 positions back. Self-synchronising means the descrambler keys
// off the RECEIVED bits, so it locks on within 7 bits with no seed
// search - the 128-seed brute force some implementations use is
// unnecessary.
//
// The cost of that property: one channel bit error becomes up to three
// output errors (the bit itself plus its two taps), so raw BER
// translates to roughly 2x MPDU BER. With FCS-32 being all-or-nothing
// over ~250 octets, a 1e-4 raw BER already costs ~30% of frames - judge
// a decoder on frames-per-second, not on bit error rate.
std::vector<uint8_t> descramble_bits(const std::vector<uint8_t>& bits);

// Packs MSB-first bits into bytes (802.11 transmits LSB-first within a
// byte; the caller decides which it needs).
std::vector<uint8_t> bits_to_bytes_lsb_first(const std::vector<uint8_t>& bits);

struct PlcpHeader {
    uint8_t signal = 0;    // rate in 100 kbit/s units: 0x0A = 1 Mbps
    uint8_t service = 0;
    uint16_t length_us = 0;  // PSDU duration in microseconds
    uint16_t crc = 0;
    bool crc_valid = false;
    double rate_mbps() const { return double(signal) / 10.0; }
};

// Parses the 48-bit (6 octet) long-preamble PLCP header and checks its
// CRC. Input must already be DESCRAMBLED - the scrambler covers every
// transmitted bit including SYNC, SFD and this header, so parsing raw
// channel bits produces nonsense that happens to work only against a
// generator which also skipped scrambling.
std::optional<PlcpHeader> parse_plcp_header(const uint8_t* bytes, size_t len);

struct BeaconInfo {
    std::string bssid;      // canonical aa:bb:cc:dd:ee:ff
    std::string ssid;       // empty for a hidden/wildcard SSID
    int channel = 0;        // from the DS Parameter Set IE, 0 if absent
    uint16_t beacon_interval_tu = 0;
    uint16_t capability = 0;
    bool fcs_valid = false;
};

// Parses a beacon/probe-response MPDU (including its trailing 4-octet
// FCS) into identity fields.
//
// Two attribution details that are easy to get wrong and produce
// confidently incorrect output rather than an obvious failure:
//
//  - The BSSID is Address 3, NOT Address 2. On 802.11ax multi-BSSID
//    radios those differ, and since virtual BSSIDs on one radio are
//    exactly the case this whole feature exists to resolve, using
//    Address 2 would silently collapse them.
//
//  - The channel comes from the DS Parameter Set IE (tag 3), not from
//    whichever channel we happened to be tuned to. At 20 Msps a capture
//    centred on channel 9 spans 2442-2462MHz, which includes channel
//    11's centre, so adjacent-channel beacons WILL decode here. Trusting
//    the tuned channel would invent co-channel emitters - the precise
//    bug this is meant to fix.
std::optional<BeaconInfo> parse_beacon(const uint8_t* mpdu, size_t len);

}  // namespace rfmon::wifi
