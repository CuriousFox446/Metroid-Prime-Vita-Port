#include "vita_draw.hpp"

#include "../gfx/texture.hpp"
#include "../internal.hpp"
#include "../vita_gl.hpp"
#include "gx.hpp"
#include "shader_info.hpp"
#include "texture.hpp"
#include "vita_tev.hpp"

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
using vita_gl::kSamplers;
using vita_gl::kTexCoords;
using vita_gl::kTexUnits;

// The vertex attribute each of a mesh's texture coordinate sets comes from (GX_VA_TEX0..7), or none.
constexpr u8 kNoTexAttr = 0xFF;
using TexAttrs = std::array<u8, kTexCoords>;
constexpr TexAttrs kNoTexAttrs{kNoTexAttr, kNoTexAttr, kNoTexAttr, kNoTexAttr};

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

// A hash of the vertex descriptor and the vertex formats in `fmtMask`, worked out once a layout:
// remembered for each mask until a descriptor or format write moves GXState::vtxLayoutVersion.
// The current state is hot (the FIFO thread just wrote it), so a list call compares one number.
u64 layout_hash(u8 fmtMask) noexcept {
  struct Memo {
    u32 version = 0;
    u64 hash = 0;
  };
  static std::array<Memo, 256> sMemo;
  Memo& memo = sMemo[fmtMask];
  if (memo.version == g_gxState.vtxLayoutVersion) {
    return memo.hash;
  }
  u64 hash = XXH3_64bits_withSeed(g_gxState.vtxDesc.data(), sizeof(g_gxState.vtxDesc), fmtMask);
  for (u32 fmt = 0; fmt < MaxVtxFmt; ++fmt) {
    if ((fmtMask & (1u << fmt)) != 0) {
      hash = XXH3_64bits_withSeed(&g_gxState.vtxFmts[fmt], sizeof(VtxFmt), hash);
    }
  }
  memo = {g_gxState.vtxLayoutVersion, hash};
  return hash;
}

// The GX state a decode read: the vertex descriptor and the vertex formats the list's primitives
// name (as layout_hash), the attributes the texture units' coordinates come from, and the arrays
// of the position, colour and those coordinates. The same list called with other state is decoded
// again; formats the list does not use change all the time (the HUD, particles) and do not count.
// Kept small: every list call compares it, from memory gone cold since the last frame (it held
// all eight formats once, 2.6 KiB, which cost some 8 us a call).
struct DecodeState {
  struct ArrayRef {
    const void* data = nullptr;
    u32 stride = 0;
    u32 baseIndex = 0;
  };
  u64 layout = 0;
  u8 fmtMask = 0;
  TexAttrs texAttrs = kNoTexAttrs;
  std::array<ArrayRef, 2 + kTexCoords> arrays{}; // as array_attrs lists them

  std::array<u8, 2 + kTexCoords> array_attrs() const noexcept {
    return {GX_VA_POS, GX_VA_CLR0, texAttrs[0], texAttrs[1], texAttrs[2], texAttrs[3]};
  }

  // After a decode with `wanted` that named the formats in fmtMask: the current state.
  void capture(const TexAttrs& wanted) noexcept {
    texAttrs = wanted;
    layout = layout_hash(fmtMask);
    const auto attrs = array_attrs();
    for (size_t i = 0; i < attrs.size(); ++i) {
      const AttrArray* current = attrs[i] == kNoTexAttr ? nullptr : &g_gxState.arrays[attrs[i]];
      arrays[i] = current == nullptr ? ArrayRef{} : ArrayRef{current->data, current->stride, current->baseIndex};
    }
  }

  // skipPositions: the position array does not count (a skinned template reads it as it is).
  bool matches_current(const TexAttrs& wanted, bool skipPositions = false) const noexcept {
    if (texAttrs != wanted || layout != layout_hash(fmtMask)) {
      return false;
    }
    // An array counts only when the descriptor (the same, as the layout is) indexes it.
    const auto attrs = array_attrs();
    for (size_t i = skipPositions ? 1 : 0; i < attrs.size(); ++i) {
      if (attrs[i] == kNoTexAttr ||
          (g_gxState.vtxDesc[attrs[i]] != GX_INDEX8 && g_gxState.vtxDesc[attrs[i]] != GX_INDEX16)) {
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

// The last position array a list call was over and whether it is retained (a list call over one
// that is not is a skinned one): a model's lists follow each other over one array, and the
// retained arrays' map is large and cold. Forgotten when a list is retained or released.
const void* sLastPosArray = nullptr;
bool sLastPosResident = false;
bool sPosAnswerKnown = false;

bool position_array_resident(const void* data) noexcept {
  if (!sPosAnswerKnown || data != sLastPosArray) {
    sLastPosArray = data;
    sLastPosResident = resident::find(data) != nullptr;
    sPosAnswerKnown = true;
  }
  return sLastPosResident;
}

// A vertex's position and normal array indices (UINT32_MAX: in the list itself, or none).
struct VertexIndices {
  u32 pos = UINT32_MAX;
  u32 nrm = UINT32_MAX;
};

// A list called with positions the CPU makes each frame (skinned models): decoded once into its
// triangles with, for each vertex, the array indices its position and normal come from; each call
// copies them and reads the positions and normals again from the arrays as they are now (CPU
// skinning writes both). The faces keep the fixed-function shading of the pose the template was
// made in.
struct SkinnedTemplate {
  DecodeState state; // compared without the position array (matches_current's skipPositions)
  GXVtxFmt fmt = GX_VTXFMT0; // the one vertex format the list's primitives name
  std::vector<vita_gl::Vertex> vertices;
  std::vector<VertexIndices> indices;
};
std::unordered_map<const void*, SkinnedTemplate> sSkinned;

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
  u32 immediatePoints = 0;
  u32 immediateFailed = 0;
  u32 recorded = 0;
  u32 failMatrixIndex = 0;
  u32 failArray = 0;
  u32 failCommand = 0;
  u32 failEmpty = 0;
  // Decodes of a list already cached, by what differed from its newest variant (the layout: the
  // descriptor or a vertex format the list names).
  u32 redecodeTex = 0;
  u32 redecodeFmt = 0;
  u32 redecodeArray = 0;
  // Recorded draws by how many textures they take, and textures left out for want of a Vita copy
  // (EFB copies, dynamic textures).
  std::array<u32, kTexUnits + 1> drawsByUnits{};
  u32 unitsWithoutCopy = 0;
  u32 copyStandIns = 0; // materials given the transparent black stand-in for an EFB copy (a depth copy)
  u32 copyTextures = 0; // materials given an EFB copy's Vita texture
  u32 loneTextures = 0; // materials freed for holding a texture nothing else held (drop_lone_textures)
  u32 copies = 0;       // EFB copies recorded, and depth copies not made
  u32 depthCopies = 0;
  // Recorded draws by their state (after the constant alpha is folded in).
  u32 blended = 0;
  u32 alphaTested = 0;
  u32 culled = 0;
  u32 depthRanged = 0; // drawn with a viewport depth range other than [0, 1]
  u32 cornered = 0;    // drawn with a viewport other than the whole screen
  u32 copyClears = 0;  // EFB copies' clears recorded
  // TEV's final alpha: a constant worked out, of those the draws its alpha test rejects outright,
  // and the draws whose alpha reads a texture; the rest depend on a lit or vertex alpha.
  u32 alphaConstant = 0;
  u32 alphaRejected = 0;
  u32 alphaTexture = 0;
  // Vita copies made for textures cached before the Vita path drew with them.
  u32 lateCopies = 0;
  // Second units added on top (GL_ADD) rather than modulated.
  u32 additiveUnits = 0;
  // Draws whose material (current_material) was the last draw's, was in the cache, was worked out.
  u32 materialReused = 0;
  u32 materialHits = 0;
  u32 materialMisses = 0;
  // Skinned list calls drawn from their template, and templates made.
  u32 skinnedFromTemplate = 0;
  u32 skinnedTemplates = 0;
  // Draws recorded with a generated program (vita_tev.hpp), and their constant arrays (vertex and
  // fragment) put in the frame's rows, and pointed at the last ones as they repeated them.
  u32 withProgram = 0;
  u32 constantsRecorded = 0;
  u32 constantsShared = 0;
  // Materials worked out that sample an EFB copy (their hash moves with every copy), and those
  // that replaced another still in use this window.
  u32 materialMissesCopy = 0;
  u32 materialEvictions = 0;
  // The material key's parts worked out again (material_hash).
  u32 keyPipelines = 0;
  u32 keyTextures = 0;
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

// A normal as a vertex holds it: unit length, times 127 (zero stays zero).
void pack_normal(const float n[3], int8_t out[3]) noexcept {
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const float scale = len > 1e-8f ? 127.f / len : 0.f;
  for (int c = 0; c < 3; ++c) {
    out[c] = static_cast<int8_t>(std::lround(std::clamp(n[c] * scale, -127.f, 127.f)));
  }
}

// One GX vertex's position, normal, colour (grey without one) and the texture units'
// coordinates, from its bytes in the list (which advance past it). False when it has a per-vertex
// matrix (not drawn yet) or reads outside an array. indices, when given, gets the position's and
// the normal's array indices (UINT32_MAX for one in the list, or none).
bool decode_vertex(const u8*& vtx, const VtxFmt& fmt, const TexAttrs& texAttrs, u8 grey, vita_gl::Vertex& out,
                   VertexIndices* indices = nullptr) noexcept {
  out = {};
  out.color[0] = out.color[1] = out.color[2] = grey;
  out.color[3] = 255;
  out.raw[0] = out.raw[1] = out.raw[2] = out.raw[3] = 255;
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
    const bool wanted = attr == GX_VA_POS || attr == GX_VA_NRM || attr == GX_VA_CLR0 ||
                        std::find(texAttrs.begin(), texAttrs.end(), static_cast<u8>(i)) != texAttrs.end();
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
      if (indices != nullptr && attr == GX_VA_POS) {
        indices->pos = index;
      } else if (indices != nullptr && attr == GX_VA_NRM) {
        indices->nrm = index;
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
    } else if (attr == GX_VA_NRM) {
      // The normal is the first three components (of a normal, binormal and tangent's nine too).
      float n[3] = {};
      for (u32 c = 0; c < 3; ++c) {
        n[c] = read_component(data + c * step, attrFmt.type, attrFmt.frac, le);
      }
      pack_normal(n, out.normal);
    } else if (attr == GX_VA_CLR0) {
      read_color(data, attrFmt.type, out.color);
      std::memcpy(out.raw, out.color, sizeof(out.raw));
    } else {
      float st[2] = {};
      for (u32 c = 0; c < cnt && c < 2; ++c) {
        st[c] = read_component(data + c * step, attrFmt.type, attrFmt.frac, le);
      }
      for (int set = 0; set < kTexCoords; ++set) {
        if (texAttrs[set] == i) {
          out.uv[set][0] = st[0];
          out.uv[set][1] = st[1];
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
  // The generated programs take the shading on its own, as the light a lit channel adds to its
  // ambient colour (vita_tev.cpp), from the same face normal without the textured damping.
  const float light = len > 1e-8f ? 0.3f + 0.7f * (0.65f * std::fabs(n[2]) + 0.35f * std::fabs(n[0])) / len : 1.f;
  for (const vita_gl::Vertex* src : {&a, &b, &c}) {
    vita_gl::Vertex vertex = *src;
    for (int i = 0; i < 3; ++i) {
      vertex.color[i] = static_cast<u8>(std::min(255.f, vertex.color[i] * shade));
    }
    vertex.shade = static_cast<u8>(std::min(255.f, light * 255.f));
    out.push_back(vertex);
  }
}

// Without a colour of their own, textured faces take their textures' as they are.
u8 grey_for(const TexAttrs& texAttrs) noexcept { return texAttrs[0] != kNoTexAttr ? 255 : 200; }

// Decodes `count` vertices from `vtx` (which advances past them), false when one cannot be.
// indices, when given, gets each one's position and normal indices (decode_vertex).
bool decode_vertices(const u8*& vtx, const u8* end, GXVtxFmt fmt, u32 count, const TexAttrs& texAttrs,
                     std::vector<vita_gl::Vertex>& out, std::vector<VertexIndices>* indices = nullptr) noexcept {
  const u8 grey = grey_for(texAttrs);
  out.resize(count);
  if (indices != nullptr) {
    indices->assign(count, VertexIndices{});
  }
  for (u32 i = 0; i < count; ++i) {
    if (vtx >= end || !decode_vertex(vtx, g_gxState.vtxFmts[fmt], texAttrs, grey, out[i],
                                     indices != nullptr ? &(*indices)[i] : nullptr)) {
      return false;
    }
  }
  return vtx <= end;
}

// The vertices of a primitive of `count` as triangles, three indices apiece, appended to `order`.
// Lines and points add none.
void triangle_order(GXPrimitive type, u32 count, std::vector<u32>& order) noexcept {
  const auto add = [&order](u32 a, u32 b, u32 c) {
    order.push_back(a);
    order.push_back(b);
    order.push_back(c);
  };
  switch (type) {
  case GX_TRIANGLES:
    for (u32 i = 0; i + 2 < count; i += 3) {
      add(i, i + 1, i + 2);
    }
    break;
  case GX_TRIANGLESTRIP:
    for (u32 i = 0; i + 2 < count; ++i) {
      if ((i & 1) == 0) {
        add(i, i + 1, i + 2);
      } else {
        add(i + 1, i, i + 2);
      }
    }
    break;
  case GX_TRIANGLEFAN:
    for (u32 i = 1; i + 1 < count; ++i) {
      add(0, i, i + 1);
    }
    break;
  case GX_QUADS:
    for (u32 i = 0; i + 3 < count; i += 4) {
      add(i, i + 1, i + 2);
      add(i, i + 2, i + 3);
    }
    break;
  default:
    break; // lines and points: not drawn yet
  }
}

// One primitive's vertices as triangles, appended to `triangles`; with primPos given, outPos gets
// the indices of each vertex appended.
void triangulate(GXPrimitive type, const std::vector<vita_gl::Vertex>& prim, bool textured,
                 std::vector<vita_gl::Vertex>& triangles, const std::vector<VertexIndices>* primPos = nullptr,
                 std::vector<VertexIndices>* outPos = nullptr) noexcept {
  static std::vector<u32> order;
  order.clear();
  triangle_order(type, static_cast<u32>(prim.size()), order);
  for (size_t i = 0; i + 2 < order.size(); i += 3) {
    append_triangle(triangles, prim[order[i]], prim[order[i + 1]], prim[order[i + 2]], textured);
    if (primPos != nullptr && outPos != nullptr) {
      for (size_t k = 0; k < 3; ++k) {
        outPos->push_back((*primPos)[order[i + k]]);
      }
    }
  }
}

// The list's triangles, or none when it holds anything this path cannot draw yet; fmtMask gets
// the vertex formats it read, and posIndices (when given) each triangle vertex's position and normal
// indices.
std::vector<vita_gl::Vertex> decode_list(const std::vector<u8>& bytes, u32 size, const TexAttrs& texAttrs,
                                         u8& fmtMask, std::vector<VertexIndices>* posIndices = nullptr) noexcept {
  const bool textured = texAttrs[0] != kNoTexAttr;
  std::vector<vita_gl::Vertex> triangles;
  std::vector<vita_gl::Vertex> prim;
  std::vector<VertexIndices> primPos;
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
    if (!decode_vertices(vtx, bytes.data() + end, fmt, count, texAttrs, prim,
                         posIndices != nullptr ? &primPos : nullptr)) {
      return {};
    }
    pos = static_cast<size_t>(vtx - bytes.data());
    triangulate(type, prim, textured, triangles, posIndices != nullptr ? &primPos : nullptr, posIndices);
  }
  return triangles;
}

// The list's entry for the current state and texture coordinates, decoding it into a new or the
// oldest variant when none matches.
const Cached& lookup(const void* key, const resident::Entry& entry, u32 size, const TexAttrs& texAttrs) noexcept {
  // An unordered_map's elements stay where they are; the entry goes only with the list
  // (GX_AURORA_RESIDENT_RELEASE releases both).
  auto* known = static_cast<CachedList*>(entry.vitaList);
  if (known == nullptr) {
    known = &sCache[key];
    entry.vitaList = known;
  }
  CachedList& list = *known;
  for (size_t i = 0; i < list.count; ++i) {
    if (list.variants[i].state.matches_current(texAttrs)) {
      return list.variants[i];
    }
  }
  ZoneScopedN("vita: decode a display list");
  if (list.count != 0) {
    const DecodeState& last = list.variants[list.newest].state;
    if (last.texAttrs != texAttrs) {
      ++sStats.redecodeTex;
    } else if (last.layout != layout_hash(last.fmtMask)) {
      ++sStats.redecodeFmt; // the descriptor or a format the list names
    } else {
      ++sStats.redecodeArray;
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
  cached.mesh = triangles.empty() ? nullptr : vita_gl::make_mesh(std::move(triangles), vita_gl::MeshKind::List);
  state.capture(texAttrs);
  return cached;
}

// A TEV stage's texture as a unit of the draw, and whether the stage adds it to what came
// before (d a register, the texture's colour among a, b and c: a glow or incandescence pass, whose
// texture is black but for its lights) rather than multiplying or blending with it.
struct UnitChoice {
  GXTexMapID map;
  const TcgConfig* tcg;
  bool additive;
  GXTexCoordID coord;
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
    out[count++] = {stage.texMapId, &tcg, adds_texture(stage), stage.texCoordId};
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

// A 1x1 transparent black texture: an EFB copy's stand-in (current_material).
const std::shared_ptr<vita_gl::Texture>& copy_stand_in() noexcept {
  static const std::shared_ptr<vita_gl::Texture> sTexture =
      vita_gl::make_texture(1, 1, vita_gl::TexelFormat::RGBA8, std::vector<uint8_t>(4, 0));
  return sTexture;
}

// Each EFB copy destination's Vita texture (record_copy), filled on the render worker at every
// copy to it; the copies' order among the draws keeps each draw sampling the copy made before it.
// Destinations no longer copies (evicted, cleared) go at log_stats.
absl::flat_hash_map<const void*, std::shared_ptr<vita_gl::Texture>> sCopyTextures;

const std::shared_ptr<vita_gl::Texture>& copy_texture(const void* dest) noexcept {
  const auto it = sCopyTextures.find(dest);
  if (it == sCopyTextures.end()) {
    ++sStats.copyStandIns;
    return copy_stand_in();
  }
  ++sStats.copyTextures;
  return it->second;
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
// With a program layout (vita_tev.hpp), the mesh's coordinate sets are the layout's and the units
// take theirs from it; without, each unit has its own set, in unit order. The layout's textures are
// resolved with the units', for the program.
int resolve_units(vita_gl::TexUnit units[kTexUnits], const TcgConfig* tcgs[kTexUnits], TexAttrs& texAttrs,
                  const TevLayout& layout) noexcept {
  UnitChoice choices[kTexUnits];
  const int chosen = choose_units(choices);
  texAttrs = kNoTexAttrs;
  if (layout.supported) {
    for (int c = 0; c < layout.coordCount; ++c) {
      // A set made from the position or the normal has no coordinates of its own in the mesh.
      const TcgConfig& tcg = g_gxState.tcgs[layout.coords[c]];
      texAttrs[c] = tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7 ? static_cast<u8>(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))
                                                                  : kNoTexAttr;
    }
  }
  int unitCount = 0;
  if (chosen != 0 || (layout.supported && layout.samplerCount != 0)) {
    static ShaderInfo sSampled;
    sSampled.sampledTextures.reset();
    for (int i = 0; i < chosen; ++i) {
      sSampled.sampledTextures.set(choices[i].map);
    }
    if (layout.supported) {
      for (int m = 0; m < layout.samplerCount; ++m) {
        sSampled.sampledTextures.set(layout.maps[m]);
      }
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
      int coord = unitCount;
      if (layout.supported) {
        // The layout holds every coordinate generator a stage samples with, the units' among them.
        coord = static_cast<int>(std::find(layout.coords.begin(), layout.coords.begin() + layout.coordCount,
                                           choices[i].coord) -
                                 layout.coords.begin());
      } else {
        texAttrs[coord] = static_cast<u8>(GX_VA_TEX0 + (choices[i].tcg->src - GX_TG_TEX0));
      }
      unit.coord = static_cast<u8>(coord);
      ++unitCount;
    }
  }
  return unitCount;
}

// What the GX state decides for a draw apart from its matrices and constants: its texture units
// (textures, samplers, combine), the attributes their coordinates come from, and the
// fixed-function state. Working it out is most of a call's cost, so it is kept: for the next
// draws while no register it reads changes (the Vita's own dirty bits, DirtyBits), and in a cache
// by a hash of that state for when a material comes back.
struct Material {
  u64 hash = 0;
  bool valid = false;
  u32 window = 0;     // the stats window it was last used in (log_stats frees the idle ones)
  int unitCount = 0;  // -1: a texture without a Vita copy (resolve_units)
  bool alphaMatters = false; // its alpha test or blending reads the fragment's alpha
  TexAttrs texAttrs = kNoTexAttrs;
  vita_gl::TexUnit units[kTexUnits];
  const TcgConfig* tcgs[kTexUnits] = {};
  vita_gl::RenderState state; // before TEV's constant alpha is folded in (record_draw)
  // Its generated program (vita_tev.hpp; null: fixed-function only), with the textures it samples,
  // its coordinate generators (the mesh's sets in order) and its state (the program alpha-tests).
  const TevProgram* program = nullptr;
  std::shared_ptr<vita_gl::Texture> programTextures[kSamplers];
  vita_gl::Sampler programSamplers[kSamplers] = {};
  int coordCount = 0;
  const TcgConfig* coordTcgs[kTexCoords] = {};
  vita_gl::RenderState programState;
  u32 used = 0; // sMaterialClock at its last use, for the set's replacement
};
// Sets of four, the least recently used of a set replaced: the frigate's heavy rooms keep about a
// thousand materials alive, which in 1024 direct-mapped slots evicted each other (some 210 worked
// out again a frame).
constexpr size_t kMaterialSets = 1024;
constexpr size_t kMaterialWays = 4;
std::array<Material, kMaterialSets * kMaterialWays> sMaterials;
u32 sMaterialClock = 0;
u32 sWindow = 0;
// The material of the last draw, and the texture bind generation it was resolved under.
Material* sLastMaterial = nullptr;
u64 sLastGeneration = 0;

// A Material's key is a hash of the GX state it reads, in two parts kept between draws and worked
// out again only when their registers were written (the Vita's dirty bits, DirtyBits): hashing
// all of it, some 1.2 KiB, for each draw that wrote any took 10 us a draw.
// - The pipeline part (DirtyPipeline): the TEV stages, blending, depth, culling, the colour mask
//   and the alpha compare, the swap tables and channels, and the texture coordinate generators the
//   stages name. Every register it reads sets DirtyPipeline (regs.cpp), as the last-draw shortcut
//   in current_material already relies on.
// - The texture part (DirtyTextures, or the pipeline part's maps changed, or the bind generation
//   moved): the textures the stages name, with their palettes, and whether one is an EFB copy.
// The game leaves slots and generators no stage reads holding whatever it set last, so they are
// left out, as are the matrices and the colour constants (read for every draw). The descriptor's
// entries for the generators' sources (a descriptor write sets no dirty bit) are read for every
// draw, four bits each.
struct PipelineKey {
  u64 hash = 0;
  u32 usedMaps = 0;
  u32 texSources = 0; // the TEXn attributes the used generators read, by bit
};

PipelineKey pipeline_key() noexcept {
  const GXState& s = g_gxState;
  const AlphaCompare& alpha = s.alphaCompare;
  const u32 fixed[] = {
      s.numTevStages,  static_cast<u32>(s.blendMode),   static_cast<u32>(s.blendFacSrc),
      static_cast<u32>(s.blendFacDst),  static_cast<u32>(s.blendOp), static_cast<u32>(s.depthFunc),
      s.depthCompare,  s.depthUpdate,   s.colorUpdate,  s.alphaUpdate, static_cast<u32>(s.cullMode),
      static_cast<u32>(alpha.comp0),    alpha.ref0,     static_cast<u32>(alpha.op),
      static_cast<u32>(alpha.comp1),    alpha.ref1,     static_cast<u32>(s.fog.type),
  };
  u64 h = XXH3_64bits(fixed, sizeof(fixed));
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  h = XXH3_64bits_withSeed(s.tevStages.data(), stages * sizeof(TevStage), h);
  // What a generated program reads besides the stages: the swap tables, the channels' setup and
  // whether indirect stages are on.
  h = XXH3_64bits_withSeed(s.tevSwapTable.data(), sizeof(s.tevSwapTable), h);
  h = XXH3_64bits_withSeed(s.colorChannelConfig.data(), sizeof(s.colorChannelConfig), h);
  h = XXH3_64bits_withSeed(&s.numIndStages, sizeof(s.numIndStages), h);
  u32 usedMaps = 0;
  u32 usedCoords = 0;
  for (u32 i = 0; i < stages; ++i) {
    const TevStage& stage = s.tevStages[i];
    usedMaps |= stage.texMapId < MaxTextures ? 1u << stage.texMapId : 0;
    usedCoords |= stage.texCoordId < MaxTexCoord ? 1u << stage.texCoordId : 0;
  }
  PipelineKey key;
  key.usedMaps = usedMaps;
  TcgConfig coords[MaxTexCoord]{};
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    if ((usedCoords & (1u << i)) == 0) {
      continue;
    }
    coords[i] = s.tcgs[i];
    const GXTexGenSrc src = s.tcgs[i].src;
    if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) {
      key.texSources |= 1u << (src - GX_TG_TEX0);
    }
  }
  key.hash = XXH3_64bits_withSeed(coords, sizeof(coords), h);
  // How many lights each lit channel part sums: the program's (its lights are read for each draw).
  u32 lights[4];
  for (int part = 0; part < 4; ++part) {
    lights[part] = s.colorChannelConfig[part].lightingEnabled
                       ? static_cast<u32>(s.colorChannelState[part].lightMask.count())
                       : 0;
  }
  key.hash = XXH3_64bits_withSeed(lights, sizeof(lights), key.hash);
  return key;
}

// The texture part of the key (PipelineKey's comment), for the maps the stages name.
u64 texture_key(u32 usedMaps, bool& readsCopy) noexcept {
  const GXState& s = g_gxState;
  // A texture by what identifies its contents and sampling, and a palette texture's palette.
  u32 textures[MaxTextures][12]{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    if ((usedMaps & (1u << i)) == 0) {
      continue;
    }
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
  // Aurora's bind generation moves at every EFB copy (several a frame), and a static texture is
  // already named by its identity above; only a slot holding a copy's destination resolves to
  // something new when it moves (the key takes the generation in then: material_hash).
  readsCopy = false;
  for (u32 i = 0; i < MaxTextures; ++i) {
    if ((usedMaps & (1u << i)) != 0 && s.copyTextures.contains(s.loadedTextures[i].data)) {
      readsCopy = true;
      break;
    }
  }
  return XXH3_64bits(textures, sizeof(textures));
}

// The key's parts as the last draw that worked them out left them.
struct KeyParts {
  bool valid = false;
  PipelineKey pipeline;
  u64 textures = 0;
  bool readsCopy = false;
  u64 generation = 0;
};
KeyParts sKeyParts;

// The current material's key, its parts worked out again where their registers changed.
u64 material_hash(u8 changed, u64 generation, bool& readsCopy) noexcept {
  KeyParts& parts = sKeyParts;
  const bool pipelineChanged = !parts.valid || (changed & DirtyPipeline) != 0;
  if (pipelineChanged) {
    parts.pipeline = pipeline_key();
    ++sStats.keyPipelines;
  }
  if (pipelineChanged || (changed & DirtyTextures) != 0 || generation != parts.generation) {
    parts.textures = texture_key(parts.pipeline.usedMaps, parts.readsCopy);
    parts.generation = generation;
    ++sStats.keyTextures;
  }
  parts.valid = true;
  u32 sources = 0;
  for (u32 i = 0; i < 8; ++i) {
    if ((parts.pipeline.texSources & (1u << i)) != 0) {
      sources |= (static_cast<u32>(g_gxState.vtxDesc[GX_VA_TEX0 + i]) + 1) << (4 * i);
    }
  }
  readsCopy = parts.readsCopy;
  const u64 words[] = {parts.pipeline.hash, parts.textures, sources, readsCopy ? generation : 0};
  return XXH3_64bits(words, sizeof(words));
}

// The Material for the current GX state: the last draw's while none of the registers it reads has
// been written since, else from the cache, else worked out.
const Material& current_material() noexcept {
  const u8 changed = g_gxState.dirty.vita & (DirtyPipeline | DirtyTextures);
  g_gxState.dirty.vita = 0;
  const u64 generation = texture::current_bind_generation();
  if (sLastMaterial != nullptr && changed == 0 && generation == sLastGeneration) {
    ++sStats.materialReused;
    sLastMaterial->window = sWindow;
    sLastMaterial->used = ++sMaterialClock;
    return *sLastMaterial;
  }
  sLastGeneration = generation;
  bool readsCopy = false;
  const u64 hash = material_hash(changed, generation, readsCopy);
  // Its set: the entry holding it, else an empty one, else the least recently used.
  Material* const set = &sMaterials[(hash % kMaterialSets) * kMaterialWays];
  Material* found = nullptr;
  Material* victim = &set[0];
  for (size_t way = 0; way < kMaterialWays; ++way) {
    Material& entry = set[way];
    if (entry.valid && entry.hash == hash) {
      found = &entry;
      break;
    }
    if (victim->valid && (!entry.valid || entry.used < victim->used)) {
      victim = &entry;
    }
  }
  Material& material = found != nullptr ? *found : *victim;
  material.window = sWindow;
  material.used = ++sMaterialClock;
  sLastMaterial = &material;
  if (found != nullptr) {
    ++sStats.materialHits;
    return material;
  }
  ZoneScopedN("vita: work out a material");
  ++sStats.materialMisses;
  sStats.materialMissesCopy += readsCopy ? 1 : 0;
  sStats.materialEvictions += material.valid ? 1 : 0;
  material = {};
  material.hash = hash;
  material.valid = true;
  material.window = sWindow;
  material.used = sMaterialClock;
  const TevLayout layout = tev_enabled() ? tev_layout() : TevLayout{};
  material.unitCount = resolve_units(material.units, material.tcgs, material.texAttrs, layout);
  if (material.unitCount < 0) {
    return material;
  }
  material.state = render_state();
  const AlphaCompare& compare = g_gxState.alphaCompare;
  const auto readsAlpha = [](u16 factor) { return factor == kGlSrcAlpha || factor == kGlOneMinusSrcAlpha; };
  material.alphaMatters = compare.comp0 != GX_ALWAYS || compare.comp1 != GX_ALWAYS ||
                          (material.state.blend && (readsAlpha(material.state.blendSrc) ||
                                                    readsAlpha(material.state.blendDst)));
  if (layout.supported) {
    // Every texture the stages sample needs its Vita copy (resolve_units resolved them all).
    bool copies = true;
    for (int m = 0; m < layout.samplerCount; ++m) {
      const gfx::TextureBind& bind = g_gxState.textures[layout.maps[m]];
      if (bind.ref && !bind.ref->vitaTexture) {
        add_missing_copy(*bind.ref, g_gxState.loadedTextures[layout.maps[m]]);
      }
      if (!bind.ref || !bind.ref->vitaTexture) {
        // An EFB copy's texture (a reflection, the morph ball's shadow, the pause screen's blurred
        // backdrop) has no Vita copy of Aurora's: its own, filled from the screen (record_copy),
        // or for a depth copy transparent black, which adds nothing and fails an alpha test.
        if (g_gxState.copyTextures.contains(g_gxState.loadedTextures[layout.maps[m]].data)) {
          material.programTextures[m] = copy_texture(g_gxState.loadedTextures[layout.maps[m]].data);
          material.programSamplers[m] = sampler_for(bind.texObj);
          continue;
        }
        copies = false;
        break;
      }
      material.programTextures[m] = bind.ref->vitaTexture;
      material.programSamplers[m] = sampler_for(bind.texObj);
    }
    if (copies) {
      material.program = tev_program(layout);
      material.coordCount = layout.coordCount;
      for (int c = 0; c < layout.coordCount; ++c) {
        material.coordTcgs[c] = &g_gxState.tcgs[layout.coords[c]];
      }
      material.programState = material.state;
      material.programState.alphaFunc = kGlAlways;
      vita_gl::RenderState& state = material.programState;
      if (state.blend && state.blendEquation == kGlFuncAdd && state.blendSrc == kGlOne && state.blendDst == kGlZero) {
        state.blend = false;
      }
    } else {
      for (auto& texture : material.programTextures) {
        texture.reset();
      }
    }
  }
  return material;
}

// A rectangle of GX's logical framebuffer (the viewport's and scissor's, an EFB copy's source;
// origin top left) on the Vita's screen, as GL takes it (origin bottom left). `clamp` keeps it on
// the screen (a scissor's or a clear's; a viewport may reach past it).
vita_gl::ScreenRect screen_rect(float left, float top, float width, float height, bool clamp) noexcept {
  const auto [fbWidth, fbHeight] = logical_fb_size();
  const float sx = fbWidth != 0 ? static_cast<float>(vita_gl::kScreenWidth) / static_cast<float>(fbWidth) : 1.f;
  const float sy = fbHeight != 0 ? static_cast<float>(vita_gl::kScreenHeight) / static_cast<float>(fbHeight) : 1.f;
  long x0 = std::lround(left * sx);
  long x1 = std::lround((left + width) * sx);
  long y0 = std::lround(top * sy);
  long y1 = std::lround((top + height) * sy);
  if (clamp) {
    x0 = std::clamp<long>(x0, 0, vita_gl::kScreenWidth);
    x1 = std::clamp<long>(x1, x0, vita_gl::kScreenWidth);
    y0 = std::clamp<long>(y0, 0, vita_gl::kScreenHeight);
    y1 = std::clamp<long>(y1, y0, vita_gl::kScreenHeight);
  }
  return {static_cast<int16_t>(x0), static_cast<int16_t>(vita_gl::kScreenHeight - y1),
          static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}

// GX's line width or point size (sixths of a pixel of its framebuffer) in the screen's whole pixels,
// as GXM takes them (1 to 16).
u8 screen_width(u8 sixths) noexcept {
  const auto [fbWidth, fbHeight] = logical_fb_size();
  const float scale = fbWidth != 0 ? static_cast<float>(vita_gl::kScreenWidth) / static_cast<float>(fbWidth) : 1.f;
  return static_cast<u8>(std::clamp<long>(std::lround(static_cast<float>(sixths) / 6.f * scale), 1, 16));
}

// Records a draw of `mesh` with GX's current matrices and the material's units and state, TEV's
// alpha folded in where it is a constant (its inputs are constants that change between draws). A
// line or point mesh is drawn `width` pixels wide.
void record_draw(std::shared_ptr<vita_gl::Mesh> mesh, const Material& material, u8 width = 1) noexcept {
  // For the FIFO thread's timing: since the triangles, the mesh made of them (the caller's
  // argument); from here to the return, the draw's recording.
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kMesh);
  struct Recorded {
    ~Recorded() { vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kRecord); }
  } recorded;
  vita_gl::RenderState state = material.state;
  if (material.alphaMatters) {
    const TevAlpha alpha = tev_alpha();
    if (alpha.known) {
      ++sStats.alphaConstant;
      if (!fold_constant_alpha(alpha.value, state)) {
        ++sStats.alphaRejected;
        return; // its alpha test fails everywhere: nothing to draw
      }
    } else if (alpha.texture) {
      ++sStats.alphaTexture;
    }
  }
  if (state.blend && state.blendEquation == kGlFuncAdd && state.blendSrc == kGlOne && state.blendDst == kGlZero) {
    state.blend = false;
  }
  vita_gl::Draw draw{.mesh = std::move(mesh)};
  draw.width = width;
  const Mat3x4<float>& mv = g_gxState.pnMtx[g_gxState.currentPnMtx].pos;
  std::memcpy(draw.modelView, &mv, sizeof(draw.modelView));
  // GX's clip depth runs from -w (near) to 0; GL's from -w to w: row 2 becomes 2 * row 2 + row 3.
  // Then the viewport's depth range (GXSetViewport's near and far, the window depth at the near
  // and far planes), folded in as glDepthRange would apply it: depth n + (f - n) * d for the
  // [0, 1] depth d, so clip z becomes (f - n) * z + (f + n - 1) * w. The game layers with it: the
  // HUD into the nearest depths, the sky and the planet into the farthest.
  const Mat4x4<float>& proj = g_gxState.proj;
  const float n = g_gxState.renderViewport.znear;
  const float f = g_gxState.renderViewport.zfar;
  for (int c = 0; c < 4; ++c) {
    draw.projection[c] = proj.m0[c];
    draw.projection[4 + c] = proj.m1[c];
    draw.projection[8 + c] = (f - n) * (2.f * proj.m2[c] + proj.m3[c]) + (f + n - 1.f) * proj.m3[c];
    draw.projection[12 + c] = proj.m3[c];
  }
  sStats.depthRanged += n != 0.f || f != 1.f ? 1 : 0;
  // GX's viewport and scissor: the game draws its reflection and shadows into a corner, then
  // copies it into a texture and clears it (record_copy_clear).
  const gfx::Viewport& vp = g_gxState.logicalViewport;
  draw.viewport = screen_rect(vp.left, vp.top, vp.width, vp.height, false);
  const gfx::ClipRect& sc = g_gxState.logicalScissor;
  draw.scissor = screen_rect(static_cast<float>(sc.x), static_cast<float>(sc.y), static_cast<float>(sc.width),
                             static_cast<float>(sc.height), true);
  sStats.cornered += draw.viewport == vita_gl::ScreenRect{} ? 0 : 1;
  for (int unit = 0; unit < material.unitCount; ++unit) {
    draw.units[unit] = material.units[unit];
    // Texture matrices animate (scrolling, pulsing): read for every draw.
    tex_matrix(*material.tcgs[unit], draw.units[unit].matrix);
    sStats.additiveUnits += draw.units[unit].envMode == kGlAdd ? 1 : 0;
  }
  draw.state = state;
  if (material.program != nullptr) {
    vita_gl::ProgramInputs& inputs = draw.program;
    inputs.program = material.program->program;
    for (int m = 0; m < kSamplers; ++m) {
      inputs.textures[m] = material.programTextures[m];
      inputs.samplers[m] = material.programSamplers[m];
    }
    // The projection times the position matrix (a 3x4 with (0, 0, 0, 1) below), here on the FIFO
    // thread: the render worker is the busier one.
    float vertex[vita_gl::kProgramVertexConstants][4];
    for (int r = 0; r < 4; ++r) {
      const float* p = &draw.projection[4 * r];
      for (int c = 0; c < 4; ++c) {
        vertex[r][c] = p[0] * draw.modelView[c] + p[1] * draw.modelView[4 + c] + p[2] * draw.modelView[8 + c] +
                       (c == 3 ? p[3] : 0.f);
      }
    }
    const TevProgram& program = *material.program;
    const int row = tev_vertex_rows(program, material.coordTcgs, material.coordCount, vertex);
    float constants[vita_gl::kProgramConstants][4];
    tev_constants(program, constants);
    // Never more than the program declares (the generators' types made both counts).
    inputs.vertexCount = static_cast<u8>(std::min(row, program.vertexCount));
    inputs.constantCount = static_cast<u8>(program.constantCount);
    // Into the frame's rows, unless they repeat this program's last ones there: the draw then points
    // at those, and the render worker sends nothing.
    const u32 serial = vita_gl::recording_serial();
    const bool earlier = program.recordedIn == serial;
    program.recordedIn = serial;
    const auto place = [earlier](const float (*rows)[4], int count, u32& at) {
      if (count == 0) {
        return;
      }
      if (earlier && std::memcmp(vita_gl::recorded_constants(at), rows, sizeof(float[4]) * count) == 0) {
        ++sStats.constantsShared;
        return;
      }
      at = vita_gl::record_constants(rows, count);
      ++sStats.constantsRecorded;
    };
    place(vertex, inputs.vertexCount, program.vertexAt);
    place(constants, inputs.constantCount, program.constantsAt);
    inputs.vertexAt = program.vertexAt;
    inputs.constantsAt = program.constantsAt;
    inputs.state = material.programState;
    ++sStats.withProgram;
  }
  sStats.blended += draw.state.blend ? 1 : 0;
  sStats.alphaTested += draw.state.alphaFunc != kGlAlways ? 1 : 0;
  sStats.culled += draw.state.cullFace != 0 ? 1 : 0;
  vita_gl::record(std::move(draw));
  ++sStats.recorded;
  ++sStats.drawsByUnits[material.unitCount];
}

// A skinned list call's triangles from its template (SkinnedTemplate), made at its first call
// under this state. False when the list cannot have one (positions not indexed, or more than one
// vertex format) or a position index falls outside the array as it is now: it is then decoded
// whole.
bool skinned_from_template(const void* key, const resident::Entry& entry, u32 size, const TexAttrs& texAttrs,
                           std::vector<vita_gl::Vertex>& out) noexcept {
  const GXAttrType posType = g_gxState.vtxDesc[GX_VA_POS];
  if (posType != GX_INDEX8 && posType != GX_INDEX16) {
    return false;
  }
  SkinnedTemplate& t = sSkinned[key];
  const bool built = !t.vertices.empty() || t.state.fmtMask != 0;
  if (!built || !t.state.matches_current(texAttrs, true)) {
    ++sStats.skinnedTemplates;
    DecodeState& state = t.state;
    state.fmtMask = 0;
    sLastFail = Fail::None;
    t.indices.clear();
    t.vertices = decode_list(*entry.bytes, size, texAttrs, state.fmtMask, &t.indices);
    state.capture(texAttrs);
    const bool usable =
        !t.vertices.empty() && std::has_single_bit(state.fmtMask) && t.indices.size() == t.vertices.size() &&
        std::none_of(t.indices.begin(), t.indices.end(), [](const VertexIndices& v) { return v.pos == UINT32_MAX; });
    if (!usable) {
      // Kept as not usable (fmtMask set, no vertices), so the next calls decode whole at once.
      state.fmtMask = state.fmtMask != 0 ? state.fmtMask : 1;
      state.layout = layout_hash(state.fmtMask);
      t.vertices.clear();
      t.indices.clear();
      return false;
    }
    t.fmt = static_cast<GXVtxFmt>(std::countr_zero(state.fmtMask));
  }
  if (t.vertices.empty()) {
    return false;
  }
  const VtxAttrFmt& posFmt = g_gxState.vtxFmts[t.fmt].attrs[GX_VA_POS];
  const AttrArray& array = g_gxState.arrays[GX_VA_POS];
  const u32 cnt = std::min<u32>(comp_cnt_count(GX_VA_POS, posFmt.cnt), 3);
  const u32 step = comp_type_size(GX_VA_POS, posFmt.type);
  if (array.data == nullptr) {
    return false;
  }
  const auto* base = static_cast<const u8*>(array.data);
  // The normals too, where the descriptor indexes them (a direct one stays the template's).
  const GXAttrType nrmType = g_gxState.vtxDesc[GX_VA_NRM];
  const VtxAttrFmt& nrmFmt = g_gxState.vtxFmts[t.fmt].attrs[GX_VA_NRM];
  const AttrArray& nrmArray = g_gxState.arrays[GX_VA_NRM];
  const bool normals = (nrmType == GX_INDEX8 || nrmType == GX_INDEX16) && nrmArray.data != nullptr;
  const u32 nrmStep = comp_type_size(GX_VA_NRM, nrmFmt.type);
  const auto* nrmBase = static_cast<const u8*>(nrmArray.data);
  out = t.vertices;
  for (size_t i = 0; i < out.size(); ++i) {
    const u64 offset = u64(t.indices[i].pos + array.baseIndex) * array.stride;
    if (offset + cnt * step > array.size) {
      return false;
    }
    for (u32 c = 0; c < cnt; ++c) {
      out[i].pos[c] = read_component(base + offset + c * step, posFmt.type, posFmt.frac, array.le);
    }
    if (normals && t.indices[i].nrm != UINT32_MAX) {
      const u64 nrmOffset = u64(t.indices[i].nrm + nrmArray.baseIndex) * nrmArray.stride;
      if (nrmOffset + 3 * nrmStep <= nrmArray.size) {
        float n[3];
        for (u32 c = 0; c < 3; ++c) {
          n[c] = read_component(nrmBase + nrmOffset + c * nrmStep, nrmFmt.type, nrmFmt.frac, nrmArray.le);
        }
        pack_normal(n, out[i].normal);
      }
    }
  }
  return true;
}
} // namespace

bool draw_resident(const void* key, const resident::Entry& entry, u32 size) noexcept {
  ZoneScoped;
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kDispatch);
  ++sStats.calls;
  const Material& material = current_material();
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kMaterial);
  if (material.unitCount < 0) {
    return false;
  }
  const TexAttrs& texAttrs = material.texAttrs;
  // Positions made at run time (CPU skinning) change under the same list, so such a call is
  // decoded each time, into a mesh for this frame alone. The retail models' own arrays are
  // retained (port_retail_resident), whatever their byte order; a workspace the game rewrites
  // each frame is not.
  if (!position_array_resident(g_gxState.arrays[GX_VA_POS].data)) {
    ZoneScopedN("vita: skinned list call");
    std::vector<vita_gl::Vertex> triangles;
    if (skinned_from_template(key, entry, size, texAttrs, triangles)) {
      ++sStats.skinnedFromTemplate;
    } else {
      u8 fmtMask = 0;
      sLastFail = Fail::None;
      triangles = decode_list(*entry.bytes, size, texAttrs, fmtMask);
      ++sStats.perCall;
    }
    vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kSkinned);
    if (triangles.empty()) {
      ++sStats.perCallFailed;
      return false;
    }
    record_draw(vita_gl::make_mesh(std::move(triangles), vita_gl::MeshKind::Skinned, material.program != nullptr),
                material);
    return true;
  }
  const Cached& cached = lookup(key, entry, size, texAttrs);
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kLookup);
  if (!cached.mesh) {
    return false;
  }
  record_draw(cached.mesh, material);
  return true;
}

bool draw_immediate(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, std::span<const u8> vertices) noexcept {
  ZoneScoped;
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kDispatch);
  const Material& material = current_material();
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kMaterial);
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
  if (prim == GX_POINTS) {
    // As GL's points, GX's point size wide, drawn fixed-function: vitaGL's point size reaches only
    // its own programs (a generated one would have to write the size). The game draws none.
    vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kImmediate);
    if (prims.empty()) {
      ++sStats.immediateFailed;
      return false;
    }
    ++sStats.immediatePoints;
    Material fixedFunction = material;
    fixedFunction.program = nullptr;
    record_draw(vita_gl::make_mesh(std::move(prims), vita_gl::MeshKind::Immediate, false,
                                   vita_gl::MeshPrimitive::Points),
                fixedFunction, screen_width(g_gxState.pointSize));
    return true;
  }
  if (prim == GX_LINES || prim == GX_LINESTRIP) {
    // As GL's lines, two vertices each (the map's outlines, among others), GX's line width wide in
    // whole pixels. GX's line texture offsets are not applied.
    std::vector<vita_gl::Vertex> lines;
    const size_t step = prim == GX_LINES ? 2 : 1;
    for (size_t i = 0; i + 1 < prims.size(); i += step) {
      lines.push_back(prims[i]);
      lines.push_back(prims[i + 1]);
    }
    vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kImmediate);
    if (lines.empty()) {
      ++sStats.immediateFailed;
      return false;
    }
    ++sStats.immediateLines;
    record_draw(vita_gl::make_mesh(std::move(lines), vita_gl::MeshKind::Immediate, material.program != nullptr,
                                   vita_gl::MeshPrimitive::Lines),
                material, screen_width(g_gxState.lineWidth));
    return true;
  }
  std::vector<vita_gl::Vertex> triangles;
  triangulate(prim, prims, texAttrs[0] != kNoTexAttr, triangles);
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kImmediate);
  if (triangles.empty()) {
    ++sStats.immediateFailed;
    return false;
  }
  ++sStats.immediate;
  record_draw(vita_gl::make_mesh(std::move(triangles), vita_gl::MeshKind::Immediate, material.program != nullptr),
              material);
  return true;
}

bool draw_indexed(GXVtxFmt fmt, u16 vtxCount, std::span<const u8> vertices, std::span<const u8> indices) noexcept {
  ZoneScoped;
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kDispatch);
  const Material& material = current_material();
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kMaterial);
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
  vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kImmediate);
  if (triangles.empty()) {
    return false;
  }
  ++sStats.immediate;
  record_draw(vita_gl::make_mesh(std::move(triangles), vita_gl::MeshKind::Indexed, material.program != nullptr),
              material);
  return true;
}

void retained() noexcept { sPosAnswerKnown = false; }

// EFB copies read back from the screen: off till milestone 5. Each took 7 to 22 ms (glReadPixels
// ends vitaGL's scene and waits for the GPU), about one a frame in play, and the pause screen's
// backdrop came out flat grey; materials sampling a copy take the black stand-in meanwhile.
constexpr bool kScreenCopies = false;

void record_copy() noexcept {
  ++sStats.copies;
  const GXTexFmt fmt = g_gxState.texCopyFmt;
  if (!kScreenCopies) {
    return;
  }
  if ((fmt & _GX_TF_ZTF) != 0) {
    ++sStats.depthCopies;
    return;
  }
  // The EFB's alpha is GX's only with an alpha pixel format (the game's has none: opaque).
  const bool alpha = g_gxState.pixelFmt == GX_PF_RGBA6_Z24;
  vita_gl::CopyKind kind;
  switch (fmt) {
  case GX_TF_I4:
  case GX_TF_I8:
    kind = vita_gl::CopyKind::Intensity;
    break;
  case GX_TF_IA4:
  case GX_TF_IA8:
    kind = alpha ? vita_gl::CopyKind::IntensityAlpha : vita_gl::CopyKind::Intensity;
    break;
  case GX_CTF_R4:
  case GX_CTF_R8:
    kind = vita_gl::CopyKind::Red;
    break;
  case GX_CTF_RA4:
  case GX_CTF_RA8:
    kind = alpha ? vita_gl::CopyKind::RedAlpha : vita_gl::CopyKind::Red;
    break;
  case GX_CTF_G8:
    kind = vita_gl::CopyKind::Green;
    break;
  case GX_CTF_B8:
    kind = vita_gl::CopyKind::Blue;
    break;
  case GX_CTF_A8:
    kind = vita_gl::CopyKind::Alpha;
    break;
  case GX_TF_RGB5A3:
  case GX_TF_RGBA8:
  case GX_CTF_YUVA8:
    kind = alpha ? vita_gl::CopyKind::Rgba : vita_gl::CopyKind::Rgb;
    break;
  default:
    kind = vita_gl::CopyKind::Rgb;
    break;
  }
  std::shared_ptr<vita_gl::Texture>& texture = sCopyTextures[g_gxState.texCopyDest];
  if (texture == nullptr) {
    texture = vita_gl::make_copy_texture();
  }
  const gfx::ClipRect& src = g_gxState.texCopySrc;
  vita_gl::Draw draw;
  draw.copyTo = texture;
  draw.copyKind = kind;
  // Halved where the destination is (GX's box filter: the blur's half-size copy); else the
  // screen's pixels, sampled at the same coordinates whatever its size.
  draw.copyHalf = static_cast<s64>(g_gxState.texCopyDstWidth) * 3 < static_cast<s64>(src.width) * 2;
  draw.scissor = screen_rect(static_cast<float>(src.x), static_cast<float>(src.y), static_cast<float>(src.width),
                             static_cast<float>(src.height), true);
  vita_gl::record(std::move(draw));
}

void record_copy_clear() noexcept {
  const gfx::ClipRect& src = g_gxState.texCopySrc;
  vita_gl::Draw draw;
  draw.clear = true;
  draw.scissor = screen_rect(static_cast<float>(src.x), static_cast<float>(src.y), static_cast<float>(src.width),
                             static_cast<float>(src.height), true);
  draw.clearColor = g_gxState.colorUpdate;
  draw.clearAlpha = g_gxState.alphaUpdate;
  draw.clearDepth = g_gxState.depthUpdate;
  for (int c = 0; c < 4; ++c) {
    draw.clearRgba[c] = g_gxState.clearColor[c];
  }
  draw.clearZ = static_cast<float>(g_gxState.clearDepth) / 16777215.f;
  vita_gl::record(std::move(draw));
  ++sStats.copyClears;
}

void release(const void* key) noexcept {
  sPosAnswerKnown = false;
  sCache.erase(key);
  sSkinned.erase(key);
}

// A material keeps its textures alive, and one unused lasts till its window ends (log_stats) or its
// set replaces it: on a screen making a new palette texture each frame (the death screen) the
// materials kept some 20 s of them, past the GPU's memory and then the heap's. A material holding a
// texture that Aurora's caches let go and no frame in flight draws (only this material holds it)
// goes now. Only this thread hands out a material's textures, so a count of the materials' own
// references is final. Materials share textures (one picture drawn with several blends or fades),
// each counting against the others: the count is over all of them, or the continue screen's new
// palette textures, each in several materials, were never let go (some 160 of them, 90 MiB).
void drop_lone_textures() noexcept {
  absl::flat_hash_map<const vita_gl::Texture*, long> held;
  const auto each_texture = [](const Material& material, const auto& visit) {
    for (const vita_gl::TexUnit& unit : material.units) {
      if (unit.texture != nullptr) {
        visit(unit.texture);
      }
    }
    for (const auto& texture : material.programTextures) {
      if (texture != nullptr) {
        visit(texture);
      }
    }
  };
  for (const Material& material : sMaterials) {
    if (material.valid) {
      each_texture(material, [&](const std::shared_ptr<vita_gl::Texture>& texture) { ++held[texture.get()]; });
    }
  }
  for (Material& material : sMaterials) {
    if (!material.valid) {
      continue;
    }
    bool lone = false;
    each_texture(material, [&](const std::shared_ptr<vita_gl::Texture>& texture) {
      lone |= texture.use_count() <= held[texture.get()];
    });
    if (lone) {
      if (sLastMaterial == &material) {
        sLastMaterial = nullptr;
      }
      // Its references leave the count with it, so it stays exact for the materials after it.
      each_texture(material, [&](const std::shared_ptr<vita_gl::Texture>& texture) { --held[texture.get()]; });
      material = {};
      ++sStats.loneTextures;
    }
  }
}

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
  sLastMaterial = nullptr;
  // The textures the materials hold, distinct, and of them those nothing but materials holds.
  absl::flat_hash_map<const vita_gl::Texture*, std::pair<long, uint64_t>> heldTextures;
  for (const Material& material : sMaterials) {
    if (!material.valid) {
      continue;
    }
    const auto note = [&heldTextures](const std::shared_ptr<vita_gl::Texture>& texture) {
      if (texture != nullptr) {
        auto& [count, useCount] = heldTextures[texture.get()];
        ++count;
        useCount = static_cast<uint64_t>(texture.use_count());
      }
    };
    for (const vita_gl::TexUnit& unit : material.units) {
      note(unit.texture);
    }
    for (const auto& texture : material.programTextures) {
      note(texture);
    }
  }
  uint64_t heldBytes = 0;
  uint64_t onlyHeldBytes = 0;
  u32 onlyHeld = 0;
  for (const auto& [texture, counts] : heldTextures) {
    const uint64_t bytes = vita_gl::texture_bytes(*texture);
    heldBytes += bytes;
    if (counts.second <= static_cast<uint64_t>(counts.first)) {
      ++onlyHeld;
      onlyHeldBytes += bytes;
    }
  }
  // Copy destinations Aurora no longer has as copies (a texture loaded there since) go; a material
  // still holding one keeps its texture till the material goes.
  absl::erase_if(sCopyTextures, [](const auto& entry) { return !g_gxState.copyTextures.contains(entry.first); });
  Log.info("materials: {} draws kept the last draw's, {} found theirs cached, {} worked one out ({} sampling an EFB "
           "copy, {} replacing another, {} freed holding a texture nothing else held); key parts worked out: "
           "pipeline {}, textures {}; {} cached of {} ({} KiB), holding {} textures ({:.1f} MiB; held by "
           "materials alone {}, {:.1f} MiB); "
           "skinned calls: {} from their template, {} decoded whole, {} templates made; {} draws with a generated "
           "program ({} generated), their constant arrays {} recorded, {} repeating the last",
           s.materialReused, s.materialHits, s.materialMisses, s.materialMissesCopy, s.materialEvictions, s.loneTextures,
           s.keyPipelines, s.keyTextures, materials,
           sMaterials.size(), sizeof(sMaterials) / 1024, heldTextures.size(), heldBytes / 1048576.0, onlyHeld,
           onlyHeldBytes / 1048576.0, s.skinnedFromTemplate, s.perCall, s.skinnedTemplates, s.withProgram,
           tev_program_count(), s.constantsRecorded, s.constantsShared);
  Log.info("immediate draws: {} drawn, {} lines, {} points, {} not decodable; list calls with positions made "
           "each frame: {} decoded and drawn, {} not decodable",
           s.immediate, s.immediateLines, s.immediatePoints, s.immediateFailed, s.perCall, s.perCallFailed);
  Log.info("lists: {} calls, {} recorded; decoded {}, not drawable {} "
           "(matrix index {}, array {}, command {}, empty {}); decoded again for changed textures {}, vertex layout "
           "{}, array {}; {} cached",
           s.calls, s.recorded, s.decoded,
           s.failMatrixIndex + s.failArray + s.failCommand + s.failEmpty, s.failMatrixIndex, s.failArray,
           s.failCommand, s.failEmpty, s.redecodeTex, s.redecodeFmt, s.redecodeArray, sCache.size());
  Log.info("textures: draws with none {}, one {}, two {}; {} draws left out for a texture without a Vita copy, {} "
           "materials with an EFB copy's texture ({} with black: a depth copy, or screen copies off), {} copies made "
           "late; "
           "{} second units added; state: {} blended, {} alpha-tested, {} culled, {} with a depth range, {} with a "
           "smaller viewport; EFB copies: {} ({} depth copies not made, {} destinations), {} clears; TEV alpha: {} "
           "constant ({} rejected), {} from a texture",
           s.drawsByUnits[0], s.drawsByUnits[1], s.drawsByUnits[2], s.unitsWithoutCopy, s.copyTextures,
           s.copyStandIns, s.lateCopies, s.additiveUnits, s.blended, s.alphaTested, s.culled, s.depthRanged,
           s.cornered, s.copies, s.depthCopies, sCopyTextures.size(), s.copyClears, s.alphaConstant,
           s.alphaRejected, s.alphaTexture);
  sStats = {};
}
} // namespace aurora::gx::vita
