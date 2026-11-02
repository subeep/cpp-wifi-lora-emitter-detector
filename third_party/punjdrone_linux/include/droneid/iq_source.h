// droneid/iq_source.h — abstract IQ sample source.
// The whole decode pipeline consumes this, so tests run file-in without a radio
// and live mode is the same code with a UsrpSource (Phase 1).
// GPL-3.0-or-later.
#pragma once

#include <cstddef>
#include <cstdint>

#include "droneid/types.h"

namespace droneid {

class IqSource {
 public:
  virtual ~IqSource() = default;

  // Fill up to n cf32 samples into buf. Returns the count written; 0 means
  // end-of-stream (file) or stopped (radio). On return, t_ns holds the timestamp
  // of the first sample in this block if the source knows it, else 0.
  virtual std::size_t recv(cf32* buf, std::size_t n, int64_t& t_ns) = 0;

  virtual double sample_rate() const = 0;
  virtual double center_freq() const { return 0.0; }
  virtual bool   eof() const { return false; }
};

}  // namespace droneid
