// Band and scan-plan configuration for the RF emitter monitor.
//
// Sub-GHz (LoRa/IN865) and 2.4GHz Wi-Fi parameters are carried over
// directly from a Python prototype of this tool that was validated
// against a real B210 (serial 3273AC6) on this machine: USB3-clean up
// to 56 Msps, a strong LO-leakage spike sits exactly on every tuned
// center frequency (must be guarded out, not filtered by UHD's own
// dc-offset/iq-balance correction which does not remove it), and a
// broad anti-alias-filter rolloff artifact sits near the Nyquist edge
// of every capture (also must be guarded out). See README.md.
//
// The 5GHz Wi-Fi band is new here. Only the non-DFS sub-bands are
// scanned - UNII-1 (channels 36-48) and UNII-3 (channels 149-165) -
// skipping the DFS-gated UNII-2/2e range (5250-5725 MHz) entirely,
// which real consumer APs use far less often and which would roughly
// triple the number of scan steps for a lot of usually-quiet spectrum.

#pragma once

#include <map>
#include <string>
#include <vector>
#include <optional>

namespace rfmon {

struct ScanStep {
    std::string band;         // "sub_ghz_ism", "wifi_2g4", or "wifi_5g"
    double center_hz;
    double sample_rate_hz;
    std::string label;
};

// Band identifiers double as UI "mode" identifiers - each mode scans
// exactly one band's plan and keeps its own device registry.
constexpr const char* BAND_SUB_GHZ = "sub_ghz_ism";
constexpr const char* BAND_WIFI_2G4 = "wifi_2g4";
constexpr const char* BAND_WIFI_5G = "wifi_5g";

// --- Sub-GHz ISM band (India) ---
// WPC de-licensed 865-867 MHz band used by LoRaWAN IN865, with a few
// MHz of margin since some gateways run channels close to the edges.
constexpr double SUB_GHZ_CENTER_HZ = 865.5e6;
constexpr double SUB_GHZ_SAMPLE_RATE_HZ = 5e6;   // covers ~863.0-868.0 MHz

// --- 2.4 GHz ISM band (Wi-Fi channels 1-13; India does not permit 14) ---
// Centers are deliberately offset from every real 802.11 channel center
// (2412, 2417, ... 2472 MHz, every 5 MHz) - a channel whose own center
// (and much of its energy) sits inside the DC-guard gap is the one case
// the detector's gap-bridging can't recover, so the gap is kept off the
// channel grid entirely rather than relying on the bridge for every
// capture.
inline const std::vector<double> WIFI_2G4_CENTERS_HZ = {2409.5e6, 2434.5e6, 2459.5e6};
constexpr double WIFI_2G4_SAMPLE_RATE_HZ = 56e6;

// --- 5 GHz ISM/UNII band (non-DFS only: UNII-1 + UNII-3) ---
// UNII-1 channels 36/40/44/48 (centers 5180-5240 MHz); UNII-3 channels
// 149/153/157/161/165 (centers 5745-5825 MHz). Centers below are offset
// from that 20MHz channel grid for the same DC-guard reason as 2.4GHz.
inline const std::vector<double> WIFI_5G_CENTERS_HZ = {
    5190e6, 5230e6,               // covers UNII-1 (36-48)
    5755e6, 5795e6, 5835e6,       // covers UNII-3 (149-165)
};
constexpr double WIFI_5G_SAMPLE_RATE_HZ = 56e6;

inline std::vector<ScanStep> scan_plan_for_band(const std::string& band) {
    std::vector<ScanStep> plan;
    if (band == BAND_SUB_GHZ) {
        plan.push_back({BAND_SUB_GHZ, SUB_GHZ_CENTER_HZ, SUB_GHZ_SAMPLE_RATE_HZ,
                         "Sub-GHz ISM 863-868 MHz (LoRa/IN865)"});
    } else if (band == BAND_WIFI_2G4) {
        for (double f : WIFI_2G4_CENTERS_HZ) {
            plan.push_back({BAND_WIFI_2G4, f, WIFI_2G4_SAMPLE_RATE_HZ,
                             "2.4GHz Wi-Fi @ " + std::to_string(int(f / 1e6)) + " MHz"});
        }
    } else if (band == BAND_WIFI_5G) {
        for (double f : WIFI_5G_CENTERS_HZ) {
            plan.push_back({BAND_WIFI_5G, f, WIFI_5G_SAMPLE_RATE_HZ,
                             "5GHz Wi-Fi @ " + std::to_string(int(f / 1e6)) + " MHz"});
        }
    }
    return plan;
}

// 802.11 2.4 GHz channel plan valid in India (channels 1-13).
inline const std::map<int, double>& wifi_2g4_channels() {
    static const std::map<int, double> channels = [] {
        std::map<int, double> m;
        for (int ch = 1; ch <= 13; ++ch) m[ch] = 2412e6 + (ch - 1) * 5e6;
        return m;
    }();
    return channels;
}

// 802.11 5 GHz channel plan, non-DFS only (UNII-1 + UNII-3).
inline const std::map<int, double>& wifi_5g_channels() {
    static const std::map<int, double> channels = {
        {36, 5180e6}, {40, 5200e6}, {44, 5220e6}, {48, 5240e6},
        {149, 5745e6}, {153, 5765e6}, {157, 5785e6}, {161, 5805e6}, {165, 5825e6},
    };
    return channels;
}

// --- Capture ---
// Each scan step takes several short captures and combines them with
// max-hold rather than one long averaged capture, so short/intermittent
// bursts (a single LoRa packet, a Wi-Fi beacon) aren't washed out.
// SUB_CAPTURE_DURATION_S was doubled from its original 0.15s - longer
// per-channel dwell gives intermittent emitters more chance to land in
// a capture window (matters more now on the X310, where each capture
// also has more RFNoC/network round-trip overhead than the B210's USB
// path), at the cost of a longer scan cycle.
constexpr int SUB_CAPTURES_PER_STEP = 4;
constexpr double SUB_CAPTURE_DURATION_S = 0.3;
constexpr double RETUNE_SETTLE_S = 0.1;

// --- Detection ---
constexpr double NOISE_FLOOR_PERCENTILE = 20.0;
constexpr double DEFAULT_DETECTION_THRESHOLD_DB = 12.0;
constexpr int MIN_SEGMENT_BINS = 3;
constexpr int MERGE_GAP_BINS = 2;

// Exclude +/- this fraction of the sample rate around the tuned center
// frequency from detection - the B210's own LO-leakage spike lands
// exactly on the tuned center on every capture, 40-50 dB above the real
// noise floor. uhd's set_rx_dc_offset()/set_rx_iq_balance() do NOT
// remove this reliably (confirmed empirically), so this guard is
// mandatory, not a nice-to-have.
constexpr double DC_GUARD_FRACTION = 0.002;
constexpr double DC_GUARD_MIN_HZ = 5e3;

// Exclude the outer edge of each capture's bandwidth too - a broad
// elevated-noise shelf consistently appears at the same *relative*
// offset (~90-95% of Nyquist) regardless of tuned frequency, the
// signature of anti-alias/decimation filter rolloff, not a real signal.
constexpr double EDGE_GUARD_FRACTION = 0.12;

// --- Classification (bandwidth heuristics - energy detection only,
// so these are best-effort guesses, not a decode) ---
inline const std::map<double, double>& lora_bandwidths_hz() {
    static const std::map<double, double> m = {{125e3, 40e3}, {250e3, 60e3}, {500e3, 100e3}};
    return m;
}
// Lower bound is 8 MHz, not ~18 (a clean 20MHz channel's real occupied
// width), because threshold-based bandwidth measurement shrinks for a
// weaker/farther AP: only the strongest central part of its spectrum
// clears a fixed dB-above-noise threshold. Confirmed on real hardware
// in the Python prototype - a real nearby AP measured at ~11-13 MHz
// above threshold despite being a normal 20MHz 802.11 channel.
constexpr double WIFI_2G4_CHANNEL_BW_LO_HZ = 8e6;
constexpr double WIFI_2G4_CHANNEL_BW_HI_HZ = 25e6;
constexpr double WIFI_5G_CHANNEL_BW_LO_HZ = 15e6;   // wider range: 5GHz commonly bonds to 40/80MHz
constexpr double WIFI_5G_CHANNEL_BW_HI_HZ = 90e6;
constexpr double NARROWBAND_2G4_LO_HZ = 1e6;
constexpr double NARROWBAND_2G4_HI_HZ = 5e6;

// --- Hardware defaults ---
// Fixed gain, not AGC: empirically, AGC was too conservative to surface
// a real nearby Wi-Fi AP that fixed 40dB gain found cleanly. AGC is
// still available (pass std::nullopt for gain_db) if the environment is
// too hot for fixed gain.
constexpr double DEFAULT_GAIN_DB = 40.0;
constexpr const char* ANTENNA = "RX2";
constexpr const char* DEVICE_ARGS = "type=b200";

// --- SDR device selection (B210 or X310) ---
// Both are driven through the same UsrpCapture wrapper (see
// sdr_capture.hpp) - only the UHD device args, antenna name, and gain
// range differ per device, captured here as one DeviceProfile per type.
enum class SdrDeviceType { B210, X310 };

// This lab's X310, reached over a direct Ethernet link (its factory-
// default static IP on the 1GigE management/data port - re-discover
// with `uhd_find_devices` if it's ever changed or moved to a different
// network). Needs the host's UDP send buffer raised first:
//   sudo sysctl -w net.core.wmem_max=50000000
// (UHD logs the exact required value if this is too small; this is a
// standard requirement for X3x0-series devices, not specific to this
// app - see PROJECT_STATUS.md.)
constexpr const char* X310_ADDR = "192.168.10.2";

struct DeviceProfile {
    std::string device_args;
    std::string antenna;
    double default_gain_db;
    double max_gain_db;              // for the UI slider - hardware will clamp regardless
    bool supports_agc;               // UBX (and most non-AD9361 daughterboards) don't
    double max_sample_rate_hz;       // transport-limited - see below
    double lora_listen_capture_rate_hz;  // see below - not always == LORA_LISTEN_SAMPLE_RATE_HZ
};

// Antenna connected to slot A / Radio#0 (channel 0) on this X310, per
// the user's physical setup - see PROJECT_STATUS.md. Both installed
// daughterboards are UBX-160 (10 MHz - 6 GHz, fixed 160MHz analog
// bandwidth), with a PGA0 gain range of 0.0-31.5dB (`uhd_usrp_probe`) -
// much narrower than the B210's, hence the separate default/max here.
// UBX has no AGC at all (UHD throws not_implemented_error - see
// sdr_capture.cpp's try_set_agc) - only the AD9361-based B210 does.
//
// max_sample_rate_hz: the B210's value (56 Msps) was validated over its
// USB3 link in the Python prototype, but this app originally reused
// that same figure for the X310 unconditionally, sizing the request to
// the transport that can carry it - not the one that's actually
// connected. This X310 is on a 1GigE link (~1 Gbps, ~125 MB/s): at
// complex sc16 (4 bytes/sample), 56 Msps is ~1.6 Gbps - well over 10x
// what a 1GbE link can carry. Confirmed as the root cause of a real
// crash: requesting 56 Msps (silently clamped by UHD to 50 Msps, which
// is still far too high) saturated the link, starved the RFNoC control
// channel of bandwidth, and produced sustained "rx xport timed out
// getting a response from mgmt_portal" / "Timed out getting recv buff
// for management transaction" errors that didn't recover - eventually
// even the automatic reconnect's own hardware teardown timed out and
// crashed the process (see UsrpCapture's destructor-exception handling
// in sdr_capture.cpp for that half of the fix). 20 Msps here leaves
// headroom under the ~31 Msps physical ceiling. If this X310 is ever
// moved to a 10GbE (SFP+) link, this can go back up to 56 Msps.
//
// lora_listen_capture_rate_hz: the LoRa PHY codec (lora_phy.hpp) hard-
// assumes capture sample rate == the transmitter's 125kHz channel
// bandwidth (each symbol is exactly 2^SF *samples* - true only when
// those two are equal). The B210 hits LORA_LISTEN_SAMPLE_RATE_HZ
// (125kHz) exactly, so it captures at that rate directly. The X310
// cannot hit 125kHz at all - confirmed empirically, requesting it
// clamps to ~196.08kHz (not a clean multiple of 125kHz, so decimating
// that down still wouldn't reconstruct 125kHz correctly). 250kHz,
// however, lands on *exactly* 250000.0 Hz - a clean 2x multiple - so
// the X310 captures at 250kHz and Scanner::run_lora_listen_step()
// decimates that by 2 (with a basic anti-alias filter, not naive
// sample-dropping) before handing it to the codec, which then sees
// data indistinguishable from a real 125kHz capture.
inline DeviceProfile device_profile(SdrDeviceType type) {
    if (type == SdrDeviceType::X310) {
        return {std::string("addr=") + X310_ADDR, "RX2", 20.0, 31.5, false, 20e6, 250e3};
    }
    return {DEVICE_ARGS, ANTENNA, DEFAULT_GAIN_DB, 70.0, true, 56e6, 125e3};
}

// --- Registry ---
constexpr int DEFAULT_EXPIRE_CYCLES = 4;
constexpr double POWER_EMA_ALPHA = 0.3;

// --- LoRa PHY listen (only runs while BAND_SUB_GHZ is the active mode,
// alongside the wideband energy scan above) - cycles the 3 mandatory
// IN865 uplink channels at the codec's native 125kHz sample rate (no
// resampling needed - the B210 can capture directly at this rate),
// blind-searching SF7-12 against each. Ported from the validated
// Python tools/lora_listen.py in the sibling newrocktest project.
//
// 866.9 MHz is not one of the 3 mandatory IN865 uplink channels - it's
// this lab's TarangMini ST22LR01 demo unit's actual configured Default
// Frequency (read live via its TarangNet config API, cmd 0x0F), added
// so the GUI can show real detections from it. See
// newrocktest/TARANGMINI_LORA_FINDINGS.md for how this was found.
inline const std::vector<double> LORA_LISTEN_CHANNELS_HZ = {865.0625e6, 865.4025e6, 865.985e6,
                                                             866.9e6};
constexpr double LORA_LISTEN_SAMPLE_RATE_HZ = 125e3;
constexpr double LORA_LISTEN_DURATION_S = 2.0;
inline const std::vector<int> LORA_LISTEN_SF_LIST = {5, 6, 7, 8, 9, 10, 11, 12};
constexpr int LORA_PACKET_LOG_MAX = 200;

}  // namespace rfmon
