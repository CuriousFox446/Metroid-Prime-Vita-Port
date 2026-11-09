#pragma once

// PORT_ZONE("name") marks a stage of the game's frame for the PS Vita build:
//  - the zone profiler (extern/aurora/platforms/vita/profile) times the rest of the
//    enclosing scope, reported with MP_PROFILE beside the [split] timing lines;
//  - while it runs, what the game sends through GX (immediate primitives, display
//    lists) is counted under its name (aurora_gx_set_source), reported as [gxsrc]
//    lines with MP_TRACE_TIMING. The innermost stage takes the count.
// PORT_ZONE_TIMED("name") only times: for helpers every stage calls (a model's surface
// draw, skinning), which would otherwise take every stage's count.
// PORT_THINK_SCOPE(entity, kThink or kPreThink) times one entity's Think or PreThink by its
// class (typeid), for the report's [think] lines.
// Elsewhere all are nothing.

#if defined(__vita__)
#include <vita_zone_profile.hpp>

#include <cstdint>
#include <typeinfo>

namespace PortThinkProfile {
enum Pass : uint8_t { kPreThink, kThink, kPassCount };

// Times what its scope runs under the entity class `type`, sampled as the zone profiler's sites
// are (a class's first calls in a report window, then one in eight, weighted). Game thread only.
class Scope {
public:
  Scope(const std::type_info& type, Pass pass) noexcept;
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  struct Type* m_type = nullptr;
  uint64_t m_start = 0;
  uint32_t m_weight = 0;
  Pass m_pass = kThink;
};

// Writes, through `write`, the classes whose Think or PreThink took the most time per frame over
// `frames` frames ([think] lines: ms and calls a frame, the pass, the class). Resets the counts.
void Report(uint32_t frames, void (*write)(const char* line));
} // namespace PortThinkProfile

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
#define PORT_THINK_SCOPE(entity, pass)                                                                                 \
  ::PortThinkProfile::Scope VITA_ZONE_CAT(portThink_, __LINE__) { typeid(entity), ::PortThinkProfile::pass }
#define PORT_PROFILE_SET_MAIN_THREAD() ::vita_profile::SetMainThread()
#define PORT_PROFILE_REPORT(frames, write)                                                                             \
  do {                                                                                                                 \
    ::vita_profile::Report(frames, write);                                                                             \
    ::PortGxSource::Report(frames, write);                                                                             \
    ::PortThinkProfile::Report(frames, write);                                                                         \
  } while (0)
#else
#define PORT_ZONE(name)
#define PORT_ZONE_TIMED(name)
#define PORT_THINK_SCOPE(entity, pass)
#define PORT_PROFILE_SET_MAIN_THREAD()
#define PORT_PROFILE_REPORT(frames, write)
#endif
