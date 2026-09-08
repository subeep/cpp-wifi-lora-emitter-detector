// Turn a raw detected Segment into a human-readable protocol guess.
//
// This is energy-detection only - bandwidth and center frequency are
// the only evidence available, so classification is a best-effort
// label, not a decoded protocol identification.

#pragma once

#include <string>

#include "detector.hpp"

namespace rfmon {

std::string classify(const std::string& band, const Segment& segment);

}  // namespace rfmon
