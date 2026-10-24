#include "vita_draw.hpp"

#include "../gfx/texture.hpp"
#include "../internal.hpp"
#include "../vita_gl.hpp"
#include "gx.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <dolphin/gx/GXCommandList.h>

#include <tracy/Tracy.hpp>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace aurora::gx::vita {
namespace {
constexpr Module Log{"aurora::gx::vita"};
using vita_gl::kTexUnits;

// The vertex attribute each texture unit's coordinates come from (GX_VA_TEX0..7), or none.
constexpr u8 kNoTexAttr = 0xFF;
using TexAttrs = std::array<u8, kTexUnits>;
constexpr TexAttrs kNoTexAttrs{kNoTexAttr, kNoTexAttr};

// GL's enums for the samplers and state (vitaGL.h is not included here).
constexpr u16 kGlNearest = 0x2600;
constexpr u16 kGlLinear = 0x2601;
constexpr u16 kGlRepeat = 0x2901;
constexpr u16 kGlClampToEdge = 0x812F;
constexpr u16 kGlMirroredRepeat = 0x8370;
constexpr u16 kGlZero = 0;
constexpr u16 kGlOne = 1;
constexpr u16 kGlSrcColor = 0x0300;
constexpr u16 kGlOneMinusSrcColor = 0x0301;
constexpr u16 kGlSrcAlpha = 0x0302;
constexpr u16 kGlOneMinusSrcAlpha = 0x0303;
constexpr u16 kGlDstAlpha = 0x0304;
constexpr u16 kGlOneMinusDstAlpha = 0x0305;
constexpr u16 kGlDstColor = 0x0306;
constexpr u16 kGlOneMinusDstColor = 0x0307;
constexpr u16 kGlFuncAdd = 0x8006;
constexpr u16 kGlFuncReverseSubtract = 0x800B;
constexpr u16 kGlNever = 0x0200; // GL's compare functions run in GX's order from here
constexpr u16 kGlAlways = 0x0207;
constexpr u16 kGlModulate = 0x2100;
constexpr u16 kGlAdd = 0x0104;
constexpr u16 kGlFront = 0x0404;
constexpr u16 kGlBack = 0x0405;
constexpr u16 kGlFrontAndBack = 0x0408;

// The GX state a decode read: the vertex descriptor, the vertex formats the list's primitives
// name, the attributes the texture units' coordinates come from, and the arrays of the position,
// colour and those coordinates. The same list called with other state is decoded again; formats
// the list does not use change all the time (the HUD, particles) and do not count.
struct DecodeState {
  std::array<GXAttrType, MaxVtxAttr> vtxDesc{};
  u8 fmtMask = 0;
  std::array<VtxFmt, MaxVtxFmt> vtxFmts{}; // only those in fmtMask are compared
  TexAttrs texAttrs = kNoTexAttrs;
  std::array<AttrArray, 2 + kTexUnits> arrays{}; // as array_attrs lists them

  std::array<u8, 2 + kTexUnits> array_attrs() const noexcept {
    return {GX_VA_POS, GX_VA_CLR0, texAttrs[0], texAttrs[1]};
  }

  void capture_arrays() noexcept {
    const auto attrs = array_attrs();
    for (size_t i = 0; i < attrs.size(); ++i) {
      arrays[i] = attrs[i] == kNoTexAttr ? AttrArray{} : g_gxState.arrays[attrs[i]];
    }
  }

  bool matches_current(const TexAttrs& wanted) const noexcept {
    if (texAttrs != wanted || vtxDesc != g_gxState.vtxDesc) {
      return false;
    }
    for (u32 fmt = 0; fmt < MaxVtxFmt; ++fmt) {
      if ((fmtMask & (1u << fmt)) != 0 &&
          std::memcmp(&vtxFmts[fmt], &g_gxState.vtxFmts[fmt], sizeof(VtxFmt)) != 0) {
        return false;
      }
    }
    // An array counts only when the descriptor indexes it.
    const auto attrs = array_attrs();
    for (size_t i = 0; i < attrs.size(); ++i) {
      if (attrs[i] == kNoTexAttr || (vtxDesc[attrs[i]] != GX_INDEX8 && vtxDesc[attrs[i]] != GX_INDEX16)) {
        continue;
      }
      const AttrArray& current = g_gxState.arrays[attrs[i]];
      if (arrays[i].data != current.data || arrays[i].stride != current.stride ||
          arrays[i].baseIndex != current.baseIndex) {
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
  u32 decoded = 0;
  // List calls whose positions the CPU makes each frame (skinned models), decoded each call.
  u32 perCall = 0;
  u32 perCallFailed = 0;
  // Immediate draws (GXBegin, uncalled display lists, GXBeginIndexed).
  u32 immediate = 0;
  u32 immediateLines = 0;
  u32 immediateFailed = 0;
  u32 recorded = 0;
  u32 failMatrixIndex = 0;
  u32 failArray = 0;
  u32 failCommand = 0;
  u32 failEmpty = 0;
  // Decodes of a list already cached, by what differed from its newest variant.
  u32 redecodeDesc = 0;
  u32 redecodeTex = 0;
  u32 redecodeFmt = 0;
  u32 redecodeArray = 0;
  // Recorded draws by how many textures they take, and textures left out for want of a Vita copy
  // (EFB copies, dynamic textures).
  std::array<u32, kTexUnits + 1> drawsByUnits{};
  u32 unitsWithoutCopy = 0;
  // Recorded draws by their state (after the constant alpha is folded in).
  u32 blended = 0;
  u32 alphaTested = 0;
  u32 culled = 0;
  // TEV's final alpha: a constant worked out, of those the draws its alpha test rejects outright,
  // and the draws whose alpha reads a texture; the rest depend on a lit or vertex alpha.
  u32 alphaConstant = 0;
  u32 alphaRejected = 0;
  u32 alphaTexture = 0;
  // Vita copies made for textures cached before the Vita path drew with them.
  u32 lateCopies = 0;
  // Second units added on top (GL_ADD) rather than modulated.
  u32 additiveUnits = 0;
  // Draws whose material (current_material) was in the cache, and those that worked it out.
  u32 materialHits = 0;
  u32 materialMisses = 0;
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

// One GX vertex's position, colour (grey without one) and the texture units' coordinates, from
// its bytes in the list (which advance past it). False when it has a per-vertex matrix (not drawn
// yet) or reads outside an array.
bool decode_vertex(const u8*& vtx, const VtxFmt& fmt, const TexAttrs& texAttrs, u8 grey,
                   vita_gl::Vertex& out) noexcept {
  out = {};
  out.color[0] = out.color[1] = out.color[2] = grey;
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
    const bool wanted = attr == GX_VA_POS || attr == GX_VA_CLR0 || i == texAttrs[0] || i == texAttrs[1];
    const u32 elemSize = attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX
                             ? 1
                             : u32(comp_type_size(attr, attrFmt.type)) * comp_cnt_count(attr, attrFmt.cnt);
    const u8* data;
    bool le = false;
    if (type == GX_DIRECT) {
      data = vtx;
      vtx += elemSize;
      if (!wanted) {
        continue;
      }
    } else {
      const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
      const u32 index = type == GX_INDEX8 ? vtx[0] : read16(vtx, false);
      vtx += (type == GX_INDEX8 ? 1 : 2) * (nbt3 ? 3 : 1);
      if (!wanted) {
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
    const u32 cnt = comp_cnt_count(attr, attrFmt.cnt);
    const u32 step = comp_type_size(attr, attrFmt.type);
    if (attr == GX_VA_POS) {
      for (u32 c = 0; c < cnt && c < 3; ++c) {
        out.pos[c] = read_component(data + c * step, attrFmt.type, attrFmt.frac, le);
      }
    } else if (attr == GX_VA_CLR0) {
      read_color(data, attrFmt.type, out.color);
    } else {
      float st[2] = {};
      for (u32 c = 0; c < cnt && c < 2; ++c) {
        st[c] = read_component(data + c * step, attrFmt.type, attrFmt.frac, le);
      }
      for (int unit = 0; unit < kTexUnits; ++unit) {
        if (texAttrs[unit] == i) {
          out.uv[unit][0] = st[0];
          out.uv[unit][1] = st[1];
        }
      }
    }
  }
  return true;
}

void append_triangle(std::vector<vita_gl::Vertex>& out, const vita_gl::Vertex& a, const vita_gl::Vertex& b,
                     const vita_gl::Vertex& c, bool textured) {
  // A face normal, in the model's space, shades the face (the fixed-function draw has no light):
  // up-facing brightest (the game's world is Z-up), walls by which way they face, so the shapes
  // read. Less of it on textured faces, whose lightmaps carry their own light.
  const float u[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
  const float v[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
  float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  float shade = 1.f;
  if (len > 1e-8f) {
    const float facing = (0.65f * std::fabs(n[2]) + 0.35f * std::fabs(n[0])) / len;
    shade = textured ? 0.6f + 0.4f * facing : 0.3f + 0.7f * facing;
  }
  for (const vita_gl::Vertex* src : {&a, &b, &c}) {
    vita_gl::Vertex vertex = *src;
    for (int i = 0; i < 3; ++i) {
      vertex.color[i] = static_cast<u8>(std::min(255.f, vertex.color[i] * shade));
    }
    out.push_back(vertex);
  }
}

// Without a colour of their own, textured faces take their textures' as they are.
u8 grey_for(const TexAttrs& texAttrs) noexcept { return texAttrs[0] != kNoTexAttr ? 255 : 200; }

// Decodes `count` vertices from `vtx` (which advances past them), false when one cannot be.
bool decode_vertices(const u8*& vtx, const u8* end, GXVtxFmt fmt, u32 count, const TexAttrs& texAttrs,
                     std::vector<vita_gl::Vertex>& out) noexcept {
  const u8 grey = grey_for(texAttrs);
  out.resize(count);
  for (u32 i = 0; i < count; ++i) {
    if (vtx >= end || !decode_vertex(vtx, g_gxState.vtxFmts[fmt], texAttrs, grey, out[i])) {
      return false;
    }
  }
  return vtx <= end;
}

// One primitive's vertices as triangles, appended to `triangles`. Lines and points add none.
void triangulate(GXPrimitive type, const std::vector<vita_gl::Vertex>& prim, bool textured,
                 std::vector<vita_gl::Vertex>& triangles) noexcept {
  const u32 count = static_cast<u32>(prim.size());
  switch (type) {
  case GX_TRIANGLES:
    for (u32 i = 0; i + 2 < count; i += 3) {
      append_triangle(triangles, prim[i], prim[i + 1], prim[i + 2], textured);
    }
    break;
  case GX_TRIANGLESTRIP:
    for (u32 i = 0; i + 2 < count; ++i) {
      if ((i & 1) == 0) {
        append_triangle(triangles, prim[i], prim[i + 1], prim[i + 2], textured);
      } else {
        append_triangle(triangles, prim[i + 1], prim[i], prim[i + 2], textured);
      }
    }
    break;
  case GX_TRIANGLEFAN:
    for (u32 i = 1; i + 1 < count; ++i) {
      append_triangle(triangles, prim[0], prim[i], prim[i + 1], textured);
    }
    break;
  case GX_QUADS:
    for (u32 i = 0; i + 3 < count; i += 4) {
      append_triangle(triangles, prim[i], prim[i + 1], prim[i + 2], textured);
      append_triangle(triangles, prim[i], prim[i + 2], prim[i + 3], textured);
    }
    break;
  default:
    break; // lines and points: not drawn yet
  }
}

// The list's triangles, or none when it holds anything this path cannot draw yet; fmtMask gets
// the vertex formats it read.
std::vector<vita_gl::Vertex> decode_list(const std::vector<u8>& bytes, u32 size, const TexAttrs& texAttrs,
                                         u8& fmtMask) noexcept {
  const bool textured = texAttrs[0] != kNoTexAttr;
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
    const u8* vtx = bytes.data() + pos;
    if (!decode_vertices(vtx, bytes.data() + end, fmt, count, texAttrs, prim)) {
      return {};
    }
    pos = static_cast<size_t>(vtx - bytes.data());
    triangulate(type, prim, textured, triangles);
  }
  return triangles;
}

// The list's entry for the current state and texture coordinates, decoding it into a new or the
// oldest variant when none matches.
const Cached& lookup(const void* key, const resident::Entry& entry, u32 size, const TexAttrs& texAttrs) noexcept {
  CachedList& list = sCache[key];
  for (size_t i = 0; i < list.count; ++i) {
    if (list.variants[i].state.matches_current(texAttrs)) {
      return list.variants[i];
    }
  }
  ZoneScopedN("vita: decode a display list");
  if (list.count != 0) {
    DecodeState formatsOnly = list.variants[list.newest].state;
    if (formatsOnly.vtxDesc != g_gxState.vtxDesc) {
      ++sStats.redecodeDesc;
    } else if (formatsOnly.texAttrs != texAttrs) {
      ++sStats.redecodeTex;
    } else {
      formatsOnly.capture_arrays();
      ++(formatsOnly.matches_current(texAttrs) ? sStats.redecodeArray : sStats.redecodeFmt);
    }
  }
  list.newest = list.count < kVariantsPerList ? list.count++ : list.nextReplace++ % kVariantsPerList;
  Cached& cached = list.variants[list.newest];
  sLastFail = Fail::None;
  DecodeState& state = cached.state;
  state.fmtMask = 0;
  std::vector<vita_gl::Vertex> triangles = decode_list(*entry.bytes, size, texAttrs, state.fmtMask);
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
  state.texAttrs = texAttrs;
  state.capture_arrays();
  return cached;
}

// A TEV stage's texture as a unit of the draw, and whether the stage adds it to what came
// before (d a register, the texture's colour among a, b and c: a glow or incandescence pass, whose
// texture is black but for its lights) rather than multiplying or blending with it.
struct UnitChoice {
  GXTexMapID map;
  const TcgConfig* tcg;
  bool additive;
};

bool adds_texture(const TevStage& stage) noexcept {
  const auto& pass = stage.colorPass;
  const bool registerD = pass.d == GX_CC_CPREV || pass.d == GX_CC_C0 || pass.d == GX_CC_C1 || pass.d == GX_CC_C2;
  return registerD && stage.colorOp.op == GX_TEV_ADD;
}

// The textures a draw takes, in TEV order: those of stages whose colour reads their texture
// (TEXC), with coordinates a matrix makes from a vertex attribute the list has, up to the units
// there are. A lightmap and a diffuse pass modulate as GX's stages would; the rest of GX's
// combinations (reflections, glow, alpha) come with the materials (milestone 4).
int choose_units(UnitChoice out[kTexUnits]) noexcept {
  int count = 0;
  for (u32 s = 0; s < g_gxState.numTevStages && s < MaxTevStages && count < kTexUnits; ++s) {
    const TevStage& stage = g_gxState.tevStages[s];
    if (stage.texMapId >= MaxTextures || stage.texCoordId >= MaxTexCoord) {
      continue;
    }
    const auto& pass = stage.colorPass;
    if (pass.a != GX_CC_TEXC && pass.b != GX_CC_TEXC && pass.c != GX_CC_TEXC && pass.d != GX_CC_TEXC) {
      continue;
    }
    bool repeated = false;
    for (int unit = 0; unit < count; ++unit) {
      repeated = repeated || out[unit].map == stage.texMapId;
    }
    const TcgConfig& tcg = g_gxState.tcgs[stage.texCoordId];
    if (repeated || (tcg.type != GX_TG_MTX2x4 && tcg.type != GX_TG_MTX3x4) || tcg.src < GX_TG_TEX0 ||
        tcg.src > GX_TG_TEX7 || g_gxState.vtxDesc[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)] == GX_NONE) {
      continue;
    }
    out[count++] = {stage.texMapId, &tcg, adds_texture(stage)};
  }
  return count;
}

// The texture coordinates' matrix, GX's rows 0 and 1 folded for GL: GX multiplies (s, t, 1, 1),
// vitaGL's fixed-function shader (s, t, 0, 1), so column 3 takes columns 2 and 3. A 3x4 matrix's
// third row (a projection) is dropped; the attribute sources used here do not need it.
void tex_matrix(const TcgConfig& tcg, float out[8]) noexcept {
  static constexpr float kIdentityRows[8] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f};
  const float* rows = kIdentityRows;
  if (tcg.mtx != GX_IDENTITY) {
    // GX numbers the position matrices first, then the texture matrices, three rows apiece.
    const u32 index = static_cast<u32>(tcg.mtx) / 3;
    if (index < MaxPnMtx) {
      rows = reinterpret_cast<const float*>(&g_gxState.pnMtx[index].pos);
    } else if (index - MaxPnMtx < MaxTexMtx) {
      rows = reinterpret_cast<const float*>(&g_gxState.texMtxs[index - MaxPnMtx]);
    }
  }
  for (int r = 0; r < 2; ++r) {
    out[r * 4 + 0] = rows[r * 4 + 0];
    out[r * 4 + 1] = rows[r * 4 + 1];
    out[r * 4 + 2] = 0.f;
    out[r * 4 + 3] = rows[r * 4 + 2] + rows[r * 4 + 3];
  }
}

u16 gl_compare(GXCompare compare) noexcept { return static_cast<u16>(kGlNever + (compare & 7)); }

// GX's blend factors as Aurora's WebGPU path maps them (gx.cpp): the colour factors name the
// other side's colour (a source factor of SRCCLR is the destination's colour).
u16 gl_blend_factor(GXBlendFactor factor, bool isDst) noexcept {
  switch (factor) {
  case GX_BL_ZERO:
    return kGlZero;
  case GX_BL_ONE:
    return kGlOne;
  case GX_BL_SRCCLR:
    return isDst ? kGlSrcColor : kGlDstColor;
  case GX_BL_INVSRCCLR:
    return isDst ? kGlOneMinusSrcColor : kGlOneMinusDstColor;
  case GX_BL_SRCALPHA:
    return kGlSrcAlpha;
  case GX_BL_INVSRCALPHA:
    return kGlOneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return kGlDstAlpha;
  case GX_BL_INVDSTALPHA:
    return kGlOneMinusDstAlpha;
  default:
    return isDst ? kGlZero : kGlOne;
  }
}

// GX's blending, alpha test, depth, colour mask and culling for the fixed-function draw.
vita_gl::RenderState render_state() noexcept {
  vita_gl::RenderState state;
  switch (g_gxState.blendMode) {
  case GX_BM_BLEND:
    state.blend = true;
    state.blendSrc = gl_blend_factor(g_gxState.blendFacSrc, false);
    state.blendDst = gl_blend_factor(g_gxState.blendFacDst, true);
    break;
  case GX_BM_SUBTRACT:
    state.blend = true;
    state.blendEquation = kGlFuncReverseSubtract;
    state.blendSrc = kGlOne;
    state.blendDst = kGlOne;
    break;
  case GX_BM_LOGIC:
    // The logic ops the WebGPU path draws: clear, and no-op (keep the destination); the rest copy.
    if (g_gxState.blendOp == GX_LO_CLEAR || g_gxState.blendOp == GX_LO_NOOP) {
      state.blend = true;
      state.blendSrc = kGlZero;
      state.blendDst = g_gxState.blendOp == GX_LO_NOOP ? kGlOne : kGlZero;
    }
    break;
  default:
    break;
  }

  // GL's alpha test takes one comparison, GX's two joined by an operator: the one that is not
  // ALWAYS under AND (the usual cut-out), none when either passes under OR, else the first.
  const AlphaCompare& alpha = g_gxState.alphaCompare;
  GXCompare compare = alpha.comp0;
  u32 ref = alpha.ref0;
  if (alpha.op == GX_AOP_AND && alpha.comp0 == GX_ALWAYS) {
    compare = alpha.comp1;
    ref = alpha.ref1;
  } else if (alpha.op == GX_AOP_OR && (alpha.comp0 == GX_ALWAYS || alpha.comp1 == GX_ALWAYS)) {
    compare = GX_ALWAYS;
  }
  state.alphaFunc = gl_compare(compare);
  state.alphaRef = static_cast<float>(ref & 0xFF) / 255.f;

  // GX writes depth with its test off when asked to (the test then always passes); GL does not
  // with GL_DEPTH_TEST off, so that case keeps the test on with ALWAYS.
  state.depthTest = g_gxState.depthCompare || g_gxState.depthUpdate;
  state.depthFunc = g_gxState.depthCompare ? gl_compare(g_gxState.depthFunc) : kGlAlways;
  state.depthWrite = g_gxState.depthUpdate;
  state.colorWrite = g_gxState.colorUpdate;
  state.alphaWrite = g_gxState.alphaUpdate;

  switch (g_gxState.cullMode) {
  case GX_CULL_FRONT:
    state.cullFace = kGlFront;
    break;
  case GX_CULL_BACK:
    state.cullFace = kGlBack;
    break;
  case GX_CULL_ALL:
    state.cullFace = kGlFrontAndBack;
    break;
  default:
    state.cullFace = 0;
    break;
  }
  return state;
}

// A texture Aurora cached before the Vita path first drew with it (made for a WebGPU draw, with no
// Vita copy) gets its copy now, from the source the game is drawing with. Not for palette
// formats (their colours come from a TLUT) or render targets.
void add_missing_copy(gfx::TextureRef& ref, const GXTexObj_& obj) noexcept {
  const u32 format = obj.format();
  if (ref.attachmentTextureView || !obj.has_data() || format == GX_TF_C4 || format == GX_TF_C8 ||
      format == GX_TF_C14X2 || obj.width() != ref.size.width || obj.height() != ref.size.height) {
    return;
  }
  const size_t bytes = texture::texture_source_size(format, obj.width(), obj.height(), obj.mip_count());
  ref.vitaTexture = gfx::make_vita_texture(obj.width(), obj.height(), format,
                                           {static_cast<const u8*>(obj.data), bytes});
  ++sStats.lateCopies;
}

// TEV's final alpha, worked out on the CPU where it does not come from a texture: the fixed-function
// draw's own alpha is the vertex colour's times the textures', which on most of the game's
// surfaces is not what TEV computes (an opaque surface is often drawn blended with an alpha of
// one from a constant, over textures whose alpha holds something else).
struct TevAlpha {
  float value = 0.f;
  bool known = true;    // a constant, worked out
  bool texture = false; // reads a texture's alpha
};

TevAlpha konst_alpha(const TevStage& stage) noexcept {
  const u32 sel = stage.kaSel;
  if (sel <= GX_TEV_KASEL_1_8) {
    return {static_cast<float>(8 - sel) / 8.f};
  }
  if (sel >= GX_TEV_KASEL_K0_R && sel <= GX_TEV_KASEL_K3_A) {
    const u32 component = (sel - GX_TEV_KASEL_K0_R) / 4;
    const u32 reg = (sel - GX_TEV_KASEL_K0_R) % 4;
    return {g_gxState.kcolors[reg][component]};
  }
  return {.known = false};
}

// The rasterised alpha: a constant when the stage's channel is unlit and takes its material
// alpha from the register.
TevAlpha raster_alpha(const TevStage& stage) noexcept {
  switch (stage.channelId) {
  case GX_COLOR0:
  case GX_COLOR1:
  case GX_ALPHA0:
  case GX_ALPHA1:
  case GX_COLOR0A0:
  case GX_COLOR1A1:
    break;
  case GX_COLOR_ZERO:
  case GX_COLOR_NULL:
    return {0.f};
  default:
    return {.known = false};
  }
  if (g_gxState.tevSwapTable[stage.tevSwapRas].alpha != GX_CH_ALPHA) {
    return {.known = false};
  }
  const u32 channel = GX_ALPHA0 + color_channel(stage.channelId);
  const ColorChannelConfig& config = g_gxState.colorChannelConfig[channel];
  if (config.lightingEnabled || config.matSrc != GX_SRC_REG) {
    return {.known = false};
  }
  return {g_gxState.colorChannelState[channel].matColor[3]};
}

TevAlpha tev_alpha() noexcept {
  std::array<TevAlpha, MaxTevRegs> regs;
  for (u32 i = 0; i < MaxTevRegs; ++i) {
    regs[i] = {g_gxState.colorRegs[i][3]};
  }
  TevAlpha last = regs[GX_TEVPREV];
  for (u32 s = 0; s < g_gxState.numTevStages && s < MaxTevStages; ++s) {
    const TevStage& stage = g_gxState.tevStages[s];
    const auto input = [&](GXTevAlphaArg arg) -> TevAlpha {
      switch (arg) {
      case GX_CA_APREV:
      case GX_CA_A0:
      case GX_CA_A1:
      case GX_CA_A2:
        return regs[arg - GX_CA_APREV];
      case GX_CA_TEXA:
        return {.known = false, .texture = true};
      case GX_CA_RASA:
        return raster_alpha(stage);
      case GX_CA_KONST:
        return konst_alpha(stage);
      default:
        return {0.f};
      }
    };
    const TevAlpha a = input(stage.alphaPass.a);
    const TevAlpha b = input(stage.alphaPass.b);
    const TevAlpha c = input(stage.alphaPass.c);
    const TevAlpha d = input(stage.alphaPass.d);
    const TevOp& op = stage.alphaOp;
    TevAlpha out;
    out.texture = a.texture || b.texture || c.texture || d.texture;
    // The compare ops are not worked out.
    out.known = a.known && b.known && c.known && d.known && op.op <= GX_TEV_SUB;
    if (out.known) {
      const float cc = std::clamp(c.value, 0.f, 1.f);
      const float lerp = a.value * (1.f - cc) + b.value * cc;
      float value = op.op == GX_TEV_SUB ? d.value - lerp : d.value + lerp;
      value += op.bias == GX_TB_ADDHALF ? 0.5f : op.bias == GX_TB_SUBHALF ? -0.5f : 0.f;
      constexpr float kScale[] = {1.f, 2.f, 4.f, 0.5f};
      value *= kScale[op.scale & 3];
      out.value = op.clamp ? std::clamp(value, 0.f, 1.f) : value;
    }
    regs[op.outReg & 3] = out;
    last = out;
  }
  return last;
}

bool alpha_compare(GXCompare compare, u32 alpha, u32 ref) noexcept {
  switch (compare) {
  case GX_NEVER:
    return false;
  case GX_LESS:
    return alpha < ref;
  case GX_EQUAL:
    return alpha == ref;
  case GX_LEQUAL:
    return alpha <= ref;
  case GX_GREATER:
    return alpha > ref;
  case GX_NEQUAL:
    return alpha != ref;
  case GX_GEQUAL:
    return alpha >= ref;
  default:
    return true;
  }
}

// With TEV's alpha a known constant, the alpha test and blending need not read the draw's alpha:
// the test passes or fails for the whole draw, and blending by a source alpha of one or zero is a
// fixed blend. False: the draw fails the alpha test everywhere and draws nothing.
bool fold_constant_alpha(float alpha, vita_gl::RenderState& state) noexcept {
  const u32 alpha8 = static_cast<u32>(std::lround(std::clamp(alpha, 0.f, 1.f) * 255.f));
  const AlphaCompare& compare = g_gxState.alphaCompare;
  const bool pass0 = alpha_compare(compare.comp0, alpha8, compare.ref0 & 0xFF);
  const bool pass1 = alpha_compare(compare.comp1, alpha8, compare.ref1 & 0xFF);
  bool pass = true;
  switch (compare.op) {
  case GX_AOP_AND:
    pass = pass0 && pass1;
    break;
  case GX_AOP_OR:
    pass = pass0 || pass1;
    break;
  case GX_AOP_XOR:
    pass = pass0 != pass1;
    break;
  case GX_AOP_XNOR:
    pass = pass0 == pass1;
    break;
  default:
    break;
  }
  if (!pass) {
    return false;
  }
  state.alphaFunc = kGlAlways;
  if (state.blend && (alpha8 == 255 || alpha8 == 0)) {
    const auto fold = [alpha8](u16 factor) -> u16 {
      if (factor == kGlSrcAlpha) {
        return alpha8 == 255 ? kGlOne : kGlZero;
      }
      if (factor == kGlOneMinusSrcAlpha) {
        return alpha8 == 255 ? kGlZero : kGlOne;
      }
      return factor;
    };
    state.blendSrc = fold(state.blendSrc);
    state.blendDst = fold(state.blendDst);
  }
  return true;
}

vita_gl::Sampler sampler_for(const GXTexObj_& obj) noexcept {
  const auto wrap = [](GXTexWrapMode mode) -> u16 {
    switch (mode) {
    case GX_CLAMP:
      return kGlClampToEdge;
    case GX_MIRROR:
      return kGlMirroredRepeat;
    default:
      return kGlRepeat;
    }
  };
  // Only the first level is copied for now, so mipmapped filters sample it alone.
  const GXTexFilter minFilter = obj.min_filter();
  const bool minLinear = minFilter == GX_LINEAR || minFilter == GX_LIN_MIP_NEAR || minFilter == GX_LIN_MIP_LIN;
  return {
      .wrapS = wrap(obj.wrap_s()),
      .wrapT = wrap(obj.wrap_t()),
      .minFilter = minLinear ? kGlLinear : kGlNearest,
      .magFilter = obj.mag_filter() == GX_LINEAR ? kGlLinear : kGlNearest,
  };
}
// The draw's textures as units, through Aurora's texture cache as the WebGPU draw would resolve
// them. texAttrs gets the attributes their coordinates come from. Returns how many units there
// are, or -1 when a texture has no Vita copy (an EFB copy, a dynamic texture): drawn without it,
// a full-screen pass over a copy of the screen would cover the screen in its vertex colour.
int resolve_units(vita_gl::TexUnit units[kTexUnits], const TcgConfig* tcgs[kTexUnits], TexAttrs& texAttrs) noexcept {
  UnitChoice choices[kTexUnits];
  const int chosen = choose_units(choices);
  texAttrs = kNoTexAttrs;
  int unitCount = 0;
  if (chosen != 0) {
    static ShaderInfo sSampled;
    sSampled.sampledTextures.reset();
    for (int i = 0; i < chosen; ++i) {
      sSampled.sampledTextures.set(choices[i].map);
    }
    {
      const gfx::VitaCopyScope copies;
      resolve_sampled_textures(sSampled);
    }
    for (int i = 0; i < chosen; ++i) {
      const gfx::TextureBind& bind = g_gxState.textures[choices[i].map];
      if (bind.ref && !bind.ref->vitaTexture) {
        add_missing_copy(*bind.ref, g_gxState.loadedTextures[choices[i].map]);
      }
      if (!bind.ref || !bind.ref->vitaTexture) {
        ++sStats.unitsWithoutCopy;
        return -1;
      }
      vita_gl::TexUnit& unit = units[unitCount];
      unit.texture = bind.ref->vitaTexture;
      unit.sampler = sampler_for(bind.texObj);
      // The first unit takes the vertex colour, so it always modulates; a later one adds when its
      // stage does.
      unit.envMode = unitCount > 0 && choices[i].additive ? kGlAdd : kGlModulate;
      tcgs[unitCount] = choices[i].tcg;
      texAttrs[unitCount] = static_cast<u8>(GX_VA_TEX0 + (choices[i].tcg->src - GX_TG_TEX0));
      ++unitCount;
    }
  }
  return unitCount;
}

// What the GX state decides for a draw apart from its matrices: its texture units (textures,
// samplers, combine), the attributes their coordinates come from, and the fixed-function state
// with TEV's constant alpha folded in. Working it out is most of a call's cost, and the game draws
// run after run of calls under one material, so it is kept, by a hash of the state it reads.
struct Material {
  u64 hash = 0;
  bool valid = false;
  u32 window = 0;     // the stats window it was last used in (log_stats frees the idle ones)
  int unitCount = 0;  // -1: a texture without a Vita copy (resolve_units)
  bool rejected = false; // its alpha test fails everywhere
  u8 alphaKind = 0;   // 1: a worked-out constant, 2: read from a texture
  TexAttrs texAttrs = kNoTexAttrs;
  vita_gl::TexUnit units[kTexUnits];
  const TcgConfig* tcgs[kTexUnits] = {};
  vita_gl::RenderState state;
};
constexpr size_t kMaterials = 256;
std::array<Material, kMaterials> sMaterials;
u32 sWindow = 0;

// A hash of the GX state a Material reads: the TEV stages, texture coordinate generation, the
// vertex descriptor (which texture coordinates the list has), the loaded textures and their
// palettes, Aurora's texture bind generation, blending, depth, culling and the colour mask, the
// alpha compare, and the constants TEV's alpha may read (konst and register colours, the alpha
// channels' configuration and material colours, the swap tables). Not the matrices: those are
// read for every draw.
u64 material_hash() noexcept {
  ZoneScopedN("vita: material hash");
  const GXState& s = g_gxState;
  const u64 generation = texture::current_bind_generation();
  const AlphaCompare& alpha = s.alphaCompare;
  const u32 fixed[] = {
      s.numTevStages,  static_cast<u32>(s.blendMode),   static_cast<u32>(s.blendFacSrc),
      static_cast<u32>(s.blendFacDst),  static_cast<u32>(s.blendOp), static_cast<u32>(s.depthFunc),
      s.depthCompare,  s.depthUpdate,   s.colorUpdate,  s.alphaUpdate, static_cast<u32>(s.cullMode),
      static_cast<u32>(alpha.comp0),    alpha.ref0,     static_cast<u32>(alpha.op),
      static_cast<u32>(alpha.comp1),    alpha.ref1,     static_cast<u32>(generation),
      static_cast<u32>(generation >> 32),
  };
  u64 h = XXH3_64bits(fixed, sizeof(fixed));
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  h = XXH3_64bits_withSeed(s.tevStages.data(), stages * sizeof(TevStage), h);
  h = XXH3_64bits_withSeed(s.tcgs.data(), sizeof(s.tcgs), h);
  h = XXH3_64bits_withSeed(s.vtxDesc.data(), sizeof(s.vtxDesc), h);
  // A texture by what identifies its contents and sampling, and a palette texture's palette.
  u32 textures[MaxTextures][12];
  for (u32 i = 0; i < MaxTextures; ++i) {
    const GXTexObj_& obj = s.loadedTextures[i];
    const u32 format = obj.format();
    const bool palette = format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2;
    const GXTlutObj_* tlut =
        palette && static_cast<size_t>(obj.tlut) < s.loadedTluts.size() ? &s.loadedTluts[obj.tlut] : nullptr;
    textures[i][0] = static_cast<u32>(reinterpret_cast<uintptr_t>(obj.data));
    textures[i][1] = obj.texObjId;
    textures[i][2] = obj.texDataVersion;
    textures[i][3] = obj.mode0;
    textures[i][4] = obj.mode1;
    textures[i][5] = obj.image0;
    textures[i][6] = obj.mWidth;
    textures[i][7] = obj.mHeight;
    textures[i][8] = obj.mFormat;
    textures[i][9] = obj.flags;
    textures[i][10] = tlut != nullptr ? tlut->tlutObjId : 0;
    textures[i][11] = tlut != nullptr ? tlut->tlutDataVersion : 0;
  }
  h = XXH3_64bits_withSeed(textures, sizeof(textures), h);
  h = XXH3_64bits_withSeed(s.kcolors.data(), sizeof(s.kcolors), h);
  h = XXH3_64bits_withSeed(s.colorRegs.data(), sizeof(s.colorRegs), h);
  h = XXH3_64bits_withSeed(s.colorChannelConfig.data(), sizeof(s.colorChannelConfig), h);
  const float alphaMaterials[2] = {s.colorChannelState[GX_ALPHA0].matColor[3],
                                   s.colorChannelState[GX_ALPHA1].matColor[3]};
  h = XXH3_64bits_withSeed(alphaMaterials, sizeof(alphaMaterials), h);
  h = XXH3_64bits_withSeed(s.tevSwapTable.data(), sizeof(s.tevSwapTable), h);
  return h;
}

// The Material for the current GX state, worked out when the cache does not hold it.
const Material& current_material() noexcept {
  const u64 hash = material_hash();
  Material& material = sMaterials[hash % kMaterials];
  material.window = sWindow;
  if (material.valid && material.hash == hash) {
    ++sStats.materialHits;
    return material;
  }
  ZoneScopedN("vita: work out a material");
  ++sStats.materialMisses;
  material = {};
  material.hash = hash;
  material.valid = true;
  material.window = sWindow;
  material.unitCount = resolve_units(material.units, material.tcgs, material.texAttrs);
  if (material.unitCount < 0) {
    return material;
  }
  material.state = render_state();
  const TevAlpha alpha = tev_alpha();
  if (alpha.known) {
    material.alphaKind = 1;
    material.rejected = !fold_constant_alpha(alpha.value, material.state);
  } else if (alpha.texture) {
    material.alphaKind = 2;
  }
  vita_gl::RenderState& state = material.state;
  if (state.blend && state.blendEquation == kGlFuncAdd && state.blendSrc == kGlOne && state.blendDst == kGlZero) {
    state.blend = false;
  }
  return material;
}

// Records a draw of `mesh` with GX's current matrices and the material's units and state.
void record_draw(std::shared_ptr<vita_gl::Mesh> mesh, const Material& material) noexcept {
  ZoneScopedN("vita: record a draw");
  if (material.alphaKind == 1) {
    ++sStats.alphaConstant;
  } else if (material.alphaKind == 2) {
    ++sStats.alphaTexture;
  }
  if (material.rejected) {
    ++sStats.alphaRejected;
    return; // its alpha test fails everywhere: nothing to draw
  }
  vita_gl::Draw draw{.mesh = std::move(mesh)};
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
  for (int unit = 0; unit < material.unitCount; ++unit) {
    draw.units[unit] = material.units[unit];
    // Texture matrices animate (scrolling, pulsing): read for every draw.
    tex_matrix(*material.tcgs[unit], draw.units[unit].matrix);
    sStats.additiveUnits += draw.units[unit].envMode == kGlAdd ? 1 : 0;
  }
  draw.state = material.state;
  sStats.blended += draw.state.blend ? 1 : 0;
  sStats.alphaTested += draw.state.alphaFunc != kGlAlways ? 1 : 0;
  sStats.culled += draw.state.cullFace != 0 ? 1 : 0;
  vita_gl::record(std::move(draw));
  ++sStats.recorded;
  ++sStats.drawsByUnits[material.unitCount];
}
} // namespace

bool draw_resident(const void* key, const resident::Entry& entry, u32 size) noexcept {
  ZoneScoped;
  ++sStats.calls;
  const Material& material = current_material();
  if (material.unitCount < 0) {
    return false;
  }
  const TexAttrs& texAttrs = material.texAttrs;
  // Positions made at run time (CPU skinning) change under the same list, so such a call is
  // decoded each time, into a mesh for this frame alone. The retail models' own arrays are
  // retained (port_retail_resident), whatever their byte order; a workspace the game rewrites
  // each frame is not.
  if (resident::find(g_gxState.arrays[GX_VA_POS].data) == nullptr) {
    ZoneScopedN("vita: decode a list call");
    u8 fmtMask = 0;
    sLastFail = Fail::None;
    std::vector<vita_gl::Vertex> triangles = decode_list(*entry.bytes, size, texAttrs, fmtMask);
    if (triangles.empty()) {
      ++sStats.perCallFailed;
      return false;
    }
    ++sStats.perCall;
    record_draw(vita_gl::make_mesh(std::move(triangles)), material);
    return true;
  }
  const Cached& cached = [&]() -> const Cached& {
    ZoneScopedN("vita: list lookup");
    return lookup(key, entry, size, texAttrs);
  }();
  if (!cached.mesh) {
    return false;
  }
  record_draw(cached.mesh, material);
  return true;
}

bool draw_immediate(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, std::span<const u8> vertices) noexcept {
  ZoneScoped;
  if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    ++sStats.immediateLines;
    return false;
  }
  const Material& material = current_material();
  if (material.unitCount < 0) {
    return false;
  }
  const TexAttrs& texAttrs = material.texAttrs;
  std::vector<vita_gl::Vertex> prims;
  const u8* vtx = vertices.data();
  sLastFail = Fail::None;
  if (!decode_vertices(vtx, vertices.data() + vertices.size(), fmt, vtxCount, texAttrs, prims)) {
    ++sStats.immediateFailed;
    return false;
  }
  std::vector<vita_gl::Vertex> triangles;
  triangulate(prim, prims, texAttrs[0] != kNoTexAttr, triangles);
  if (triangles.empty()) {
    ++sStats.immediateFailed;
    return false;
  }
  ++sStats.immediate;
  record_draw(vita_gl::make_mesh(std::move(triangles)), material);
  return true;
}

bool draw_indexed(GXVtxFmt fmt, u16 vtxCount, std::span<const u8> vertices, std::span<const u8> indices) noexcept {
  ZoneScoped;
  const Material& material = current_material();
  if (material.unitCount < 0) {
    return false;
  }
  const TexAttrs& texAttrs = material.texAttrs;
  std::vector<vita_gl::Vertex> prims;
  const u8* vtx = vertices.data();
  sLastFail = Fail::None;
  if (!decode_vertices(vtx, vertices.data() + vertices.size(), fmt, vtxCount, texAttrs, prims)) {
    ++sStats.immediateFailed;
    return false;
  }
  // GXBeginIndexed's indices are 16-bit, in the host's byte order, three to a triangle.
  const bool textured = texAttrs[0] != kNoTexAttr;
  const size_t indexCount = indices.size() / sizeof(u16);
  std::vector<vita_gl::Vertex> triangles;
  triangles.reserve(indexCount);
  for (size_t i = 0; i + 2 < indexCount; i += 3) {
    u16 idx[3];
    std::memcpy(idx, indices.data() + i * sizeof(u16), sizeof(idx));
    if (idx[0] >= prims.size() || idx[1] >= prims.size() || idx[2] >= prims.size()) {
      ++sStats.immediateFailed;
      return false;
    }
    append_triangle(triangles, prims[idx[0]], prims[idx[1]], prims[idx[2]], textured);
  }
  if (triangles.empty()) {
    return false;
  }
  ++sStats.immediate;
  record_draw(vita_gl::make_mesh(std::move(triangles)), material);
  return true;
}

void release(const void* key) noexcept { sCache.erase(key); }

void log_stats() noexcept {
  const Stats& s = sStats;
  // Materials no draw used this window go, with the textures they hold.
  u32 materials = 0;
  for (Material& material : sMaterials) {
    if (material.valid && material.window != sWindow) {
      material = {};
    }
    materials += material.valid ? 1 : 0;
  }
  ++sWindow;
  Log.info("materials: {} draws found theirs cached, {} worked one out; {} cached", s.materialHits,
           s.materialMisses, materials);
  Log.info("immediate draws: {} drawn, {} lines or points, {} not decodable; list calls with positions made "
           "each frame: {} decoded and drawn, {} not decodable",
           s.immediate, s.immediateLines, s.immediateFailed, s.perCall, s.perCallFailed);
  Log.info("lists: {} calls, {} recorded; decoded {}, not drawable {} "
           "(matrix index {}, array {}, command {}, empty {}); decoded again for a changed vertex descriptor {}, "
           "textures {}, vertex format {}, array {}; {} cached",
           s.calls, s.recorded, s.decoded,
           s.failMatrixIndex + s.failArray + s.failCommand + s.failEmpty, s.failMatrixIndex, s.failArray,
           s.failCommand, s.failEmpty, s.redecodeDesc, s.redecodeTex, s.redecodeFmt, s.redecodeArray, sCache.size());
  Log.info("textures: draws with none {}, one {}, two {}; {} draws left out for a texture without a Vita copy, {} "
           "copies made late; "
           "{} second units added; state: {} blended, {} alpha-tested, {} culled; TEV alpha: {} constant ({} "
           "rejected), {} from a texture",
           s.drawsByUnits[0], s.drawsByUnits[1], s.drawsByUnits[2], s.unitsWithoutCopy, s.lateCopies, s.additiveUnits,
           s.blended, s.alphaTested, s.culled, s.alphaConstant, s.alphaRejected, s.alphaTexture);
  sStats = {};
}
} // namespace aurora::gx::vita
