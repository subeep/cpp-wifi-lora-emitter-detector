#pragma once
#include "wifi_frame.hpp"
#include <complex>
#include <string>
#include <vector>
namespace rfmon::wifi {
// Header of a non-legacy OFDM PPDU (802.11n HT-SIG, 802.11ac VHT-SIG-A,
// 802.11ax HE-SIG-A), decoded from the 20 MHz primary slice. Fields are
// -1 / false when not carried.
struct HighThroughputSig {
    std::string format;          // "802.11n HT", "802.11ac VHT", "802.11ax HE SU", "... HE TB", "... HE MU"
    bool crc_ok = false;
    int mcs = -1, bandwidth_mhz = 0;
    int streams = 0;                       // spatial streams (N_SS)
    int space_time_streams = 0;            // N_STS: 2 x streams with STBC
    bool ldpc = false, stbc = false, short_gi = false, aggregation = false, dcm = false;
    int ht_length = -1;                    // HT PSDU length (bytes)
    int extension_ltfs = 0;                // HT Ness
    bool nsym_disambiguation = false;      // VHT short-GI symbol-count disambiguation
    bool ldpc_extra_symbol = false;        // VHT: LDPC added an OFDM symbol; HE: LDPC extra symbol segment
    int pre_fec_padding = 0;               // HE: pre-FEC padding factor a (1..4)
    bool pe_disambiguity = false, doppler = false;  // HE SU
    int group_id = -1, partial_aid = -1;   // VHT; group 0 = SU to an AP (partial AID from its BSSID)
    int bss_color = -1, spatial_reuse = -1, txop = -1, gi_ltf = -1;  // HE
    int uplink = -1;                       // HE / VHT group 0: 1 = towards the AP
    std::string summary;                   // one line for tables
};
// Partial AID an 802.11ac station puts in frames to this BSSID (group 0): BSSID bits 39..47.
int vht_partial_aid(const std::string& bssid);
// Why an HT/VHT/HE payload with this header is not decoded; empty when it is
// in scope (802.11n/ac, 20 MHz, one spatial stream, BCC or LDPC, with or
// without STBC).
std::string ht_payload_unsupported_reason(const HighThroughputSig& sig);

// Legacy 20 MHz OFDM payloads, plus 802.11n/ac payloads within
// ht_payload_unsupported_reason(); other formats report their header.
// Diagnostic progress never constitutes decoded identity.
struct OfdmDecodeResult {
    std::string status = "No OFDM preamble";
    bool preamble_found = false, header_valid = false, fcs_valid = false;
    int rate_mbps = 0;                     // legacy rate, or the HT/VHT data rate (rounded) when its payload decoded
    size_t psdu_length = 0, samples_required = 0, ltf_start = 0;
    double cfo_hz = 0, ltf_correlation = 0;
    std::vector<uint8_t> mpdu;             // the (first) FCS-valid MPDU
    std::optional<BeaconInfo> beacon;
    std::optional<HighThroughputSig> sig;  // set when the L-SIG is followed by an HT/VHT/HE header
    // Every FCS-valid MPDU (mpdu is the first; several only from an A-MPDU),
    // the A-MPDU subframes that carried an MPDU, and the HT/VHT data rate.
    std::vector<std::vector<uint8_t>> mpdus;
    int ampdu_subframes = 0;
    double data_rate_mbps = 0;
};
OfdmDecodeResult decode_ofdm_burst(const std::complex<float>* x, size_t n,
    double sample_rate_hz, double capture_center_hz, double channel_center_hz);
}
