#include "vita_draw.hpp"

#include "../internal.hpp"
#include "../vita_gl.hpp"
#include "gx.hpp"

#include <dolphin/gx/GXCommandList.h>

#include <tracy/Tracy.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace aurora::gx::vita {
namespace {
constexpr Module Log{"aurora::gx::vita"};

// The GX state a decode read: the vertex descriptor, the vertex formats the list's primitives
// name, and the position and colour arrays. The same list called with other state is decoded
// again; formats the list does not use change all the time (the HUD, particles) and do not count.
struct DecodeState {
  std::array<GXAttrType, MaxVtxAttr> vtxDesc{};
  u8 fmtMask = 0;
  std::array<VtxFmt, MaxVtxFmt> vtxFmts{}; // only those in fmtMask are compared
  std::array<AttrArray, 2> arrays{};       // GX_VA_POS, GX_VA_CLR0

  bool matches_current() const noexcept {
    if (vtxDesc != g_gxState.vtxDesc) {
      return false;
    }
    for (u32 fmt = 0; fmt < MaxVtxFmt; ++fmt) {
      if ((fmtMask & (1u << fmt)) != 0 &&
          std::memcmp(&vtxFmts[fmt], &g_gxState.vtxFmts[fmt], sizeof(VtxFmt)) != 0) {
        return false;
      }
    }
    // An array counts only when the descriptor indexes it.
    constexpr GXAttr attrs[2] = {GX_VA_POS, GX_VA_CLR0};
    for (int i = 0; i < 2; ++i) {
      if (vtxDesc[attrs[i]] != GX_INDEX8 && vtxDesc[attrs[i]] != GX_INDEX16) {
        continue;
      }
      const AttrArray* current = &g_gxState.arrays[attrs[i]];
      if (arrays[i].data != current->data || arrays[i].stride != current->stride ||
          arrays[i].baseIndex != current->baseIndex) {
        return false;
      }
    }
    return true;
  }
};

// A list's decoded triangles, and the state they were decoded with. A list can be called in a
// few states in turn (one model's lists under two vertex formats), so a few are kept.
struct Cached {
  std::shared_ptr<vita_gl::Mesh> mesh; // null: the list cannot be drawn this way
  DecodeState state;
};
constexpr size_t kVariantsPerList = 4;
struct CachedList {
  std::array<Cached, kVariantsPerList> variants;
  size_t count = 0;
  size_t nextReplace = 0;
  size_t newest = 0;
};
std::unordered_map<const void*, CachedList> sCache;

// For log_stats: what became of the calls since the last report, and why lists were not drawn.
struct Stats {
  u32 calls = 0;
  u32 nativePositions = 0;
  u32 decoded = 0;
  u32 recorded = 0;
  u32 failMatrixIndex = 0;
  u32 failArray = 0;
  u32 failCommand = 0;
  u32 failEmpty = 0;
  // Decodes of a list already cached, by what differed from its newest variant.
  u32 redecodeDesc = 0;
  u32 redecodeFmt = 0;
  u32 redecodeArray = 0;
};
Stats sStats;
enum class Fail { None, MatrixIndex, Array, Command, Empty };
Fail sLastFail = Fail::None;
bool sFirstFailLogged = false;

u16 read16(const u8* p, bool le) noexcept {
  return le ? static_cast<u16>(p[0] | (p[1] << 8)) : static_cast<u16>((p[0] << 8) | p[1]);
}

u32 read32(const u8* p, bool le) noexcept {
  return le ? static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
            : static_cast<u32>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

float read_component(const u8* p, GXCompType type, u8 frac, bool le) noexcept {
  const float scale = 1.f / static_cast<float>(1u << frac);
  switch (type) {
  case GX_U8:
    return p[0] * scale;
  case GX_S8:
    return static_cast<s8>(p[0]) * scale;
  case GX_U16:
    return read16(p, le) * scale;
  case GX_S16:
    return static_cast<s16>(read16(p, le)) * scale;
  case GX_F32:
    return std::bit_cast<float>(read32(p, le));
  default:
    return 0.f;
  }
}

void read_color(const u8* p, GXCompType type, u8 out[4]) noexcept {
  switch (type) {
  case GX_RGB565: {
    const u16 v = read16(p, false);
    out[0] = static_cast<u8>(((v >> 11) & 31) * 255 / 31);
    out[1] = static_cast<u8>(((v >> 5) & 63) * 255 / 63);
    out[2] = static_cast<u8>((v & 31) * 255 / 31);
    out[3] = 255;
    break;
  }
  case GX_RGB8:
  case GX_RGBX8:
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = 255;
    break;
  case GX_RGBA4: {
    const u16 v = read16(p, false);
    out[0] = static_cast<u8>(((v >> 12) & 15) * 17);
    out[1] = static_cast<u8>(((v >> 8) & 15) * 17);
    out[2] = static_cast<u8>(((v >> 4) & 15) * 17);
    out[3] = static_cast<u8>((v & 15) * 17);
    break;
  }
  case GX_RGBA6: {
    const u32 v = (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
    out[0] = static_cast<u8>(((v >> 18) & 63) * 255 / 63);
    out[1] = static_cast<u8>(((v >> 12) & 63) * 255 / 63);
    out[2] = static_cast<u8>(((v >> 6) & 63) * 255 / 63);
    out[3] = static_cast<u8>((v & 63) * 255 / 63);
    break;
  }
  case GX_RGBA8:
    std::memcpy(out, p, 4);
    break;
  default:
    out[0] = out[1] = out[2] = out[3] = 255;
    break;
  }
}

// One GX vertex's position and colour, from its bytes in the list (which advance past it).
// False when it has a per-vertex matrix (not drawn yet) or reads outside an array.
bool decode_vertex(const u8*& vtx, const VtxFmt& fmt, vita_gl::Vertex& out) noexcept {
  out = {};
  out.color[0] = out.color[1] = out.color[2] = 200;
  out.color[3] = 255;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const GXAttrType type = g_gxState.vtxDesc[i];
    if (type == GX_NONE) {
      continue;
    }
    const VtxAttrFmt& attrFmt = fmt.attrs[i];
    if (attr == GX_VA_PNMTXIDX) {
      sLastFail = Fail::MatrixIndex;
      return false;
    }
    const u32 elemSize = attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX
                             ? 1
                             : u32(comp_type_size(attr, attrFmt.type)) * comp_cnt_count(attr, attrFmt.cnt);
    const u8* data;
    bool le = false;
    if (type == GX_DIRECT) {
      data = vtx;
      vtx += elemSize;
    } else {
      const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
      const u32 index = type == GX_INDEX8 ? vtx[0] : read16(vtx, false);
      vtx += (type == GX_INDEX8 ? 1 : 2) * (nbt3 ? 3 : 1);
      if (attr != GX_VA_POS && attr != GX_VA_CLR0) {
        continue;
      }
      const AttrArray& array = g_gxState.arrays[i];
      const u64 offset = u64(index + array.baseIndex) * array.stride;
      if (array.data == nullptr || offset + elemSize > array.size) {
        sLastFail = Fail::Array;
        if (!sFirstFailLogged) {
          sFirstFailLogged = true;
          Log.warn("first list not drawn: attribute {} index {} (+{}) stride {} past its array ({} bytes at {})", i,
                   index, array.baseIndex, array.stride, array.size, array.data);
        }
        return false;
      }
      data = static_cast<const u8*>(array.data) + offset;
      le = array.le;
    }
    if (attr == GX_VA_POS) {
      const u32 cnt = comp_cnt_count(attr, attrFmt.cnt);
      const u32 step = comp_type_size(attr, attrFmt.type);
      for (u32 c = 0; c < cnt; ++c) {
        out.pos[c] = read_component(data + c * step, attrFmt.type, attrFmt.frac, le);
      }
    } else if (attr == GX_VA_CLR0) {
      read_color(data, attrFmt.type, out.color);
    }
  }
  return true;
}

void append_triangle(std::vector<vita_gl::Vertex>& out, const vita_gl::Vertex& a, const vita_gl::Vertex& b,
                     const vita_gl::Vertex& c) {
  // A face normal, in the model's space, shades the face (the fixed-function draw has no light):
  // up-facing brightest (the game's world is Z-up), walls by which way they face, so the shapes
  // read without textures.
  const float u[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
  const float v[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
  float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  float shade = 1.f;
  if (len > 1e-8f) {
    for (float& component : n) {
      component /= len;
    }
    shade = 0.3f + 0.7f * (0.65f * std::fabs(n[2]) + 0.35f * std::fabs(n[0]));
  }
  for (const vita_gl::Vertex* src : {&a, &b, &c}) {
    vita_gl::Vertex vertex = *src;
    std::memcpy(vertex.nrm, n, sizeof(n));
    for (int i = 0; i < 3; ++i) {
      vertex.color[i] = static_cast<u8>(std::min(255.f, vertex.color[i] * shade));
    }
    out.push_back(vertex);
  }
}

// The list's triangles, or none when it holds anything this path cannot draw yet; fmtMask gets
// the vertex formats it read.
std::vector<vita_gl::Vertex> decode_list(const std::vector<u8>& bytes, u32 size, u8& fmtMask) noexcept {
  std::vector<vita_gl::Vertex> triangles;
  std::vector<vita_gl::Vertex> prim;
  size_t pos = 0;
  const size_t end = std::min<size_t>(size, bytes.size());
  while (pos < end) {
    const u8 cmd = bytes[pos++];
    if (cmd == GX_NOP) {
      continue;
    }
    if (cmd < 0x80 || end - pos < 2) {
      sLastFail = Fail::Command;
      return {};
    }
    const auto fmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK);
    fmtMask |= static_cast<u8>(1u << fmt);
    const auto type = static_cast<GXPrimitive>(cmd & GX_OPCODE_MASK);
    const u16 count = static_cast<u16>((bytes[pos] << 8) | bytes[pos + 1]);
    pos += 2;
    prim.resize(count);
    const u8* vtx = bytes.data() + pos;
    for (u16 i = 0; i < count; ++i) {
      if (vtx >= bytes.data() + end || !decode_vertex(vtx, g_gxState.vtxFmts[fmt], prim[i])) {
        return {};
      }
    }
    pos = static_cast<size_t>(vtx - bytes.data());
    if (pos > end) {
      return {};
    }
    switch (type) {
    case GX_TRIANGLES:
      for (u32 i = 0; i + 2 < count; i += 3) {
        append_triangle(triangles, prim[i], prim[i + 1], prim[i + 2]);
      }
      break;
    case GX_TRIANGLESTRIP:
      for (u32 i = 0; i + 2 < count; ++i) {
        if ((i & 1) == 0) {
          append_triangle(triangles, prim[i], prim[i + 1], prim[i + 2]);
        } else {
          append_triangle(triangles, prim[i + 1], prim[i], prim[i + 2]);
        }
      }
      break;
    case GX_TRIANGLEFAN:
      for (u32 i = 1; i + 1 < count; ++i) {
        append_triangle(triangles, prim[0], prim[i], prim[i + 1]);
      }
      break;
    case GX_QUADS:
      for (u32 i = 0; i + 3 < count; i += 4) {
        append_triangle(triangles, prim[i], prim[i + 1], prim[i + 2]);
        append_triangle(triangles, prim[i], prim[i + 2], prim[i + 3]);
      }
      break;
    default:
      break; // lines and points: not drawn yet
    }
  }
  return triangles;
}

// The list's entry for the current state, decoding it into a new or the oldest variant when
// none matches.
const Cached& lookup(const void* key, const resident::Entry& entry, u32 size) noexcept {
  CachedList& list = sCache[key];
  for (size_t i = 0; i < list.count; ++i) {
    if (list.variants[i].state.matches_current()) {
      return list.variants[i];
    }
  }
  ZoneScopedN("vita: decode a display list");
  if (list.count != 0) {
    DecodeState formatsOnly = list.variants[list.newest].state;
    if (formatsOnly.vtxDesc != g_gxState.vtxDesc) {
      ++sStats.redecodeDesc;
    } else {
      formatsOnly.arrays = {g_gxState.arrays[GX_VA_POS], g_gxState.arrays[GX_VA_CLR0]};
      ++(formatsOnly.matches_current() ? sStats.redecodeArray : sStats.redecodeFmt);
    }
  }
  list.newest = list.count < kVariantsPerList ? list.count++ : list.nextReplace++ % kVariantsPerList;
  Cached& cached = list.variants[list.newest];
  sLastFail = Fail::None;
  DecodeState& state = cached.state;
  state.fmtMask = 0;
  std::vector<vita_gl::Vertex> triangles = decode_list(*entry.bytes, size, state.fmtMask);
  if (triangles.empty()) {
    switch (sLastFail) {
    case Fail::MatrixIndex:
      ++sStats.failMatrixIndex;
      break;
    case Fail::Array:
      ++sStats.failArray;
      break;
    case Fail::Command:
      ++sStats.failCommand;
      break;
    default:
      ++sStats.failEmpty;
      break;
    }
  } else {
    ++sStats.decoded;
  }
  cached.mesh = triangles.empty() ? nullptr : vita_gl::make_mesh(std::move(triangles));
  state.vtxDesc = g_gxState.vtxDesc;
  for (u32 fmt = 0; fmt < MaxVtxFmt; ++fmt) {
    state.vtxFmts[fmt] = (state.fmtMask & (1u << fmt)) != 0 ? g_gxState.vtxFmts[fmt] : VtxFmt{};
  }
  state.arrays = {g_gxState.arrays[GX_VA_POS], g_gxState.arrays[GX_VA_CLR0]};
  return cached;
}
} // namespace

bool draw_resident(const void* key, const resident::Entry& entry, u32 size) noexcept {
  ZoneScoped;
  ++sStats.calls;
  // Positions made at run time (CPU skinning) change under the same list: not drawn yet. The
  // retail models' own arrays are retained too (port_retail_resident), whatever their byte
  // order; a workspace the game rewrites each frame is not.
  if (resident::find(g_gxState.arrays[GX_VA_POS].data) == nullptr) {
    ++sStats.nativePositions;
    return false;
  }
  const Cached& cached = lookup(key, entry, size);
  if (!cached.mesh) {
    return false;
  }
  vita_gl::Draw draw{.mesh = cached.mesh};
  const Mat3x4<float>& mv = g_gxState.pnMtx[g_gxState.currentPnMtx].pos;
  std::memcpy(draw.modelView, &mv, sizeof(draw.modelView));
  // GX's clip depth runs from -w (near) to 0; GL's from -w to w: row 2 becomes 2 * row 2 + row 3.
  const Mat4x4<float>& proj = g_gxState.proj;
  for (int c = 0; c < 4; ++c) {
    draw.projection[c] = proj.m0[c];
    draw.projection[4 + c] = proj.m1[c];
    draw.projection[8 + c] = 2.f * proj.m2[c] + proj.m3[c];
    draw.projection[12 + c] = proj.m3[c];
  }
  vita_gl::record(std::move(draw));
  ++sStats.recorded;
  return true;
}

void release(const void* key) noexcept { sCache.erase(key); }

void log_stats() noexcept {
  const Stats& s = sStats;
  Log.info("lists: {} calls, {} recorded, {} with positions not retained; decoded {}, not drawable {} "
           "(matrix index {}, array {}, command {}, empty {}); decoded again for a changed vertex descriptor {}, "
           "vertex format {}, array {}; {} cached",
           s.calls, s.recorded, s.nativePositions, s.decoded,
           s.failMatrixIndex + s.failArray + s.failCommand + s.failEmpty, s.failMatrixIndex, s.failArray,
           s.failCommand, s.failEmpty, s.redecodeDesc, s.redecodeFmt, s.redecodeArray, sCache.size());
  sStats = {};
}
} // namespace aurora::gx::vita
