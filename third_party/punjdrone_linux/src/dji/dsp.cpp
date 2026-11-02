// src/dji/dsp.cpp — FFTW-backed DSP helpers for the DJI decoder (with portable radix-2/Bluestein fallback).
// GPL-3.0-or-later.
#include "dsp.hpp"

#if DRONEID_USE_FFTW3
#include <fftw3.h>
#else
#define FFTW_FORWARD (-1)
#define FFTW_BACKWARD 1
#endif

#include <cmath>
#include <complex>
#include <algorithm>
#include <stdexcept>
#include <limits>
#include <array>
#include <map>
#include <mutex>

namespace droneid::dji::dsp {
namespace {

#if DRONEID_USE_FFTW3
// FFTW execution may run concurrently; planning/destruction must be serialized.
// Each processing thread reuses a bounded set of plans keyed by array alignment.
std::mutex& planner_mutex() { static std::mutex mutex; return mutex; }
struct PlanCache {
  std::map<std::array<int,4>,fftwf_plan> plans;
  ~PlanCache() {
    std::lock_guard<std::mutex> lock(planner_mutex());
    for(auto& entry:plans) fftwf_destroy_plan(entry.second);
  }
  fftwf_plan get(int n,int dir,fftwf_complex* in,fftwf_complex* out) {
    std::lock_guard<std::mutex> lock(planner_mutex());
    const std::array<int,4> key{n,dir,fftwf_alignment_of(reinterpret_cast<float*>(in)),
                                    fftwf_alignment_of(reinterpret_cast<float*>(out))};
    auto found=plans.find(key);
    if(found!=plans.end()) return found->second;
    if(plans.size()>=32) {fftwf_destroy_plan(plans.begin()->second);plans.erase(plans.begin());}
    auto plan=fftwf_plan_dft_1d(n,in,out,dir,FFTW_ESTIMATE);
    if(!plan) throw std::runtime_error("FFTW plan allocation failed");
    try {plans.emplace(key,plan);} catch(...) {fftwf_destroy_plan(plan);throw;}
    return plan;
  }
};
#endif

#if !DRONEID_USE_FFTW3
void radix2_fft(std::vector<std::complex<double>>& a, int dir) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  const double sign = (dir < 0) ? -1.0 : 1.0;
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = sign * 2.0 * M_PI / static_cast<double>(len);
    const std::complex<double> wlen(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<double> u = a[i + j];
        const std::complex<double> v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}
#endif

// One-shot FFT (dir = FFTW_FORWARD / FFTW_BACKWARD), no scaling.
std::vector<cf32> run_fft(const std::vector<cf32>& x, int dir) {
  const int n = static_cast<int>(x.size());
  if (n <= 0) return {};
#if DRONEID_USE_FFTW3
  std::vector<cf32> out(n);
  auto* in = reinterpret_cast<fftwf_complex*>(const_cast<cf32*>(x.data()));
  auto* o = reinterpret_cast<fftwf_complex*>(out.data());
  thread_local PlanCache cache;
  fftwf_execute_dft(cache.get(n,dir,in,o),in,o);
  return out;
#else
  if ((n & (n - 1)) == 0) {
    std::vector<std::complex<double>> a(n);
    for (int i = 0; i < n; ++i) a[i] = std::complex<double>(x[i].real(), x[i].imag());
    radix2_fft(a, dir);
    std::vector<cf32> out(n);
    for (int i = 0; i < n; ++i)
      out[i] = cf32(static_cast<float>(a[i].real()), static_cast<float>(a[i].imag()));
    return out;
  }
  size_t m = 1;
  while (m < static_cast<size_t>(2 * n - 1)) m <<= 1;
  const double sign = (dir < 0) ? -1.0 : 1.0;
  std::vector<std::complex<double>> w(n), a(m, {0.0, 0.0}), b(m, {0.0, 0.0});
  for (int k = 0; k < n; ++k) {
    const double ang = sign * M_PI * static_cast<double>(1LL * k * k % (2LL * n)) / static_cast<double>(n);
    w[k] = std::complex<double>(std::cos(ang), std::sin(ang));
    a[k] = std::complex<double>(x[k].real(), x[k].imag()) * w[k];
    b[k] = std::conj(w[k]);
    if (k > 0) b[m - k] = b[k];
  }
  radix2_fft(a, -1);
  radix2_fft(b, -1);
  for (size_t i = 0; i < m; ++i) a[i] *= b[i];
  radix2_fft(a, 1);
  std::vector<cf32> out(n);
  const double inv_m = 1.0 / static_cast<double>(m);
  for (int k = 0; k < n; ++k) {
    const std::complex<double> val = a[k] * inv_m * w[k];
    out[k] = cf32(static_cast<float>(val.real()), static_cast<float>(val.imag()));
  }
  return out;
#endif
}

}  // namespace

std::vector<cf32> fft(const std::vector<cf32>& x) { return run_fft(x, FFTW_FORWARD); }

std::vector<cf32> ifft(const std::vector<cf32>& x) {
  auto out = run_fft(x, FFTW_BACKWARD);
  const float s = 1.0f / static_cast<float>(x.size());
  for (auto& v : out) v *= s;
  return out;
}

std::vector<cf32> fftshift(const std::vector<cf32>& x) {
  const size_t n = x.size();
  const size_t h = n / 2;  // even n
  std::vector<cf32> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = x[(i + h) % n];
  return out;
}

void freq_shift(std::vector<cf32>& x, double freq, double fs, double n0) {
  const double w = 2.0 * M_PI * freq / fs;
  for (size_t i = 0; i < x.size(); ++i) {
    const double ph = w * (n0 + static_cast<double>(i));
    x[i] *= cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
  }
}

std::vector<cf32> fir_lowpass(const std::vector<cf32>& x, double cutoff_hz, double fs, int taps) {
  // Windowed-sinc (Hamming), normalized cutoff fc = cutoff/(fs/2) matching fir1(taps, Wn).
  const double fc = cutoff_hz / (fs / 2.0);
  const int N = taps + 1;
  std::vector<double> h(N);
  double sum = 0;
  for (int i = 0; i < N; ++i) {
    const double m = i - (N - 1) / 2.0;
    const double sinc = (m == 0.0) ? fc : std::sin(M_PI * fc * m) / (M_PI * m);
    const double win = 0.54 - 0.46 * std::cos(2 * M_PI * i / (N - 1));
    h[i] = sinc * win;
    sum += h[i];
  }
  for (double& v : h) v /= sum;  // unity DC gain

  std::vector<cf32> y(x.size(), cf32(0, 0));
  for (size_t n = 0; n < x.size(); ++n) {
    cf32 acc(0, 0);
    const int kmax = std::min<int>(N, static_cast<int>(n) + 1);
    for (int k = 0; k < kmax; ++k) acc += x[n - k] * static_cast<float>(h[k]);
    y[n] = acc;
  }
  return y;
}

std::vector<float> normalized_corr(const std::vector<cf32>& x, const std::vector<cf32>& zc) {
  const int M = static_cast<int>(zc.size());
  const int N = static_cast<int>(x.size());
  if (N < M) return {};

  // Zero-mean template and its sample variance.
  cf32 mean(0, 0);
  for (const auto& v : zc) mean += v;
  mean /= static_cast<float>(M);
  std::vector<cf32> h(M);
  double var_filter = 0;
  for (int k = 0; k < M; ++k) {
    h[k] = zc[k] - mean;
    var_filter += std::norm(h[k]);
  }
  var_filter /= (M - 1);

  // corr[i] = sum_k x[i+k] * conj(h[k]) via linear convolution with reversed-conj kernel.
  std::vector<cf32> grev(M);
  for (int k = 0; k < M; ++k) grev[k] = std::conj(h[M - 1 - k]);

  // Overlap-add convolution, FFT size = next pow2 >= B + M - 1.
  const int B = 8192;
  int L = 1;
  while (L < B + M - 1) L <<= 1;

  std::vector<cf32> kpad(L, cf32(0, 0));
  for (int k = 0; k < M; ++k) kpad[k] = grev[k];
  const std::vector<cf32> K = fft(kpad);

  const int conv_len = N + M - 1;
  std::vector<cf32> conv(conv_len, cf32(0, 0));
  std::vector<cf32> block(L);
  for (int start = 0; start < N; start += B) {
    const int blen = std::min(B, N - start);
    for (int i = 0; i < blen; ++i) block[i] = x[start + i];
    for (int i = blen; i < L; ++i) block[i] = cf32(0, 0);
    std::vector<cf32> Xb = fft(block);
    for (int i = 0; i < L; ++i) Xb[i] *= K[i];
    std::vector<cf32> yb = ifft(Xb);
    const int out_len = blen + M - 1;
    for (int i = 0; i < out_len && start + i < conv_len; ++i) conv[start + i] += yb[i];
  }

  // Running energy and sum over the M-window via prefix sums.
  std::vector<double> p2(N + 1, 0.0);
  std::vector<cf32> p1(N + 1, cf32(0, 0));
  double peak_power = 0;
  for (int i = 0; i < N; ++i) {
    p2[i + 1] = p2[i] + std::norm(x[i]);
    p1[i + 1] = p1[i] + x[i];
    peak_power = std::max(peak_power, static_cast<double>(std::norm(x[i])));
  }
  const double precision = std::numeric_limits<float>::epsilon();
  const double energy_floor = 64 * precision * precision * M * peak_power;

  const int outN = N - M + 1;
  std::vector<float> score(outN, 0.0f);
  for (int i = 0; i < outN; ++i) {
    const double E = p2[i + M] - p2[i];
    if (E <= energy_floor) continue;
    const cf32 S = p1[i + M] - p1[i];
    double var_w = (E - std::norm(S) / M) / (M - 1);
    if (var_w <= 0) { score[i] = 0; continue; }
    const cf32 c = conv[i + M - 1];  // == corr[i]
    const double num = std::norm(c);
    const double den = static_cast<double>(M) * M * var_w * var_filter;
    score[i] = den > 0 ? static_cast<float>(std::min(1.0, num / den)) : 0.0f;
  }
  return score;
}

double estimate_channel_center(const std::vector<cf32>& samples, double sample_rate) {
  constexpr size_t transform_size = 8192;
  if (samples.size() < transform_size) return 0;
  const size_t band_bins = std::min(transform_size,
      static_cast<size_t>(std::round(10e6 * transform_size / sample_rate)));
  std::vector<double> power(transform_size, 0);
  std::vector<cf32> window(transform_size);
  for (size_t offset = 0; offset + transform_size <= samples.size(); offset += transform_size) {
    for (size_t sample = 0; sample < transform_size; ++sample) {
      const float weight = 0.5 - 0.5 * std::cos(2 * M_PI * sample / (transform_size - 1));
      window[sample] = samples[offset + sample] * weight;
    }
    const auto spectrum = fftshift(fft(window));
    for (size_t bin = 0; bin < transform_size; ++bin) power[bin] += std::norm(spectrum[bin]);
  }
  double energy = 0;
  for (size_t bin = 0; bin < band_bins; ++bin) energy += power[bin];
  double best_energy = energy;
  size_t best_start = 0;
  for (size_t start = 1; start + band_bins <= transform_size; ++start) {
    energy += power[start + band_bins - 1] - power[start - 1];
    if (energy > best_energy) { best_energy = energy; best_start = start; }
  }
  if (best_energy <= 0) return 0;
  return (best_start + (band_bins - 1) / 2.0 - transform_size / 2.0) *
         sample_rate / transform_size;
}

ChannelResampler::ChannelResampler(double input_rate, double frequency_shift, double output_rate)
    : input_rate_(input_rate), output_rate_(output_rate), frequency_shift_(frequency_shift) {
  if (!std::isfinite(input_rate) || !std::isfinite(output_rate) ||
      input_rate < 10e6 || input_rate > 250e6 || output_rate < 10e6 || output_rate > 250e6 ||
      !std::isfinite(frequency_shift) || std::abs(frequency_shift) >= input_rate / 2)
    throw std::invalid_argument("resampler: rates must be 10–250 MSPS and shift inside input Nyquist");
  radius_ = static_cast<int>(std::ceil(24 * std::max(1.0, input_rate / output_rate)));
  if (input_rate == output_rate) return;
  const double cutoff = 0.4 * std::min(input_rate, output_rate) / input_rate;
  coefficients_.resize(1024, std::vector<float>(2 * radius_ + 1));
  for (size_t phase = 0; phase < coefficients_.size(); ++phase) {
    double sum = 0;
    for (int tap = -radius_; tap <= radius_; ++tap) {
      const double distance = tap - static_cast<double>(phase) / coefficients_.size();
      const double sinc = std::abs(distance) < 1e-12 ? 2 * cutoff :
          std::sin(2 * M_PI * cutoff * distance) / (M_PI * distance);
      const double position = distance / (radius_ + 1);
      const double window = 0.42 + 0.5 * std::cos(M_PI * position) +
                            0.08 * std::cos(2 * M_PI * position);
      const double weight = sinc * window;
      coefficients_[phase][tap + radius_] = static_cast<float>(weight);
      sum += weight;
    }
    for (float& weight : coefficients_[phase]) weight /= sum;
  }
}

std::vector<cf32> ChannelResampler::process(const std::vector<cf32>& input, bool final) {
  auto shifted = input;
  if (frequency_shift_ != 0) freq_shift(shifted, frequency_shift_, input_rate_, received_);
  received_ += static_cast<int64_t>(input.size());
  if (input_rate_ == output_rate_) { emitted_ += input.size(); return shifted; }
  buffer_.insert(buffer_.end(), shifted.begin(), shifted.end());
  std::vector<cf32> output;
  const double step = input_rate_ / output_rate_;
  while (emitted_ * step < received_) {
    const double position = emitted_ * step;
    const int64_t center = static_cast<int64_t>(std::floor(position));
    if (!final && center + radius_ >= received_) break;
    const size_t phase = std::min<size_t>(1023, static_cast<size_t>((position - center) * 1024));
    cf32 value(0, 0);
    for (int tap = -radius_; tap <= radius_; ++tap) {
      const int64_t index = center + tap;
      if (index >= 0 && index < received_)
        value += buffer_[index - buffer_start_] * coefficients_[phase][tap + radius_];
    }
    output.push_back(value);
    ++emitted_;
  }
  const int64_t retain_from = std::max<int64_t>(0, static_cast<int64_t>(std::floor(emitted_ * step)) - radius_);
  const int64_t discard = std::min<int64_t>(buffer_.size(), retain_from - buffer_start_);
  buffer_.erase(buffer_.begin(), buffer_.begin() + discard);
  buffer_start_ += discard;
  return output;
}

}  // namespace droneid::dji::dsp
