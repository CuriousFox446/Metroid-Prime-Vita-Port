// What each stage of the game's frame sends through GX, and the heap in use: the
// [gxsrc] and [heap] lines (see port_profile.h).

#include "port_profile.h"

#include <aurora/gfx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace PortGxSource {
namespace {
// Index 0 is everything sent outside a named stage. Game thread only, like GX.
const char* sNames[AURORA_GX_SOURCES] = {"(no stage)"};
uint32_t sCount = 1;
} // namespace

uint32_t Register(const char* name) {
  for (uint32_t i = 1; i < sCount; ++i) {
    if (std::strcmp(sNames[i], name) == 0) {
      return i;
    }
  }
  if (sCount == AURORA_GX_SOURCES) {
    return 0;
  }
  sNames[sCount] = name;
  return sCount++;
}

Scope::Scope(uint32_t source) noexcept : m_previous(aurora_gx_set_source(source)) {}

Scope::~Scope() { aurora_gx_set_source(m_previous); }

void Report(uint32_t frames, void (*write)(const char* line)) {
  if (frames == 0 || write == nullptr) {
    return;
  }
  AuroraGxSourceCounts counts[AURORA_GX_SOURCES];
  aurora_take_gx_source_counts(counts);
  std::vector< uint32_t > order;
  for (uint32_t i = 0; i < sCount; ++i) {
    const AuroraGxSourceCounts& c = counts[i];
    if (c.begins != 0 || c.lists != 0 || c.residentLists != 0) {
      order.push_back(i);
    }
  }
  // Most per-primitive work first: immediate primitives, then lists sent as they are.
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return counts[a].begins + counts[a].lists > counts[b].begins + counts[b].lists;
  });
  const double perFrame = 1.0 / frames;
  char line[256];
  for (const uint32_t i : order) {
    const AuroraGxSourceCounts& c = counts[i];
    std::snprintf(line, sizeof(line),
                  "[gxsrc] %7.1f begins (%8.1f verts), %6.1f lists sent (%7.1f KiB), %6.1f resident  %s\n",
                  c.begins * perFrame, c.beginVertices * perFrame, c.lists * perFrame, c.listKiB * perFrame,
                  c.residentLists * perFrame, sNames[i]);
    write(line);
  }
  const struct mallinfo heap = mallinfo();
  std::snprintf(line, sizeof(line), "[heap] %.1f MiB in use of %.1f MiB taken from the system\n",
                static_cast< double >(heap.uordblks) / (1024.0 * 1024.0),
                static_cast< double >(heap.arena) / (1024.0 * 1024.0));
  write(line);
}

} // namespace PortGxSource
