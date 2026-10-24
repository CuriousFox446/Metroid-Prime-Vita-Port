// Host check of the Vita zone profiler (../profile). From this directory:
//   g++ -std=c++20 -I../profile profile_test.cpp ../profile/zone_profile.cpp -o profile_test -pthread
//   MP_PROFILE=1 ./profile_test

#include <vita_zone_profile.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::vector<std::string> g_lines;
static int g_failures = 0;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("FAIL %d: %s\n", __LINE__, #cond);                                                                   \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

static void Spin(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
  }
}

static void Inner() {
  VITA_ZONE("inner");
  Spin(2);
}

static void Outer() {
  VITA_ZONE("outer");
  Spin(1);
  Inner();
  Inner();
}

static const std::string* Find(const char* name) {
  for (const auto& line : g_lines) {
    if (line.find(std::string("  ") + name + "\n") != std::string::npos) {
      return &line;
    }
  }
  return nullptr;
}

int main() {
  if (!vita_profile::Enabled()) {
    std::printf("profile_test: set MP_PROFILE=1\n");
    return 1;
  }
  vita_profile::SetMainThread();
  for (int frame = 0; frame < 2; ++frame) {
    Outer();
  }
  std::thread worker([] {
    VITA_ZONE("worker zone");
    Spin(3);
  });
  worker.join();

  vita_profile::Report(2, [](const char* line) { g_lines.emplace_back(line); });
  for (const auto& line : g_lines) {
    std::fputs(line.c_str(), stdout);
  }
  const std::string* outer = Find("outer");
  const std::string* inner = Find("inner");
  CHECK(outer != nullptr && inner != nullptr);
  if (outer != nullptr && inner != nullptr) {
    double outerMs = 0, innerMs = 0, outerCalls = 0, innerCalls = 0;
    std::sscanf(outer->c_str(), "[zones] %lf ms %lf calls", &outerMs, &outerCalls);
    std::sscanf(inner->c_str(), "[zones] %lf ms %lf calls", &innerMs, &innerCalls);
    // Per frame: outer once (1 + 2*2 ms, inclusive), inner twice (2*2 ms).
    CHECK(outerCalls == 1.0 && innerCalls == 2.0);
    CHECK(outerMs >= 5.0 && outerMs < 6.5);
    CHECK(innerMs >= 4.0 && innerMs < 5.5);
    CHECK(outer < inner); // sorted by time, longest first
  }
  CHECK(Find("worker zone") == nullptr); // not the main thread's
  CHECK(!g_lines.empty() && g_lines.back().find("worker zone") != std::string::npos);

  // Counts reset after a report.
  g_lines.clear();
  vita_profile::Report(1, [](const char* line) { g_lines.emplace_back(line); });
  CHECK(Find("outer") == nullptr);

  std::printf(g_failures == 0 ? "profile_test: all passed\n" : "profile_test: %d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
