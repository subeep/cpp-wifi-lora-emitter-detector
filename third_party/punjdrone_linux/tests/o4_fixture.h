#pragma once
#include "droneid/types.h"
#include <cmath>
#include <random>
#include <vector>

inline std::vector<droneid::cf32> structured_reference(double rate, double useful_hz=30000,
                                                     double symbol_hz=28000) {
  const auto size = static_cast<std::size_t>(std::llround(rate * .16));
  const auto useful = static_cast<std::size_t>(std::llround(rate/useful_hz));
  std::vector<droneid::cf32> x;
  std::mt19937 gen(379);
  std::normal_distribution<float> normal(0,.03);
  for (std::size_t k=0; x.size()<size; ++k) {
    const auto total = static_cast<std::size_t>(std::llround((k+1)*rate/symbol_hz)) -
                       static_cast<std::size_t>(std::llround(k*rate/symbol_hz));
    const auto prefix = total-useful;
    std::vector<droneid::cf32> symbol(useful);
    for (auto& v : symbol) v={normal(gen),normal(gen)};
    x.insert(x.end(),symbol.end()-prefix,symbol.end());
    x.insert(x.end(),symbol.begin(),symbol.end());
  }
  x.resize(size);
  for (std::size_t n=0;n<x.size();++n)
    x[n] *= std::polar(1.f, static_cast<float>(2*std::acos(-1.)*1.2e6*n/rate));
  return x;
}
