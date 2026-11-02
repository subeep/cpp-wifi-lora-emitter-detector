#include "droneid/o4_detector.h"
#include "droneid/test/microtest.h"
#include "o4_fixture.h"
#include <algorithm>
#include <cmath>
#include <random>

using namespace droneid;

TEST(O4Evidence, StructureRatesAndChunkContinuity) {
  for (double rate : {11.52e6,15.36e6,20e6}) {
    auto x=structured_reference(rate);
    O4CandidateDetector whole(rate), streamed(rate);
    const auto expected=whole.feed(x.data(),x.size(),0);
    EXPECT_EQ(expected.size(),std::size_t(10));
    EXPECT_TRUE(expected.back().candidate);
    EXPECT_FALSE(expected[0].candidate);
    std::vector<O4Evidence> actual;
    for (std::size_t i=0;i<x.size();i+=13711) {
      auto events=streamed.feed(x.data()+i,std::min(std::size_t(13711),x.size()-i),i);
      actual.insert(actual.end(),events.begin(),events.end());
    }
    EXPECT_EQ(actual.size(),expected.size());
    for (std::size_t i=0;i<actual.size();++i) {
      EXPECT_EQ(actual[i].candidate,expected[i].candidate);
      EXPECT_EQ(actual[i].start_sample,expected[i].start_sample);
      EXPECT_NEAR(actual[i].cyclic_correlation,expected[i].cyclic_correlation,1e-10);
    }
    auto after_gap=streamed.feed(x.data(),streamed.window_samples(),x.size()+100);
    EXPECT_EQ(after_gap.size(),std::size_t(1));
    EXPECT_FALSE(after_gap[0].candidate);
    EXPECT_EQ(after_gap[0].supporting_windows,1u);
  }
}

TEST(O4Evidence, RejectsNoiseToneWifiAndOtherNumerology) {
  const double rate=20e6;
  auto noise=structured_reference(rate);
  std::mt19937 gen(831);
  std::normal_distribution<float> normal(0,.05);
  for (auto& x:noise) x={normal(gen),normal(gen)};
  auto tone=noise;
  for (std::size_t n=0;n<tone.size();++n) {
    const float am=.1f*(1+.8f*std::cos(2*std::acos(-1.)*28000*n/rate));
    tone[n]=std::polar(am,static_cast<float>(2*std::acos(-1.)*.1*n));
  }
  for (const auto& x : {noise,tone,structured_reference(rate,312500,250000),structured_reference(rate,15000,14000)}) {
    O4CandidateDetector detector(rate);
    for (const auto& event:detector.feed(x.data(),x.size(),0)) EXPECT_FALSE(event.candidate);
  }
}

TEST(O4Evidence, ClippingInvalidSamplesAndShortInput) {
  O4CandidateDetector detector(15.36e6);
  auto x=structured_reference(15.36e6);
  for (auto& v:x) v*=1000.f;
  for (const auto& e:detector.feed(x.data(),x.size(),0)) {
    EXPECT_FALSE(e.candidate);
    EXPECT_EQ(e.reason,std::string("clipping"));
  }
  detector.reset();
  EXPECT_TRUE(detector.feed(x.data(),100,0).empty());
  x[0]={NAN,0};
  bool rejected=false;
  try { detector.feed(x.data(),1,100); } catch(const std::exception&) { rejected=true; }
  EXPECT_TRUE(rejected);
  rejected=false;
  try { O4CandidateDetector invalid(0); } catch(const std::exception&) { rejected=true; }
  EXPECT_TRUE(rejected);
}

TEST(O4Evidence, WeakStructureRequiresLongPersistenceAndResetsOnGap) {
  const double rate = 15.36e6;
  auto x = structured_reference(rate);
  std::mt19937 gen(129);
  std::normal_distribution<float> normal(0, .04);
  for (auto& sample : x) sample += cf32(normal(gen), normal(gen));
  O4CandidateDetector detector(rate);
  const auto events = detector.feed(x.data(), x.size(), 0);
  EXPECT_EQ(events.size(), std::size_t(10));
  for (std::size_t i = 0; i < 9; ++i) EXPECT_FALSE(events[i].candidate);
  EXPECT_FALSE(events.back().structure_match);
  EXPECT_TRUE(events.back().weak_structure_match);
  EXPECT_TRUE(events.back().candidate);
  EXPECT_EQ(events.back().reason, std::string("persistent_weak_structure_identity_unverified"));
  const auto after = detector.feed(x.data(), detector.window_samples(), x.size()+1);
  EXPECT_FALSE(after.front().candidate);
  EXPECT_EQ(after.front().weak_supporting_windows, 1u);
}

MICROTEST_MAIN
