#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "droneid/test/microtest.h"
#include "../src/dji/dsp.hpp"

using droneid::cf32;
using droneid::dji::dsp::ChannelResampler;

namespace {
std::vector<cf32> tone(size_t count, double rate, double frequency) {
  std::vector<cf32> samples(count);
  for (size_t sample = 0; sample < count; ++sample)
    samples[sample] = std::polar(1.0f, static_cast<float>(2 * std::acos(-1.0) * frequency * sample / rate));
  return samples;
}
}

TEST(Resampler, StreamingMatchesContinuousTime) {
  for (double input_rate : {11.52e6, 15.36e6, 50e6, 61.44e6, 92.16e6}) {
    const double shift = input_rate <= 15.36e6 ? -1e6 : -9e6;
    const auto input = tone(25003, input_rate, -shift + 1e6);
    ChannelResampler whole(input_rate, shift);
    const auto expected = whole.process(input, true);
    EXPECT_EQ(expected.size(), static_cast<size_t>(std::ceil(input.size() * 15.36e6 / input_rate)));
    ChannelResampler streamed(input_rate, shift);
    std::vector<cf32> actual;
    for (size_t start = 0; start < input.size(); start += 137) {
      const size_t end = std::min(input.size(), start + 137);
      const std::vector<cf32> block(input.begin() + start, input.begin() + end);
      const auto output = streamed.process(block);
      actual.insert(actual.end(), output.begin(), output.end());
    }
    const auto tail = streamed.process({}, true);
    actual.insert(actual.end(), tail.begin(), tail.end());
    EXPECT_EQ(actual.size(), expected.size());
    if (actual.size() != expected.size()) continue;
    double max_difference = 0, max_tone_error = 0;
    for (size_t sample = 0; sample < actual.size(); ++sample) {
      max_difference = std::max(max_difference, static_cast<double>(std::abs(actual[sample] - expected[sample])));
      if (sample > 100 && sample + 100 < actual.size()) {
        const auto ideal = std::polar(1.0, 2 * std::acos(-1.0) * 1e6 * sample / 15.36e6);
        max_tone_error = std::max(max_tone_error, std::abs(std::complex<double>(actual[sample]) - ideal));
      }
    }
    EXPECT_TRUE(max_difference < 1e-6);
    EXPECT_TRUE(max_tone_error < 0.005);
  }
}

TEST(Resampler, RejectsAliasedEnergy) {
  const auto input = tone(30000, 50e6, 12e6);
  ChannelResampler converter(50e6, 0);
  const auto output = converter.process(input, true);
  double power = 0;
  for (size_t sample = 100; sample + 100 < output.size(); ++sample) power += std::norm(output[sample]);
  power /= output.size() - 200;
  EXPECT_TRUE(power < 1e-6);
}

TEST(Resampler, PreservesOccupiedBand) {
  for (double frequency : {-4.5e6, 4.5e6}) {
    ChannelResampler converter(50e6, 0);
    const auto output = converter.process(tone(30000, 50e6, frequency), true);
    double power = 0;
    for (size_t sample = 100; sample + 100 < output.size(); ++sample) power += std::norm(output[sample]);
    power /= output.size() - 200;
    EXPECT_NEAR(power, 1, 0.01);
  }
}

TEST(Resampler, EmptyAndInvalidInput) {
  ChannelResampler converter(50e6, 0);
  EXPECT_TRUE(converter.process({}, true).empty());
  for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()}) {
    bool rejected = false;
    try { ChannelResampler invalid(rate, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    EXPECT_TRUE(rejected);
  }
}

MICROTEST_MAIN
