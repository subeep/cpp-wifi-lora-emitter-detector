// droneid/test/microtest.h — minimal header-only test harness.
// Mirrors a subset of GoogleTest's API (TEST, EXPECT_*) so tests migrate to real
// GoogleTest later with only an include/link change. Zero external dependencies.
// GPL-3.0-or-later.
#pragma once

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace microtest {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

inline int run_all() {
  int failed_cases = 0;
  for (auto& c : cases()) {
    const int before = failures();
    std::printf("[ RUN    ] %s\n", c.name.c_str());
    try {
      c.fn();
    } catch (const std::exception& e) {
      failures()++;
      std::printf("          exception: %s\n", e.what());
    }
    if (failures() > before) {
      std::printf("[   FAIL ] %s\n", c.name.c_str());
      ++failed_cases;
    } else {
      std::printf("[     OK ] %s\n", c.name.c_str());
    }
  }
  std::printf("----\n%zu case(s), %d failing\n", cases().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace microtest

#define TEST(suite, name)                                                      \
  static void suite##_##name();                                                \
  static ::microtest::Registrar microtest_reg_##suite##_##name(               \
      #suite "." #name, suite##_##name);                                       \
  static void suite##_##name()

#define EXPECT_TRUE(x)                                                         \
  do {                                                                         \
    if (!(x)) {                                                                \
      ::microtest::failures()++;                                               \
      std::printf("          EXPECT_TRUE failed: %s (%s:%d)\n", #x, __FILE__,  \
                  __LINE__);                                                   \
    }                                                                          \
  } while (0)

#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))

#define EXPECT_EQ(a, b)                                                        \
  do {                                                                         \
    auto microtest_a = (a);                                                    \
    auto microtest_b = (b);                                                    \
    if (!(microtest_a == microtest_b)) {                                       \
      ::microtest::failures()++;                                               \
      std::printf("          EXPECT_EQ failed: %s == %s (%s:%d)\n", #a, #b,    \
                  __FILE__, __LINE__);                                         \
    }                                                                          \
  } while (0)

#define EXPECT_NEAR(a, b, tol)                                                 \
  do {                                                                         \
    const double microtest_a = (a);                                           \
    const double microtest_b = (b);                                           \
    if (std::fabs(microtest_a - microtest_b) > (tol)) {                        \
      ::microtest::failures()++;                                               \
      std::printf("          EXPECT_NEAR failed: |%s - %s| > %g (%s:%d)\n",    \
                  #a, #b, (double)(tol), __FILE__, __LINE__);                  \
    }                                                                          \
  } while (0)

#define MICROTEST_MAIN                                                         \
  int main() { return ::microtest::run_all(); }
