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
// based result from wifi_phy.hpp, run by the caller against this
// segment's own slice of the wideband capture (see scanner.cpp).
// Unknown (the default) falls back to today's bandwidth-only label, so
// a candidate the correlators couldn't confirm still shows up exactly
// as it did before this existed - it just doesn't get the extra detail.
std::string classify(const std::string& band, const Segment& segment,
                      wifi::ModClass mod = wifi::ModClass::Unknown);

// Whether `bandwidth_hz` is even plausibly a WiFi channel's width for
// `band` - the same bandwidth gate classify() itself uses to decide
// whether to show a modulation tag at all. Exposed so the caller can
// skip running the (comparatively expensive, and only meaningful for a
// real ~20MHz-wide candidate) correlators entirely on a segment that
// could never qualify anyway - a narrowband spur or CW tone can
// trivially satisfy the Schmidl-Cox periodicity test on its own terms
// (a pure tone is perfectly "periodic" at any period), so this gate
// matters for correctness, not just for saving cycles.
bool bandwidth_looks_like_wifi(const std::string& band, double bandwidth_hz);

}  // namespace rfmon
