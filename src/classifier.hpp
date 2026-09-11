// Turn a raw detected Segment into a human-readable protocol guess.
//
// This is energy-detection only - bandwidth and center frequency are
// the only evidence available, so classification is a best-effort
// label, not a decoded protocol identification.

#pragma once

#include <string>

#include "detector.hpp"
#include "wifi_phy.hpp"

namespace rfmon {

// `mod` is only meaningful for the WiFi bands - it's the correlator-
// based result from wifi_phy.hpp, run by the caller (scanner.cpp)
// directly against a WiFi channel's own capture, unconditionally - not
// gated on this segment's bandwidth (see wifi_phy.hpp's file header for
// why: bandwidth is descriptive here, not a detection gate). When mod
// is DSSS/OFDM, that correlator confirmation alone is enough to call it
// "WiFi-like" regardless of the measured bandwidth bucket. Unknown (the
// default, and always what BAND_SUB_GHZ passes) falls back to today's
// original bandwidth-only heuristic label, untouched.
std::string classify(const std::string& band, const Segment& segment,
                      wifi::ModClass mod = wifi::ModClass::Unknown);

}  // namespace rfmon
