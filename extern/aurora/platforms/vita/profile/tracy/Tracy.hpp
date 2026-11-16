#pragma once

// PS Vita: Tracy's headers, with its CPU zones sent to the Vita zone profiler
// (vita_zone_profile.hpp). Found before Tracy's own include directory, which
// #include_next reaches; the rest of Tracy stays compiled out (TRACY_ENABLE is off).

#include_next <tracy/Tracy.hpp>

#include "../vita_zone_profile.hpp"

#undef ZoneScoped
#undef ZoneScopedN
#undef ZoneScopedS
#define ZoneScoped VITA_ZONE(__func__)
#define ZoneScopedN(name) VITA_ZONE(name)
#define ZoneScopedS(depth) VITA_ZONE(__func__)
