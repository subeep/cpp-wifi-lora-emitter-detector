// droneid/src/sdr/file_source.cpp
// GPL-3.0-or-later.
#include "droneid/file_source.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>

#include "droneid/logging.h"

namespace droneid {
namespace {

// Load a "<path>.meta" sidecar if present. Keys: format, sample_rate, center_freq.
void load_sidecar(const std::string& path, SampleFormat& fmt, double& rate, double& center) {
  std::ifstream in(std::filesystem::u8path(path + ".meta"));
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    auto trim = [](std::string& s) {
      const char* ws = " \t\r\n\"";
      const auto b = s.find_first_not_of(ws);
      const auto e = s.find_last_not_of(ws);
      s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    };
    trim(k);
    trim(v);
    if (k == "format")           parse_format(v, fmt);
    else if (k == "sample_rate") rate = std::strtod(v.c_str(), nullptr);
    else if (k == "center_freq") center = std::strtod(v.c_str(), nullptr);
  }
  LOG_INFO("FileSource: loaded sidecar %s.meta (format=%s rate=%.0f center=%.0f)",
           path.c_str(), to_string(fmt), rate, center);
}

}  // namespace

FileSource::FileSource(const std::string& path, SampleFormat fmt, double sample_rate,
                       double center_freq, bool loop)
    : fmt_(fmt), sample_rate_(sample_rate), center_freq_(center_freq), loop_(loop) {
  load_sidecar(path, fmt_, sample_rate_, center_freq_);
#ifdef _WIN32
  fp_ = _wfopen(std::filesystem::u8path(path).c_str(), L"rb");
#else
  fp_ = std::fopen(path.c_str(), "rb");
#endif
  if (!fp_) throw std::runtime_error("FileSource: cannot open " + path);
}

FileSource::~FileSource() {
  if (fp_) std::fclose(fp_);
}

std::size_t FileSource::recv(cf32* buf, std::size_t n, int64_t& t_ns) {
  t_ns = 0;
  if (eof_ || n == 0) return 0;

  const std::size_t bps = bytes_per_sample(fmt_);
  raw_.resize(n * bps);
  std::size_t got = std::fread(&raw_[0], 1, n * bps, fp_);
  std::size_t samples = got / bps;

  if (samples == 0) {
    if (loop_ && std::fseek(fp_, 0, SEEK_SET) == 0) {
      got = std::fread(&raw_[0], 1, n * bps, fp_);
      samples = got / bps;
    }
    if (samples == 0) {
      eof_ = true;
      return 0;
    }
  }

  const auto* p = reinterpret_cast<const std::uint8_t*>(raw_.data());
  switch (fmt_) {
    case SampleFormat::cf32: {
      const auto* f = reinterpret_cast<const float*>(p);
      for (std::size_t i = 0; i < samples; ++i)
        buf[i] = cf32(f[2 * i], f[2 * i + 1]);
      break;
    }
    case SampleFormat::sc16: {
      const auto* s = reinterpret_cast<const std::int16_t*>(p);
      constexpr float kScale = 1.0f / 32768.0f;
      for (std::size_t i = 0; i < samples; ++i)
        buf[i] = cf32(s[2 * i] * kScale, s[2 * i + 1] * kScale);
      break;
    }
    case SampleFormat::sc8: {
      const auto* s = reinterpret_cast<const std::int8_t*>(p);
      constexpr float kScale = 1.0f / 128.0f;
      for (std::size_t i = 0; i < samples; ++i)
        buf[i] = cf32(s[2 * i] * kScale, s[2 * i + 1] * kScale);
      break;
    }
  }

  samples_read_ += samples;
  return samples;
}

}  // namespace droneid
