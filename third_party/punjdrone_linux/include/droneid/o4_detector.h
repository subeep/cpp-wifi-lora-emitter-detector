#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "droneid/types.h"

namespace droneid {

inline constexpr const char* kO4Profile = "o4_structure_v2";

struct O4Evidence {
  uint64_t start_sample = 0;
  uint64_t sample_count = 0;
  double cp_correlation = 0;
  double lag_prominence = 0;
  double cyclic_correlation = 0;
  double cyclic_prominence = 0;
  double harmonic_correlation = 0;
  double clipped_fraction = 0;
  unsigned supporting_windows = 0;
  unsigned weak_supporting_windows = 0;
  bool structure_match = false;
  bool weak_structure_match = false;
  bool candidate = false;
  std::string reason = "insufficient_structure";
  std::string to_jsonl() const;
};

class O4CandidateDetector {
 public:
  explicit O4CandidateDetector(double sample_rate);
  std::vector<O4Evidence> feed(const cf32* samples, std::size_t count,
                             uint64_t start_sample);
  void reset();
  std::size_t window_samples() const { return window_size_; }

 private:
  O4Evidence analyze() const;
  double rate_;
  std::size_t window_size_;
  uint64_t expected_sample_ = 0, window_start_ = 0;
  bool positioned_ = false;
  std::vector<cf32> pending_;
  std::deque<bool> history_;
  std::deque<bool> weak_history_;
};

}
