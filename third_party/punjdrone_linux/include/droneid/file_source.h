// droneid/file_source.h — IqSource backed by a raw interleaved IQ file.
// Supports cf32 (float32 I/Q), sc16 (int16 I/Q) and sc8 (int8 I/Q), converting to cf32.
// GPL-3.0-or-later.
#pragma once

#include <cstdio>
#include <string>

#include "droneid/iq_source.h"

namespace droneid {

class FileSource : public IqSource {
 public:
  // rate/center are metadata only (no resampling here). If a sidecar "<path>.meta"
  // exists with key=value lines (format, sample_rate, center_freq), it overrides args.
  FileSource(const std::string& path, SampleFormat fmt, double sample_rate,
             double center_freq = 0.0, bool loop = false);
  ~FileSource() override;

  std::size_t recv(cf32* buf, std::size_t n, int64_t& t_ns) override;
  double sample_rate() const override { return sample_rate_; }
  double center_freq() const override { return center_freq_; }
  bool   eof() const override { return eof_; }

  // Total samples read so far.
  std::uint64_t samples_read() const { return samples_read_; }

 private:
  bool refill_bytes(std::size_t want_samples);

  std::FILE* fp_ = nullptr;
  SampleFormat fmt_;
  double sample_rate_;
  double center_freq_;
  bool loop_;
  bool eof_ = false;
  std::uint64_t samples_read_ = 0;
  std::string raw_;  // scratch byte buffer for one recv()
};

}  // namespace droneid
