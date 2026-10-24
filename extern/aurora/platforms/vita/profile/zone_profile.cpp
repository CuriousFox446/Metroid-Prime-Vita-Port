#include "vita_zone_profile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#else
#include <chrono>
#include <thread>
#endif

namespace vita_profile {
namespace {
std::atomic<Site*> g_sites{nullptr};
#if defined(__vita__)
pthread_t g_mainThread{};
#else
std::thread::id g_mainThread{};
#endif
std::atomic<bool> g_mainSet{false};

bool ReadEnabled() {
  const char* value = std::getenv("MP_PROFILE");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}
} // namespace

uint64_t NowNs() noexcept {
#if defined(__vita__)
  // Microseconds; the difference of two floored readings is unbiased on average.
  return sceKernelGetProcessTimeWide() * 1000ull;
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

bool Enabled() noexcept {
  static const bool enabled = ReadEnabled();
  return enabled;
}

bool IsMainThread() noexcept {
  if (!g_mainSet.load(std::memory_order_acquire)) {
    return false;
  }
#if defined(__vita__)
  return pthread_equal(pthread_self(), g_mainThread) != 0;
#else
  return std::this_thread::get_id() == g_mainThread;
#endif
}

void SetMainThread() noexcept {
#if defined(__vita__)
  g_mainThread = pthread_self();
#else
  g_mainThread = std::this_thread::get_id();
#endif
  g_mainSet.store(true, std::memory_order_release);
}

void Link(Site& site) noexcept {
  bool expected = false;
  if (!site.linked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return; // another thread linked it first
  }
  Site* head = g_sites.load(std::memory_order_relaxed);
  do {
    site.next = head;
  } while (!g_sites.compare_exchange_weak(head, &site, std::memory_order_release, std::memory_order_relaxed));
}

void Report(uint32_t frames, void (*write)(const char* line)) {
  if (!Enabled() || frames == 0 || write == nullptr) {
    return;
  }
  if (!g_mainSet.load(std::memory_order_acquire)) {
    SetMainThread();
  }
  struct Row {
    const char* name;
    uint64_t ns;
    uint32_t calls;
  };
  std::vector<Row> main, other;
  for (Site* site = g_sites.load(std::memory_order_acquire); site != nullptr; site = site->next) {
    const uint64_t mainNs = site->mainNs.exchange(0, std::memory_order_relaxed);
    const uint32_t mainCalls = site->mainCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t otherNs = site->otherNs.exchange(0, std::memory_order_relaxed);
    const uint32_t otherCalls = site->otherCalls.exchange(0, std::memory_order_relaxed);
    if (mainCalls != 0) {
      main.push_back({site->name, mainNs, mainCalls});
    }
    if (otherCalls != 0) {
      other.push_back({site->name, otherNs, otherCalls});
    }
  }
  const auto byTime = [](const Row& a, const Row& b) { return a.ns > b.ns; };
  std::sort(main.begin(), main.end(), byTime);
  std::sort(other.begin(), other.end(), byTime);
  const double perFrame = 1.0 / static_cast<double>(frames);
  char line[512];
  constexpr size_t kMainRows = 14;
  for (size_t i = 0; i < main.size() && i < kMainRows; ++i) {
    std::snprintf(line, sizeof(line), "[zones]   %6.2f ms %7.1f calls  %s\n",
                  static_cast<double>(main[i].ns) / 1e6 * perFrame, main[i].calls * perFrame, main[i].name);
    write(line);
  }
  size_t used = static_cast<size_t>(std::snprintf(line, sizeof(line), "[zones] other threads, ms/frame:"));
  for (size_t i = 0; i < other.size() && i < 5 && used < sizeof(line); ++i) {
    used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %s %.2f", other[i].name,
                                              static_cast<double>(other[i].ns) / 1e6 * perFrame));
  }
  if (used < sizeof(line) - 1) {
    line[used++] = '\n';
    line[used] = '\0';
  } else {
    line[sizeof(line) - 2] = '\n';
  }
  write(line);
}

} // namespace vita_profile
