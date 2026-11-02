#include "droneid/o4_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <sstream>
#include <stdexcept>

namespace droneid {
namespace {
double median4(std::array<double, 4> values) {
  std::sort(values.begin(), values.end());
  return (values[1] + values[2]) / 2;
}
}

std::string O4Evidence::to_jsonl() const {
  std::ostringstream out;
  out.precision(10);
  out << "{\"event\":\"o4_evidence\",\"profile\":\"" << kO4Profile << "\","
      << "\"classification\":\"" << (candidate ? "o4_candidate" : "unknown_rf")
      << "\",\"identity_confirmed\":false,\"start_sample\":" << start_sample
      << ",\"sample_count\":" << sample_count
      << ",\"cp_correlation\":" << cp_correlation
      << ",\"lag_prominence\":" << lag_prominence
      << ",\"cyclic_correlation\":" << cyclic_correlation
      << ",\"cyclic_prominence\":" << cyclic_prominence
      << ",\"harmonic_correlation\":" << harmonic_correlation
      << ",\"clipped_fraction\":" << clipped_fraction
      << ",\"supporting_windows\":" << supporting_windows
      << ",\"weak_supporting_windows\":" << weak_supporting_windows
      << ",\"weak_structure_match\":" << (weak_structure_match ? "true" : "false")
      << ",\"structure_match\":" << (structure_match ? "true" : "false")
      << ",\"candidate\":" << (candidate ? "true" : "false")
      << ",\"reason\":\"" << reason << "\"}";
  return out.str();
}

O4CandidateDetector::O4CandidateDetector(double rate) : rate_(rate) {
  if (!std::isfinite(rate) || rate < 10e6 || rate > 250e6)
    throw std::invalid_argument("O4 evidence requires a sample rate in [10,250] MSPS");
  window_size_ = static_cast<std::size_t>(std::llround(rate * .016));
  pending_.reserve(window_size_);
}

void O4CandidateDetector::reset() {
  pending_.clear();
  history_.clear();
  weak_history_.clear();
  positioned_ = false;
}

O4Evidence O4CandidateDetector::analyze() const {
  O4Evidence result;
  result.start_sample = window_start_;
  result.sample_count = pending_.size();
  std::complex<double> mean(0, 0);
  for (const auto x : pending_) {
    mean += std::complex<double>(x);
    if (std::abs(x.real()) >= .999 || std::abs(x.imag()) >= .999)
      result.clipped_fraction += 1;
  }
  mean /= pending_.size();
  result.clipped_fraction /= pending_.size();
  if (result.clipped_fraction > .001) {
    result.reason = "clipping";
    return result;
  }
  const auto lag = static_cast<std::size_t>(std::llround(rate_ / 30000));
  const auto d1 = static_cast<std::size_t>(std::llround(rate_ * 1e-6));
  const auto d2 = static_cast<std::size_t>(std::llround(rate_ * 2e-6));
  const auto count = pending_.size() - lag - d2;
  const std::array<std::size_t, 5> lags{lag, lag-d2, lag-d1, lag+d1, lag+d2};
  std::array<std::complex<double>, 5> correlations{};
  std::array<std::complex<double>, 6> cycles{}, oscillators{}, rotations{};
  const std::array<double, 6> frequencies{26000,27000,28000,29000,30000,56000};
  for (std::size_t k = 0; k < cycles.size(); ++k) {
    oscillators[k] = 1;
    rotations[k] = std::polar(1., -2 * std::acos(-1.) * frequencies[k] / rate_);
  }
  double power0 = 0, power1 = 0;
  for (std::size_t n = 0; n < count; ++n) {
    const auto a = std::complex<double>(pending_[n]) - mean;
    const auto b = std::complex<double>(pending_[n+lag]) - mean;
    power0 += std::norm(a);
    power1 += std::norm(b);
    const auto product = std::conj(a) * b;
    correlations[0] += product;
    for (std::size_t k = 1; k < lags.size(); ++k)
      correlations[k] += std::conj(a) * (std::complex<double>(pending_[n+lags[k]]) - mean);
    for (std::size_t k = 0; k < cycles.size(); ++k) {
      cycles[k] += product * oscillators[k];
      oscillators[k] *= rotations[k];
    }
  }
  const double norm = std::sqrt(power0 * power1);
  if (norm <= 1e-20) { result.reason = "no_signal"; return result; }
  result.cp_correlation = std::abs(correlations[0]) / norm;
  const double lag_floor = median4({std::abs(correlations[1]), std::abs(correlations[2]),
                                    std::abs(correlations[3]), std::abs(correlations[4])}) / norm;
  result.lag_prominence = result.cp_correlation / std::max(lag_floor, 1e-6);
  result.cyclic_correlation = std::abs(cycles[2]) / norm;
  const double cyclic_floor = median4({std::abs(cycles[0]), std::abs(cycles[1]),
                                       std::abs(cycles[3]), std::abs(cycles[4])}) / norm;
  result.cyclic_prominence = result.cyclic_correlation / std::max(cyclic_floor, 1e-6);
  result.harmonic_correlation = std::abs(cycles[5]) / norm;
  result.structure_match = result.cp_correlation > .03 && result.lag_prominence > 3 &&
      result.cyclic_correlation > .02 && result.cyclic_prominence > 1.5 &&
      result.harmonic_correlation > .01;
  if (result.structure_match) result.reason = "awaiting_persistence";
  // Low-amplitude normalized evidence needs stricter cyclic prominence and
  // 8/10 support (160 ms), independently of the original 3/5 strong path.
  result.weak_structure_match = result.cp_correlation > .015 && result.lag_prominence > 3 &&
      result.cyclic_correlation > .01 && result.cyclic_prominence > 2 &&
      result.harmonic_correlation > .005;
  if (!result.structure_match && result.weak_structure_match) result.reason = "awaiting_weak_persistence";
  return result;
}

std::vector<O4Evidence> O4CandidateDetector::feed(const cf32* samples, std::size_t count,
                                               uint64_t start_sample) {
  if (!samples && count) throw std::invalid_argument("null O4 detector input");
  if (count > UINT64_MAX - start_sample) throw std::invalid_argument("sample index overflow");
  if (positioned_ && start_sample != expected_sample_) reset();
  positioned_ = true;
  std::vector<O4Evidence> results;
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(samples[i].real()) || !std::isfinite(samples[i].imag())) {
      reset();
      throw std::invalid_argument("non-finite O4 detector input");
    }
    if (pending_.empty()) window_start_ = start_sample+i;
    pending_.push_back(samples[i]);
    if (pending_.size() == window_size_) {
      auto result = analyze();
      history_.push_back(result.structure_match);
      if (history_.size() > 5) history_.pop_front();
      result.supporting_windows = static_cast<unsigned>(std::count(history_.begin(), history_.end(), true));
      result.candidate = result.structure_match && history_.size() == 5 && result.supporting_windows >= 3;
      if (result.candidate) result.reason = "repeated_structure_identity_unverified";
      weak_history_.push_back(result.weak_structure_match);
      if (weak_history_.size() > 10) weak_history_.pop_front();
      result.weak_supporting_windows = static_cast<unsigned>(std::count(weak_history_.begin(), weak_history_.end(), true));
      if (!result.candidate && result.weak_structure_match && weak_history_.size() == 10 && result.weak_supporting_windows >= 8) {
        result.candidate = true;
        result.reason = "persistent_weak_structure_identity_unverified";
      }
      results.push_back(std::move(result));
      pending_.clear();
    }
  }
  expected_sample_ = start_sample + count;
  return results;
}
}
