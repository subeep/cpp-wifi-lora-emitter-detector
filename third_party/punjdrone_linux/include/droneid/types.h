// droneid/types.h — shared types and protocol constants.
// Part of the punjdrone DroneID passive receiver. GPL-3.0-or-later.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace droneid {

using cf32 = std::complex<float>;

// Wire / file sample formats we convert from into cf32 internally.
enum class SampleFormat { cf32, sc16, sc8 };

inline const char* to_string(SampleFormat f) {
  switch (f) {
    case SampleFormat::cf32: return "cf32";
    case SampleFormat::sc16: return "sc16";
    case SampleFormat::sc8:  return "sc8";
  }
  return "?";
}

inline bool parse_format(const std::string& s, SampleFormat& out) {
  if (s == "cf32" || s == "fc32") { out = SampleFormat::cf32; return true; }
  if (s == "sc16")                { out = SampleFormat::sc16; return true; }
  if (s == "sc8")                 { out = SampleFormat::sc8;  return true; }
  return false;
}

// Bytes per complex sample on disk / wire.
inline std::size_t bytes_per_sample(SampleFormat f) {
  switch (f) {
    case SampleFormat::cf32: return 8;  // 2x float32
    case SampleFormat::sc16: return 4;  // 2x int16
    case SampleFormat::sc8:  return 2;  // 2x int8
  }
  return 0;
}

// DroneID signal constants — HYPOTHESES to confirm against a real capture (see §2 of the build plan).
// Do not treat as verified until Phase 1 says so.
namespace sig {
constexpr double kChannelRate = 15.36e6;  // samples/s per DroneID channel
constexpr int    kFftSize     = 1024;
constexpr double kScsHz       = 15e3;     // subcarrier spacing
constexpr int    kZcRoot1     = 600;      // Zadoff-Chu sync root #1
constexpr int    kZcRoot2     = 147;      // Zadoff-Chu sync root #2
}  // namespace sig

// A detected OFDM burst. Populated from Phase 1 onward; defined here so interfaces are stable.
struct Burst {
  int64_t t_ns = 0;        // timestamp of first sample
  double  cfo_hz = 0;      // estimated carrier frequency offset
  float   snr_db = 0;
  float   rssi_dbfs = 0;
  uint32_t channel_hz = 0; // center frequency this burst was seen on
  std::vector<cf32> samples;
};

}  // namespace droneid
