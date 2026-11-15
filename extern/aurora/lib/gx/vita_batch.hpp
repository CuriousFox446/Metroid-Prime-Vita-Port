#pragma once

// 0.42.0 test 25: the geometry of a batch (GXPortDrawBatch, GXExtra.h) as the PS Vita's draw path
// draws it (vita_draw.cpp, draw_batch). It is kept apart from the Vita's code so that the PC can
// check it (~/scratch/batchtest).
//
// Triangles are the primitives' own, made as Aurora makes them of GX's (vita_draw.cpp's
// triangle_order: a strip's odd triangles turned, a fan's about its first vertex, a quad as two).
// A run of primitives from one source in one colour shares its vertices.
//
// A line is a quad one pixel of the frame wide: half a pixel to each side across its major axis
// (up and down for a line running more across the frame than up it, else left and right). That is
// the quad GL draws a one-pixel line as, and GXM draws its lines so (the Vita draws every line a
// pixel wide: line_width). The pixels are the same except at a line's ends: they follow a quad's
// edge rule, not GL's rule for a line's last pixel. Where a strip turns from one major axis to the
// other, the two quads overlap or leave a gap of a pixel at most. Where it keeps its axis, the
// corner is shared, so the quads meet edge to edge, as the pixels of GL's line strips do.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aurora::gx::batch {

// GX's primitives (GXEnum.h), named here for the PC's check too.
constexpr uint8_t kQuads = 0x80;
constexpr uint8_t kTriangles = 0x90;
constexpr uint8_t kTriangleStrip = 0x98;
constexpr uint8_t kTriangleFan = 0xA0;
constexpr uint8_t kLines = 0xA8;
constexpr uint8_t kLineStrip = 0xB0;

// The most vertices one draw takes (16-bit indices).
constexpr uint32_t kChunkVertices = 65535;

// A source as GXPortBatchSource lays it out: its positions (x, y, z) and the matrix (rows) that
// takes them to the position matrix's space.
struct Source {
  const float* positions;
  uint32_t count;
  float mtx[3][4];
};

// A primitive's record as GXPortBatchPrim lays it out; its 8-bit indices follow it.
struct Prim {
  uint8_t primitive;
  uint8_t source;
  uint16_t count;
  uint8_t color[4];
};
static_assert(sizeof(Prim) == 8);

// The view a batch is drawn in: GX's position matrix and projection (rows), and the viewport's
// size in the frame's pixels.
struct View {
  float mv[3][4];
  float proj[4][4];
  float width;
  float height;
};

// What the lines' corners take from the view, worked out once a batch.
struct LinePlan {
  bool ok = false; // false: no step moves a line across (a degenerate view); no lines are drawn
  float mvp[4][4]{}; // the projection times the position matrix
  // A step in the position matrix's space that moves the clip position's x (ux) or y (uy) by one,
  // keeping its w (a perspective view's: the depth stays as well) or its z (an orthographic view's).
  float ux[3]{};
  float uy[3]{};
  float halfX = 0.f; // half a pixel, in NDC
  float halfY = 0.f;
  float pxX = 0.f; // pixels to an NDC unit
  float pxY = 0.f;
};

inline LinePlan plan_lines(const View& v) noexcept {
  LinePlan plan;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      plan.mvp[r][c] = v.proj[r][0] * v.mv[0][c] + v.proj[r][1] * v.mv[1][c] + v.proj[r][2] * v.mv[2][c] +
                       (c == 3 ? v.proj[r][3] : 0.f);
    }
  }
  if (!(v.width > 0.f) || !(v.height > 0.f)) {
    return plan;
  }
  const auto size = [](const float* row) {
    return std::max({std::fabs(row[0]), std::fabs(row[1]), std::fabs(row[2])});
  };
  const float across = std::max(size(plan.mvp[0]), size(plan.mvp[1]));
  // A perspective view's w moves with the position. Keeping it keeps the depth too, since GX's z is
  // a function of it. An orthographic view's w does not move, so its z is kept instead.
  const float* kept = size(plan.mvp[3]) > 1e-6f * across ? plan.mvp[3] : plan.mvp[2];
  const double m[3][3] = {
      {plan.mvp[0][0], plan.mvp[0][1], plan.mvp[0][2]},
      {plan.mvp[1][0], plan.mvp[1][1], plan.mvp[1][2]},
      {kept[0], kept[1], kept[2]},
  };
  // The inverse's first two columns, by cofactors.
  const double c0[3] = {m[1][1] * m[2][2] - m[1][2] * m[2][1], m[1][2] * m[2][0] - m[1][0] * m[2][2],
                        m[1][0] * m[2][1] - m[1][1] * m[2][0]};
  const double c1[3] = {m[0][2] * m[2][1] - m[0][1] * m[2][2], m[0][0] * m[2][2] - m[0][2] * m[2][0],
                        m[0][1] * m[2][0] - m[0][0] * m[2][1]};
  const double det = m[0][0] * c0[0] + m[0][1] * c0[1] + m[0][2] * c0[2];
  const double scale = static_cast<double>(size(plan.mvp[0])) * size(plan.mvp[1]) * size(kept);
  if (!(std::fabs(det) > 1e-9 * scale)) {
    return plan;
  }
  for (int i = 0; i < 3; ++i) {
    plan.ux[i] = static_cast<float>(c0[i] / det);
    plan.uy[i] = static_cast<float>(c1[i] / det);
  }
  plan.halfX = 1.f / v.width;
  plan.halfY = 1.f / v.height;
  plan.pxX = 0.5f * v.width;
  plan.pxY = 0.5f * v.height;
  plan.ok = true;
  return plan;
}

// A position's clip x, y and w.
struct Clip {
  float x;
  float y;
  float w;
};

inline Clip clip_of(const LinePlan& p, const float v[3]) noexcept {
  return {p.mvp[0][0] * v[0] + p.mvp[0][1] * v[1] + p.mvp[0][2] * v[2] + p.mvp[0][3],
          p.mvp[1][0] * v[0] + p.mvp[1][1] * v[1] + p.mvp[1][2] * v[2] + p.mvp[1][3],
          p.mvp[3][0] * v[0] + p.mvp[3][1] * v[1] + p.mvp[3][2] * v[2] + p.mvp[3][3]};
}

// Whether a segment runs at least as far across the frame as up it (GL's x-major), by its ends'
// clip positions. One with an end behind the eye counts as across: the GPU clips its quad.
inline bool x_major(const LinePlan& p, const Clip& a, const Clip& b) noexcept {
  if (!(a.w > 0.f) || !(b.w > 0.f)) {
    return true;
  }
  const float dx = (b.x / b.w - a.x / a.w) * p.pxX;
  const float dy = (b.y / b.w - a.y / a.w) * p.pxY;
  return std::fabs(dx) >= std::fabs(dy);
}

// A source's position `index` in the position matrix's space.
inline void transform(const Source& s, uint32_t index, float out[3]) noexcept {
  const float* v = s.positions + 3 * static_cast<size_t>(index);
  for (int r = 0; r < 3; ++r) {
    out[r] = s.mtx[r][0] * v[0] + s.mtx[r][1] * v[1] + s.mtx[r][2] * v[2] + s.mtx[r][3];
  }
}

// What an expansion made, for the log.
struct Counts {
  uint32_t prims = 0;
  uint32_t triangles = 0;
  uint32_t segments = 0;
  uint32_t vertices = 0;
  uint32_t chunks = 0;
  // Primitives left out: an index past its source, a source past the batch's, a kind no batch
  // draws (points), more vertices than a draw takes, or a record running past the batch.
  uint32_t skipped = 0;
};

// What an expansion works in, kept for its room (one thread expands at a time).
struct Scratch {
  std::vector<uint16_t> indices;
  uint32_t stamp[256]{}; // a fill run's vertex of a source index: its serial and the vertex
  uint16_t slot[256]{};
  uint32_t serial = 0;
};

// Expands a batch (sources, and `size` bytes of primitive records) into chunks of vertices and
// triangle indices (16-bit, into the chunk's vertices), the primitives in their order, no chunk over
// kChunkVertices vertices. `fills` false leaves the triangles out (GX culls them all); `twoSided`
// gives each line's quad both windings (the culling keeps one), else one. `out` takes them:
//   uint32_t room(uint32_t n): room for n more of the chunk's vertices; the chunk's index of the first
//   void give_back(uint32_t n): the last n of that room unused
//   void put(uint32_t i, const float pos[3], const uint8_t rgba[4]): the chunk's vertex i
//   void chunk(std::vector<uint16_t>& indices, uint32_t vertices): the chunk done, its indices as
//     they are (none padded)
template <class Out>
void expand(const Source* sources, uint32_t sourceCount, const uint8_t* bytes, size_t size, const LinePlan& plan,
            bool fills, bool twoSided, Out& out, Scratch& scratch, Counts& counts) {
  std::vector<uint16_t>& indices = scratch.indices;
  indices.clear();
  uint32_t chunk = 0; // the chunk's vertices so far
  const auto flush = [&] {
    if (chunk != 0 || !indices.empty()) {
      out.chunk(indices, chunk);
      ++counts.chunks;
    }
    indices.clear();
    chunk = 0;
    ++scratch.serial; // no fill run's vertex is in the next chunk
  };
  // The fill run whose vertices the next fill shares: its source and colour.
  int runSource = -1;
  uint8_t runColor[4]{};
  size_t at = 0;
  while (at < size) {
    if (size - at < sizeof(Prim)) {
      ++counts.skipped;
      break;
    }
    Prim prim;
    std::memcpy(&prim, bytes + at, sizeof(prim));
    at += sizeof(prim);
    if (prim.count > size - at) {
      ++counts.skipped;
      break;
    }
    const uint8_t* idx = bytes + at;
    at += prim.count;
    ++counts.prims;
    if (prim.source >= sourceCount) {
      ++counts.skipped;
      continue;
    }
    const Source& src = sources[prim.source];
    // Every index inside its source, as GX's draws reading past their arrays are dropped.
    const uint32_t n = prim.count;
    bool inside = src.positions != nullptr;
    for (uint32_t k = 0; k < n && inside; ++k) {
      inside = idx[k] < src.count;
    }
    if (!inside) {
      ++counts.skipped;
      continue;
    }
    const uint8_t kind = prim.primitive;
    if (kind == kLines || kind == kLineStrip) {
      const uint32_t segments = kind == kLineStrip ? (n >= 2 ? n - 1 : 0) : n / 2;
      if (segments == 0 || !plan.ok) {
        continue;
      }
      const uint32_t worst = 4 * segments;
      if (worst > kChunkVertices) {
        ++counts.skipped;
        continue;
      }
      if (chunk + worst > kChunkVertices) {
        flush();
      }
      const uint32_t first = out.room(worst);
      uint32_t used = 0;
      const auto corner = [&](const float p[3], float s, const float* u) {
        const float q[3] = {p[0] + s * u[0], p[1] + s * u[1], p[2] + s * u[2]};
        out.put(first + used, q, prim.color);
        return static_cast<uint16_t>(first + used++);
      };
      // a and b at the first end (+ and -), c and d at the second (- and +).
      const auto quad = [&](uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
        indices.insert(indices.end(), {a, b, c, a, c, d});
        if (twoSided) {
          indices.insert(indices.end(), {a, c, b, a, d, c});
        }
        ++counts.segments;
      };
      const auto step = [&plan](bool across, const Clip& c, const float*& u) {
        u = across ? plan.uy : plan.ux;
        return std::fabs(c.w) * (across ? plan.halfY : plan.halfX);
      };
      if (kind == kLineStrip) {
        float p0[3];
        transform(src, idx[0], p0);
        Clip c0 = clip_of(plan, p0);
        int lastAxis = -1;
        uint16_t lastMinus = 0;
        uint16_t lastPlus = 0;
        for (uint32_t k = 0; k + 1 < n; ++k) {
          float p1[3];
          transform(src, idx[k + 1], p1);
          const Clip c1 = clip_of(plan, p1);
          const bool across = x_major(plan, c0, c1);
          const float* u = nullptr;
          const float s0 = step(across, c0, u);
          const float s1 = step(across, c1, u);
          uint16_t a;
          uint16_t b;
          if (static_cast<int>(across) == lastAxis) {
            // The last segment's end corners: the same position, step and axis.
            a = lastPlus;
            b = lastMinus;
          } else {
            a = corner(p0, s0, u);
            b = corner(p0, -s0, u);
          }
          const uint16_t c = corner(p1, -s1, u);
          const uint16_t d = corner(p1, s1, u);
          quad(a, b, c, d);
          lastAxis = static_cast<int>(across);
          lastMinus = c;
          lastPlus = d;
          std::memcpy(p0, p1, sizeof(p0));
          c0 = c1;
        }
      } else {
        for (uint32_t k = 0; k + 1 < n; k += 2) {
          float p0[3];
          float p1[3];
          transform(src, idx[k], p0);
          transform(src, idx[k + 1], p1);
          const Clip c0 = clip_of(plan, p0);
          const Clip c1 = clip_of(plan, p1);
          const bool across = x_major(plan, c0, c1);
          const float* u = nullptr;
          const float s0 = step(across, c0, u);
          const float s1 = step(across, c1, u);
          const uint16_t a = corner(p0, s0, u);
          const uint16_t b = corner(p0, -s0, u);
          const uint16_t c = corner(p1, -s1, u);
          const uint16_t d = corner(p1, s1, u);
          quad(a, b, c, d);
        }
      }
      out.give_back(worst - used);
      chunk = first + used;
      counts.vertices += used;
      continue;
    }
    if (kind != kTriangles && kind != kTriangleStrip && kind != kTriangleFan && kind != kQuads) {
      ++counts.skipped;
      continue;
    }
    if (!fills) {
      continue;
    }
    const uint32_t worst = n; // a vertex for each index at most
    if (worst > kChunkVertices) {
      ++counts.skipped;
      continue;
    }
    if (chunk + worst > kChunkVertices) {
      flush();
    }
    if (static_cast<int>(prim.source) != runSource || std::memcmp(prim.color, runColor, sizeof(runColor)) != 0) {
      ++scratch.serial;
      runSource = prim.source;
      std::memcpy(runColor, prim.color, sizeof(runColor));
    }
    const uint32_t first = out.room(worst);
    uint32_t used = 0;
    const auto vertex = [&](uint32_t k) {
      const uint8_t i = idx[k];
      if (scratch.stamp[i] != scratch.serial) {
        scratch.stamp[i] = scratch.serial;
        float p[3];
        transform(src, i, p);
        out.put(first + used, p, prim.color);
        scratch.slot[i] = static_cast<uint16_t>(first + used++);
      }
      return scratch.slot[i];
    };
    const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
      const uint16_t va = vertex(a);
      const uint16_t vb = vertex(b);
      const uint16_t vc = vertex(c);
      indices.insert(indices.end(), {va, vb, vc});
      ++counts.triangles;
    };
    switch (kind) {
    case kTriangles:
      for (uint32_t i = 0; i + 2 < n; i += 3) {
        triangle(i, i + 1, i + 2);
      }
      break;
    case kTriangleStrip:
      for (uint32_t i = 0; i + 2 < n; ++i) {
        if ((i & 1) == 0) {
          triangle(i, i + 1, i + 2);
        } else {
          triangle(i + 1, i, i + 2);
        }
      }
      break;
    case kTriangleFan:
      for (uint32_t i = 1; i + 1 < n; ++i) {
        triangle(0, i, i + 1);
      }
      break;
    default: // kQuads
      for (uint32_t i = 0; i + 3 < n; i += 4) {
        triangle(i, i + 1, i + 2);
        triangle(i, i + 2, i + 3);
      }
      break;
    }
    out.give_back(worst - used);
    chunk = first + used;
    counts.vertices += used;
  }
  flush();
}

} // namespace aurora::gx::batch
