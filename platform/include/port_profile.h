#pragma once

// PORT_ZONE("name") marks a stage of the game's frame for the PS Vita build:
//  - the zone profiler (extern/aurora/platforms/vita/profile) times the rest of the
//    enclosing scope, reported with MP_PROFILE beside the [split] timing lines;
//  - while it runs, what the game sends through GX (immediate primitives, display
//    lists) is counted under its name (aurora_gx_set_source), reported as [gxsrc]
//    lines with MP_TRACE_TIMING. The innermost stage takes the count.
// PORT_ZONE_TIMED("name") only times: for helpers every stage calls (a model's surface
// draw, skinning), which would otherwise take every stage's count.
// Elsewhere both are nothing.

#if defined(__vita__)
#include <vita_zone_profile.hpp>

#include <cstdint>

namespace PortGxSource {
// The source index for a stage's name, 1-31, once per call site; 0 once all are taken.
uint32_t Register(const char* name);

class Scope {
public:
  explicit Scope(uint32_t source) noexcept;
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  uint32_t m_previous;
};

// Writes, through `write`, what each source sent per frame over `frames` frames, and the
// heap in use. Resets the counts.
void Report(uint32_t frames, void (*write)(const char* line));
} // namespace PortGxSource

#define PORT_ZONE(name)                                                                                                \
  VITA_ZONE(name);                                                                                                     \
  static const uint32_t VITA_ZONE_CAT(portGxSource_, __LINE__) = ::PortGxSource::Register(name);                       \
  ::PortGxSource::Scope VITA_ZONE_CAT(portGxScope_, __LINE__) { VITA_ZONE_CAT(portGxSource_, __LINE__) }
#define PORT_ZONE_TIMED(name) VITA_ZONE(name)
#define PORT_PROFILE_SET_MAIN_THREAD() ::vita_profile::SetMainThread()
#define PORT_PROFILE_REPORT(frames, write)                                                                             \
  do {                                                                                                                 \
    ::vita_profile::Report(frames, write);                                                                             \
    ::PortGxSource::Report(frames, write);                                                                             \
  } while (0)
#else
#define PORT_ZONE(name)
#define PORT_ZONE_TIMED(name)
#define PORT_PROFILE_SET_MAIN_THREAD()
#define PORT_PROFILE_REPORT(frames, write)
#endif
