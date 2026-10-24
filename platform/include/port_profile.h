#pragma once

// PORT_ZONE("name") times the rest of the enclosing scope in the PS Vita build's zone
// profiler (extern/aurora/platforms/vita/profile), reported with MP_PROFILE beside the
// [split] timing lines. Elsewhere it is nothing.

#if defined(__vita__)
#include <vita_zone_profile.hpp>
#define PORT_ZONE(name) VITA_ZONE(name)
#define PORT_PROFILE_SET_MAIN_THREAD() ::vita_profile::SetMainThread()
#define PORT_PROFILE_REPORT(frames, write) ::vita_profile::Report(frames, write)
#else
#define PORT_ZONE(name)
#define PORT_PROFILE_SET_MAIN_THREAD()
#define PORT_PROFILE_REPORT(frames, write)
#endif
