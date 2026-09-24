// 802.11b 1 Mbps DSSS receive chain: IQ in, decoded beacon out.
//
// This is the PHY half of beacon decoding; wifi_frame.hpp is the byte
// half. The split matters because the two have very different
// confidence levels - everything in wifi_frame is checkable against
// published vectors with no radio, whereas everything here can only
// really be proven against real air.
//
// Chain: resample to an exact chip grid -> Barker-11 despread with a
// timing search -> DBPSK differential demodulation -> descramble ->
// locate SYNC/SFD -> PLCP header (CRC-16) -> PSDU -> beacon parse
// (FCS-32).
//
// WHY 1 Mbps SPECIFICALLY: beacons are sent at the lowest basic rate,
// which on 2.4GHz is normally 1 Mbps DBPSK, and that rate is the
// simplest thing in all of 802.11 to decode - Barker spreading with no
// forward error correction at all, just scrambling. 2 Mbps is DQPSK and
// 5.5/11 Mbps are CCK, neither of which this handles; 5GHz has no DSSS
// whatsoever and needs a completely different OFDM chain.
//
// WHAT THIS BUYS: the classifier can say a burst happened on a channel;
// beacon cadence can infer how many sources are there. Only this can say
// WHICH networks they are, by name and BSSID - and the FCS makes the
// answer self-validating rather than inferred.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wifi_frame.hpp"

namespace rfmon::wifi {

struct DsssDecodeResult {
    bool preamble_found = false;       // SYNC run + SFD located
    std::optional<PlcpHeader> plcp;    // present once the header parsed
    std::optional<BeaconInfo> beacon;  // present only for beacon/probe-response
    size_t mpdu_len = 0;
    int chip_phase = 0;  // winning timing offset, for diagnostics

    // General MPDU exposure for the passive security monitor
    // (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package A step 3).
    // `mpdu` holds the recovered PSDU octets including the trailing FCS
    // whenever the PLCP header passed its CRC at the one rate this chain
    // demodulates and the whole PSDU fitted in the burst window - for ANY
    // frame type, not just beacons. `fcs_valid` is checked here directly,
    // independently of parse_beacon(); bytes alone are not acceptance, so
    // consumers must gate on it. `beacon` above is unchanged: still only an
    // FCS-valid beacon/probe response.
    std::vector<uint8_t> mpdu;
    bool rate_supported = false;  // PLCP SIGNAL 0x0A: 1 Mbps DBPSK, long preamble
    bool psdu_complete = false;
    bool fcs_valid = false;
    // Furthest stage reached, for decode-yield diagnostics - mirrors
    // OfdmDecodeResult::status. Never an identity claim.
    std::string status = "No DSSS preamble";

    // For src/wifi_fingerprint.cpp - only populated when preamble_found.
    // Every Barker-despread symbol (the complex correlator output
    // BEFORE the differential-decode step turns it into bits) from the
    // confirmed all-ones SYNC run immediately preceding the matched
    // SFD - the most reliably-validated stretch of the frame available
    // at this point in the chain. One microsecond apart always (one
    // Barker symbol at the fixed 11 Mchip/s spreading rate this whole
    // file operates at, regardless of the original capture's sample
    // rate - see kChipRateHz), which is enough on its own to derotate
    // by a CFO estimate without any other timing metadata.
    //
    // These symbols are enough to reconstruct an absolute (up to one
    // unknown but fixed starting phase - which the widely-linear fit's
    // own complex `mu` absorbs, see wifi_fingerprint.cpp) BPSK reference
    // sequence for the IQ-imbalance/DC-offset fit, without needing to
    // know the self-synchronising scrambler's state: DBPSK encodes
    // bit=1 as a 180-degree flip and bit=0 as no change, so
    // re-integrating the symbol-to-symbol differential decisions (the
    // same one-line formula decode_dsss_burst() already uses to build
    // raw_bits, just not re-exposed here to avoid a second copy that
    // could fall out of sync with this one) recovers the absolute phase
    // sequence up to that one constant - a standard DBPSK operation.
    std::vector<std::complex<double>> sync_symbols;
};

// Attempts a full 1 Mbps DSSS decode of one burst.
//
// `x`/`n` is a single burst window (see wifi::detect_bursts) at
// `sample_rate_hz`, already mixed so the channel sits at baseband.
// Returns what it managed to recover - preamble only, header only, or a
// full beacon - rather than all-or-nothing, so a partial result is still
// diagnostic rather than silently indistinguishable from "no signal".
//
// Nothing here trusts itself: the PLCP header is accepted only on its
// CRC-16 and the MPDU only on its FCS-32, so a returned beacon is
// essentially certainly correct. The open question for this chain is
// never precision, it is yield.
// `capture_center_hz`/`segment_center_hz` describe where the channel
// sits inside the capture, exactly as classify_modulation() takes them;
// leaving both at 0 means the input is already at baseband.
DsssDecodeResult decode_dsss_burst(const std::complex<float>* x, size_t n,
                                    double sample_rate_hz, double capture_center_hz = 0.0,
                                    double segment_center_hz = 0.0);

}  // namespace rfmon::wifi
