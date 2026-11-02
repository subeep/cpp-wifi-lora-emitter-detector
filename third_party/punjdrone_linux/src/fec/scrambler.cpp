// src/fec/scrambler.cpp — LTE Gold scrambler (3GPP 36.211 7.2).
// Ported from generate_scrambler_seq.m. GPL-3.0-or-later.
#include "droneid/dji.h"

namespace droneid::dji {

std::vector<uint8_t> generate_scrambler_seq(int num_bits, const std::array<uint8_t, 31>& x2_init) {
  const int Nc = 1600;
  const int total = num_bits + Nc;
  std::vector<uint8_t> x1(total + 31, 0), x2(total + 31, 0);

  x1[0] = 1;  // x1 fixed init: [1 0 0 ... 0]
  for (int i = 0; i < 31; ++i) x2[i] = x2_init[i];

  for (int n = 0; n < total; ++n) {
    x1[n + 31] = (x1[n + 3] + x1[n]) & 1;
    x2[n + 31] = (x2[n + 3] + x2[n + 2] + x2[n + 1] + x2[n]) & 1;
  }

  std::vector<uint8_t> c(num_bits);
  for (int n = 0; n < num_bits; ++n) c[n] = (x1[n + Nc] + x2[n + Nc]) & 1;
  return c;
}

const std::array<uint8_t, 31>& scrambler_x2_init() {
  // process_file.m: fliplr([0 0 1, 0 0 1 0, 0 0 1 1, 0 1 0 0, 0 1 0 1, 0 1 1 0, 0 1 1 1, 1 0 0 0])
  // i.e. bit-reversed 0x12345678 as a 31-bit sequence.
  static const std::array<uint8_t, 31> forward = {
      0,0,1, 0,0,1,0, 0,0,1,1, 0,1,0,0, 0,1,0,1, 0,1,1,0, 0,1,1,1, 1,0,0,0};
  static std::array<uint8_t, 31> reversed = [] {
    std::array<uint8_t, 31> r{};
    for (int i = 0; i < 31; ++i) r[i] = forward[30 - i];
    return r;
  }();
  return reversed;
}

}  // namespace droneid::dji
