// droneid/dji.h — DJI DroneID decoder: parameters and public API.
//
// Ported from the GPL reference github.com/proto17/dji_droneid (matlab/updated_scripts and
// cpp/remove_turbo.cc). Turbo/rate-match via vendored turbofec (GPLv3); CRC via CRCpp (BSD).
// GPL-3.0-or-later.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "droneid/types.h"

namespace droneid::dji {

// --- OFDM parameters (functions of sample rate), from get_fft_size / get_cyclic_prefix_lengths ---
inline int    fft_size(double fs)      { return static_cast<int>(fs / 15e3 + 0.5); }
inline int    long_cp_len(double fs)   { return static_cast<int>(fs / 192000.0 + 0.5); }
inline int    short_cp_len(double fs)  { return static_cast<int>(0.0000046875 * fs + 0.5); }
constexpr int kDataCarriers = 600;
constexpr int kNumSymbols   = 9;                 // 2 long-CP (sym 1,9), 7 short-CP
constexpr int kZcRootSym4   = 600;
constexpr int kZcRootSym6   = 147;

// Known DroneID center frequencies (Hz), from the reference README.
inline const std::vector<double>& known_channels_24() {
  static const std::vector<double> v = {2.3995e9, 2.4145e9, 2.4295e9, 2.4445e9, 2.4595e9};
  return v;
}

// 0-based indices into an fftshift'd FFT that hold the 600 data carriers (DC excluded).
std::vector<int> data_carrier_indices(double fs);

// Time-domain golden ZC reference for OFDM symbol 4 (root 600) or 6 (root 147). No CP.
std::vector<cf32> create_zc(int fft_size, int symbol_index);

// --- Decoded output ---
struct DecodedBurst {
  int64_t sample_index = 0;   // padded burst-window start in original input-sample units
  double  cfo_hz = 0;         // CFO after CP estimate and joint-ZC integer-carrier correction
  float   corr = 0;           // ZC correlation score [0,1]
  bool    crc_ok = false;
  std::vector<uint8_t> payload;  // 176 raw frame bytes (valid if crc_ok)
};

struct PairedDecodedBurst {
  DecodedBurst decoded;
  int64_t end_sample_index = 0;
  std::vector<cf32> ch0_window;
  std::vector<cf32> ch1_window;
};

// Full telemetry parsed from a CRC-valid payload (fields confirmed/placeheld — see frame.cpp).
struct Telemetry {
  uint8_t  version = 0;
  uint16_t sequence = 0;
  std::string serial;         // ASCII serial number
  double   drone_lat = 0, drone_lon = 0;   // degrees
  double   height_m = 0, altitude_m = 0;
  double   v_north = 0, v_east = 0, v_up = 0;  // m/s
  double   yaw_deg = 0;
  double   pilot_lat = 0, pilot_lon = 0;   // degrees (0 if not yet fixed)
  double   home_lat = 0, home_lon = 0;
  uint64_t gps_time_ms = 0;
  bool     pilot_valid = false, home_valid = false;
  std::string type_name;      // best-effort product id
};

struct DecodeOptions {
  double sample_rate = 15.36e6;
  double freq_offset = 0.0;    // Hz to shift the input before processing (to center a channel)
  float  corr_threshold = 0.5f;
  bool   low_pass = true;      // apply 10 MHz LPF after centering
  size_t max_samples = 0;      // 0 = whole input; otherwise limit original input samples
  bool auto_center = false;
};

// Decode every burst found in a capture file. `fmt` is the on-disk sample format.
std::vector<DecodedBurst> decode_file(const std::string& path, SampleFormat fmt,
                                      const DecodeOptions& opt);

// Decode an in-memory stream, centering/resampling to 15.36 MSPS before detection.
std::vector<DecodedBurst> decode_samples(const std::vector<cf32>& iq, const DecodeOptions& opt);

// DF-3: Shared paired DSP: uses one frequency/centering decision and matched resamplers on
// ch0 and ch1, detects bursts once, and extracts identical intervals from both channels.
std::vector<PairedDecodedBurst> decode_paired_samples(const std::vector<cf32>& ch0,
                                                      const std::vector<cf32>& ch1,
                                                      const DecodeOptions& opt,
                                                      std::vector<cf32>* resampled_ch0_out = nullptr,
                                                      std::vector<cf32>* resampled_ch1_out = nullptr);

// Diagnostic: peak ZC (symbol-4) correlation at each frequency offset in [lo,hi] step.
struct OffsetScore { double offset_hz; float max_corr; int64_t index; };
std::vector<OffsetScore> scan_freq_offsets(const std::vector<cf32>& iq, double fs,
                                           double lo, double hi, double step);

// Parse a CRC-valid 176-byte payload into telemetry.
bool parse_frame(const std::vector<uint8_t>& payload, Telemetry& out);

struct PacketEnvelope {
  std::string format = "unknown";
  size_t logical_bytes = 0;
  bool length_valid = false;
  bool crc16_ok = false;
};

PacketEnvelope inspect_packet(const std::vector<uint8_t>& payload);
uint16_t packet_crc16(const uint8_t* bytes, size_t size);

// --- lower-level pieces (exposed for tests / frame_gen) ---

// LTE Gold scrambler sequence (3GPP 36.211 7.2). x2_init: 31 bits (LSB-first row as in ref).
std::vector<uint8_t> generate_scrambler_seq(int num_bits, const std::array<uint8_t, 31>& x2_init);

// The DroneID scrambler x2 init: bit-reversed 0x12345678 (from process_file.m).
const std::array<uint8_t, 31>& scrambler_x2_init();

// Turbo + rate-match + CRC-24 removal. Input: 7200 descrambled hard bits (0/1).
// Output: 176 bytes; returns true if CRC-24-LTEA zeroed out.
bool turbo_decode_frame(const std::vector<uint8_t>& descrambled_bits_7200,
                        std::vector<uint8_t>& out_176);

// Build a 176-byte frame from user bytes: pad to 173 (91 user + 82 tail), append CRC-24-LTEA.
std::vector<uint8_t> build_frame176(const std::vector<uint8_t>& user_bytes);

// Turbo-encode + rate-match a 176-byte frame to 7200 bits (inverse of turbo_decode_frame).
bool turbo_encode_frame(const std::vector<uint8_t>& frame176, std::vector<uint8_t>& out_bits_7200);

// Build a synthetic time-domain 9-symbol OFDM burst from 7200 scrambled (air) bits. For tests.
std::vector<cf32> synthesize_burst(const std::vector<uint8_t>& scrambled_bits_7200, double fs);

}  // namespace droneid::dji
