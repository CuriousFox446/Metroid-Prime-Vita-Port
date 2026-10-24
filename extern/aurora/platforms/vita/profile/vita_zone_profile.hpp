#pragma once

// A small zone profiler for the PS Vita build, which has no Tracy: VITA_ZONE(name)
// times the rest of its scope and adds it to a per-call-site record, kept apart for
// the main thread and the others. Report() writes the zones that took the most main
// thread time since its last call, per frame. Aurora's own Tracy zones (ZoneScoped,
// ZoneScopedN) come here through the tracy/Tracy.hpp shim beside this file; the game
// marks its stages with PORT_ZONE (platform/include/port_profile.h).
//
// Times are inclusive (a zone counts the zones inside it), and each zone costs two
// clock reads, so many tiny zones inflate what they measure a little.

#include <atomic>
#include <cstdint>

namespace vita_profile {

struct Site {
  const char* name;
  std::atomic<uint64_t> mainNs{0};
  std::atomic<uint64_t> otherNs{0};
  std::atomic<uint32_t> mainCalls{0};
  std::atomic<uint32_t> otherCalls{0};
  std::atomic<bool> linked{false};
  Site* next = nullptr;

  explicit Site(const char* siteName) noexcept : name(siteName) {}
};

uint64_t NowNs() noexcept;
bool Enabled() noexcept;
bool IsMainThread() noexcept;
// Adds a site to the list Report walks; once per site, on its first use.
void Link(Site& site) noexcept;
// The thread whose time Report lists; the first caller of Report otherwise.
void SetMainThread() noexcept;
// Writes the main thread's top zones since the last call, as milliseconds and calls
// per frame over `frames` frames, through `write` (one call per line), and the busiest
// zones on other threads in one more line. Resets the counts.
void Report(uint32_t frames, void (*write)(const char* line));

class Zone {
public:
  explicit Zone(Site& site) noexcept : m_site(Enabled() ? &site : nullptr) {
    if (m_site != nullptr) {
      if (!site.linked.load(std::memory_order_acquire)) {
        Link(site);
      }
      m_start = NowNs();
    }
  }
  ~Zone() {
    if (m_site == nullptr) {
      return;
    }
    const uint64_t elapsed = NowNs() - m_start;
    if (IsMainThread()) {
      m_site->mainNs.fetch_add(elapsed, std::memory_order_relaxed);
      m_site->mainCalls.fetch_add(1, std::memory_order_relaxed);
    } else {
      m_site->otherNs.fetch_add(elapsed, std::memory_order_relaxed);
      m_site->otherCalls.fetch_add(1, std::memory_order_relaxed);
    }
  }
  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;

private:
  Site* m_site;
  uint64_t m_start = 0;
};

} // namespace vita_profile

#define VITA_ZONE_CAT2(a, b) a##b
#define VITA_ZONE_CAT(a, b) VITA_ZONE_CAT2(a, b)
#define VITA_ZONE(name)                                                                                                \
  static ::vita_profile::Site VITA_ZONE_CAT(vitaZoneSite_, __LINE__){name};                                            \
  ::vita_profile::Zone VITA_ZONE_CAT(vitaZone_, __LINE__) { VITA_ZONE_CAT(vitaZoneSite_, __LINE__) }
