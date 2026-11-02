// src/dji/params.cpp — carrier mapping and ZC sequence generation.
// Ported from get_data_carrier_indices.m and create_zc.m. GPL-3.0-or-later.
#include <cmath>
#include <complex>

#include "droneid/dji.h"
#include "dsp.hpp"

namespace droneid::dji {

std::vector<int> data_carrier_indices(double fs) {
  const int N = fft_size(fs);
  const int dc = N / 2;  // 0-based DC (MATLAB dc = N/2+1, 1-based)
  std::vector<int> idx;
  idx.reserve(kDataCarriers);
  for (int i = dc - kDataCarriers / 2; i <= dc - 1; ++i) idx.push_back(i);
  for (int i = dc + 1; i <= dc + kDataCarriers / 2; ++i) idx.push_back(i);
  return idx;
}

std::vector<cf32> create_zc(int N, int symbol_index) {
  const int root = (symbol_index == 4) ? kZcRootSym4 : kZcRootSym6;

  // zc[k] = exp(-j*pi*root*k*(k+1)/601), k=0..600 ; then drop the middle (k=300).
  std::vector<cf32> zc;
  zc.reserve(600);
  for (int k = 0; k <= 600; ++k) {
    if (k == 300) continue;  // DC carrier, removed
    const double ph = -M_PI * root * static_cast<double>(k) * (k + 1) / 601.0;
    zc.emplace_back(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
  }

  // Map onto the data carriers of an fftshift'd spectrum, then ifft(fftshift(.)).
  std::vector<cf32> freq(N, cf32(0, 0));
  const auto carriers = data_carrier_indices(static_cast<double>(N) * 15e3);
  for (size_t i = 0; i < carriers.size(); ++i) freq[carriers[i]] = zc[i];

  return dsp::ifft(dsp::fftshift(freq));
}

}  // namespace droneid::dji
