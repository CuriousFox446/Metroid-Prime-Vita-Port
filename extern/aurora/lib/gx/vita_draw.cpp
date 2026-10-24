#include "vita_draw.hpp"

#include "../gfx/texture.hpp"
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
  u32 nativePositions = 0;
  u32 decoded = 0;
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
  // Recorded draws by their state.
  u32 blended = 0;
  u32 alphaTested = 0;
  u32 culled = 0;
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

// The list's triangles, or none when it holds anything this path cannot draw yet; fmtMask gets
// the vertex formats it read.
std::vector<vita_gl::Vertex> decode_list(const std::vector<u8>& bytes, u32 size, const TexAttrs& texAttrs,
                                         u8& fmtMask) noexcept {
  const bool textured = texAttrs[0] != kNoTexAttr;
  // Without a colour of their own, textured faces take their textures' as they are.
  const u8 grey = textured ? 255 : 200;
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
      if (vtx >= bytes.data() + end || !decode_vertex(vtx, g_gxState.vtxFmts[fmt], texAttrs, grey, prim[i])) {
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

// A TEV stage's texture as a unit of the draw.
struct UnitChoice {
  GXTexMapID map;
  const TcgConfig* tcg;
};

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
    out[count++] = {stage.texMapId, &tcg};
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

  // The textures, through Aurora's texture cache as the WebGPU draw would resolve them; a unit
  // whose texture has no Vita copy is left out.
  UnitChoice choices[kTexUnits];
  const int chosen = choose_units(choices);
  vita_gl::TexUnit units[kTexUnits];
  TexAttrs texAttrs = kNoTexAttrs;
  int unitCount = 0;
  if (chosen != 0) {
    static ShaderInfo sSampled;
    sSampled.sampledTextures.reset();
    for (int i = 0; i < chosen; ++i) {
      sSampled.sampledTextures.set(choices[i].map);
    }
    resolve_sampled_textures(sSampled);
    for (int i = 0; i < chosen; ++i) {
      const gfx::TextureBind& bind = g_gxState.textures[choices[i].map];
      if (!bind.ref || !bind.ref->vitaTexture) {
        ++sStats.unitsWithoutCopy;
        continue;
      }
      vita_gl::TexUnit& unit = units[unitCount];
      unit.texture = bind.ref->vitaTexture;
      tex_matrix(*choices[i].tcg, unit.matrix);
      unit.sampler = sampler_for(bind.texObj);
      texAttrs[unitCount] = static_cast<u8>(GX_VA_TEX0 + (choices[i].tcg->src - GX_TG_TEX0));
      ++unitCount;
    }
  }

  const Cached& cached = lookup(key, entry, size, texAttrs);
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
  for (int unit = 0; unit < unitCount; ++unit) {
    draw.units[unit] = std::move(units[unit]);
  }
  draw.state = render_state();
  sStats.blended += draw.state.blend ? 1 : 0;
  sStats.alphaTested += draw.state.alphaFunc != kGlAlways ? 1 : 0;
  sStats.culled += draw.state.cullFace != 0 ? 1 : 0;
  vita_gl::record(std::move(draw));
  ++sStats.recorded;
  ++sStats.drawsByUnits[unitCount];
  return true;
}

void release(const void* key) noexcept { sCache.erase(key); }

void log_stats() noexcept {
  const Stats& s = sStats;
  Log.info("lists: {} calls, {} recorded, {} with positions not retained; decoded {}, not drawable {} "
           "(matrix index {}, array {}, command {}, empty {}); decoded again for a changed vertex descriptor {}, "
           "textures {}, vertex format {}, array {}; {} cached",
           s.calls, s.recorded, s.nativePositions, s.decoded,
           s.failMatrixIndex + s.failArray + s.failCommand + s.failEmpty, s.failMatrixIndex, s.failArray,
           s.failCommand, s.failEmpty, s.redecodeDesc, s.redecodeTex, s.redecodeFmt, s.redecodeArray, sCache.size());
  Log.info("textures: draws with none {}, one {}, two {}; {} left out without a Vita copy; state: {} blended, {} "
           "alpha-tested, {} culled",
           s.drawsByUnits[0], s.drawsByUnits[1], s.drawsByUnits[2], s.unitsWithoutCopy, s.blended, s.alphaTested,
           s.culled);
  sStats = {};
}
} // namespace aurora::gx::vita
