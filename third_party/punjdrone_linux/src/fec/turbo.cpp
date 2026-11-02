// src/fec/turbo.cpp — LTE turbo + rate-match + CRC-24 removal for DJI DroneID.
// Ported from the GPL reference cpp/remove_turbo.cc (proto17/dji_droneid).
// Uses vendored turbofec (GPLv3) and CRCpp (BSD). GPL-3.0-or-later.
#include <cstdint>
#include <vector>

#include "droneid/dji.h"

extern "C" {
#include <turbofec/rate_match.h>
#include <turbofec/turbo.h>
}

#define CRCPP_INCLUDE_ESOTERIC_CRC_DEFINITIONS
#include <CRC.h>

namespace droneid::dji {

bool turbo_decode_frame(const std::vector<uint8_t>& bits7200, std::vector<uint8_t>& out_176) {
  const int E = 7200;
  const int D = 1412;                // turbo decoder input length
  const int payload_bytes = 176;
  const int payload_bits = payload_bytes * 8;  // 1408
  if (static_cast<int>(bits7200.size()) != E) return false;

  // Map hard bits to soft values (magic -63/+63 from the reference).
  static const int8_t lut[2] = {-63, 63};
  std::vector<int8_t> soft(E);
  for (int i = 0; i < E; ++i) {
    const uint8_t b = bits7200[i];
    if (b > 1) return false;
    soft[i] = lut[b];
  }

  std::vector<int8_t> d1(D), d2(D), d3(D);
  out_176.assign(payload_bytes, 0);

  struct lte_rate_matcher* rm = lte_rate_matcher_alloc();
  struct tdecoder* dec = alloc_tdec();

  struct lte_rate_matcher_io io;
  io.D = D;
  io.E = E;
  io.d[0] = d1.data();
  io.d[1] = d2.data();
  io.d[2] = d3.data();
  io.e = soft.data();

  lte_rate_match_rv(rm, &io, 0);
  const int status = lte_turbo_decode(dec, payload_bits, 4, out_176.data(),
                                      d1.data(), d2.data(), d3.data());

  free_tdec(dec);
  lte_rate_matcher_free(rm);

  if (status != 0) return false;

  // CRC-24-LTEA over the 176 bytes (includes the trailing CRC) must zero out.
  const uint32_t crc = CRC::Calculate(out_176.data(), out_176.size(), CRC::CRC_24_LTEA());
  return crc == 0;
}

std::vector<uint8_t> build_frame176(const std::vector<uint8_t>& user) {
  std::vector<uint8_t> f(173, 0);
  for (size_t i = 0; i < user.size() && i < 173; ++i) f[i] = user[i];
  const uint32_t crc = CRC::Calculate(f.data(), f.size(), CRC::CRC_24_LTEA());
  f.push_back((crc >> 16) & 0xff);
  f.push_back((crc >> 8) & 0xff);
  f.push_back(crc & 0xff);
  return f;  // 176 bytes
}

bool turbo_encode_frame(const std::vector<uint8_t>& frame176, std::vector<uint8_t>& out_bits) {
  if (frame176.size() != 176) return false;
  const int bits_in = 176 * 8;       // 1408
  const int D = bits_in + 4;         // 1412 (turbo tail)
  const int E = 7200;

  struct lte_turbo_code tc;
  tc.n = 2; tc.k = 4; tc.len = bits_in; tc.rgen = 11; tc.gen = 13;

  std::vector<uint8_t> unpacked;
  unpacked.reserve(bits_in);
  for (uint8_t b : frame176)
    for (int i = 0; i < 8; ++i) unpacked.push_back((b >> (7 - i)) & 1);

  std::vector<int8_t> d1(D, 0), d2(D, 0), d3(D, 0), e(E, 0);
  lte_turbo_encode(&tc, unpacked.data(),
                   reinterpret_cast<uint8_t*>(d1.data()),
                   reinterpret_cast<uint8_t*>(d2.data()),
                   reinterpret_cast<uint8_t*>(d3.data()));

  struct lte_rate_matcher_io io;
  io.D = D; io.E = E;
  io.d[0] = d1.data(); io.d[1] = d2.data(); io.d[2] = d3.data();
  io.e = e.data();

  struct lte_rate_matcher* rm = lte_rate_matcher_alloc();
  lte_rate_match_fw(rm, &io, 0);
  lte_rate_matcher_free(rm);

  out_bits.assign(E, 0);
  for (int i = 0; i < E; ++i) out_bits[i] = e[i] ? 1 : 0;
  return true;
}

}  // namespace droneid::dji
