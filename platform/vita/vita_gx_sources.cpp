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

// The extension commands' names (GXAurora.h) for the ones a frame sends most.
const char* OpName(uint32_t op) {
  switch (op) {
  case 0x01: return "VIEWPORT";
  case 0x02: return "SCISSOR";
  case 0x03: return "PROJECTION";
  case 0x23: return "DRAW_SYNC";
  case 0x30: return "TEXOBJ";
  case 0x31: return "TLUT";
  case 0x35: return "COPY_SRC";
  case 0x36: return "COPY_DST";
  case 0x37: return "COPY_DEST";
  case 0x40: return "DRAW_SIZED";
  case 0x41: return "DRAW_INDEXED";
  case 0x42: return "ARRAY_BASE_INDEX";
  case 0x43: return "PBR";
  case 0x45: return "PBR_PROBE";
  case 0x46: return "PBR_MATERIAL";
  case 0x49: return "PBR_CUBE";
  case 0x4A: return "PBR_AMBIENT";
  case 0x4D: return "PBR_VOLUME";
  case 0x4E: return "PBR_TONE";
  case 0x4F: return "SDF";
  case 0x50: return "DRAW_TAG";
  case 0x51: return "PBR_LIGHT_SKIP";
  case 0x53: return "PBR_LIGHT_SCALE";
  case 0x58: return "PBR_BACKLIGHT";
  case 0x59: return "VOLFOG";
  case 0x5B: return "DRAW_SERIAL";
  case 0x5E: return "SHADOW_CASTER";
  case 0x5F: return "SHADOW_FRAME";
  case 0x60: return "RESIDENT_RETAIN";
  case 0x61: return "RESIDENT_RELEASE";
  case 0x62: return "RESIDENT_CALL";
  case 0x66: return "PBR_LIGHTMAP";
  case 0x67: return "PBR_LIGHTMAP_ATTR";
  case 0x68: return "HUD_SAMPLE";
  case 0x69: return "ROOM_LIGHTS";
  case 0x6A: return "PARTICLE_FOG";
  case 0x6D: return "PBR_MODEL_MOD";
  case 0x70: return "ACTOR_SHADOW";
  default:
    return op >= 0x10 && op <= 0x1F ? "ARRAYBASE" : nullptr;
  }
}

uint32_t sLastOps[AURORA_GX_OPCODES] = {};

// [ops]: extension commands per frame, most first, and their total.
void ReportAuroraOps(double perFrame, void (*write)(const char* line)) {
  uint32_t now[AURORA_GX_OPCODES];
  aurora_read_aurora_op_counts(now);
  uint32_t delta[AURORA_GX_OPCODES];
  uint64_t total = 0;
  std::vector< uint32_t > order;
  for (uint32_t i = 0; i < AURORA_GX_OPCODES; ++i) {
    delta[i] = now[i] - sLastOps[i];
    sLastOps[i] = now[i];
    total += delta[i];
    if (delta[i] != 0) {
      order.push_back(i);
    }
  }
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return delta[a] > delta[b]; });
  char line[512];
  int used = std::snprintf(line, sizeof(line), "[ops] %.0f a frame:", static_cast< double >(total) * perFrame);
  for (size_t k = 0; k < order.size() && k < 12 && used > 0 && used < static_cast< int >(sizeof(line)) - 48; ++k) {
    const uint32_t op = order[k];
    const char* name = OpName(op);
    used += name != nullptr
                ? std::snprintf(line + used, sizeof(line) - used, " %s %.0f", name, delta[op] * perFrame)
                : std::snprintf(line + used, sizeof(line) - used, " 0x%02X %.0f", op, delta[op] * perFrame);
  }
  std::snprintf(line + std::min< int >(used, sizeof(line) - 2), 2, "\n");
  write(line);
}

// [drain]: where the game waited for the FIFO thread. The offsets are from
// aurora_take_drain_sites; tools/vita_drain_sites.sh names them from the build's ELF.
void ReportDrains(double perFrame, void (*write)(const char* line)) {
  AuroraDrainSite sites[AURORA_DRAIN_SITES];
  const uint32_t count = aurora_take_drain_sites(sites);
  const auto anchor = reinterpret_cast< intptr_t >(aurora_drain_anchor());
  char line[256];
  for (uint32_t i = 0; i < count; ++i) {
    const AuroraDrainSite& site = sites[i];
    const intptr_t caller = reinterpret_cast< intptr_t >(site.caller) - anchor;
    const intptr_t origin = site.origin != nullptr ? reinterpret_cast< intptr_t >(site.origin) - anchor : 0;
    std::snprintf(line, sizeof(line), "[drain] %5.2f a frame, %6.2f ms a frame  caller %+lld origin %+lld\n",
                  site.calls * perFrame, static_cast< double >(site.waitNs) * perFrame / 1e6,
                  static_cast< long long >(caller), static_cast< long long >(origin));
    write(line);
  }
}
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
  const AuroraStats* stats = aurora_get_stats();
  std::snprintf(line, sizeof(line),
                "[heap] %.1f MiB in use of %.1f MiB taken from the system | last frame's staging: vertices %u KiB, "
                "uniforms %u KiB, indices %u KiB, arrays %u KiB, textures %u KiB; %u draws (%u merged)\n",
                static_cast< double >(heap.uordblks) / (1024.0 * 1024.0),
                static_cast< double >(heap.arena) / (1024.0 * 1024.0), stats->lastVertSize >> 10,
                stats->lastUniformSize >> 10, stats->lastIndexSize >> 10, stats->lastStorageSize >> 10,
                stats->lastTextureUploadSize >> 10, stats->drawCallCount, stats->mergedDrawCallCount);
  write(line);
  ReportAuroraOps(perFrame, write);
  ReportDrains(perFrame, write);
}

} // namespace PortGxSource
