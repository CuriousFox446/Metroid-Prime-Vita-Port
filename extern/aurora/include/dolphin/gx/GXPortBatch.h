#ifndef DOLPHIN_GXPORTBATCH_H
#define DOLPHIN_GXPORTBATCH_H

// Port extension (0.42.0 test 25): primitives drawn as one draw, in their order, each in its own
// colour. Kept out of gx.h so that only its users rebuild when it changes.
//
// It was made for the map (CMapWorld::DrawAreas). The map's surfaces were a draw for each fill and
// each outline, every one in a TEV constant of its own: some 300 draws a frame on the minimap and
// 2,600 to 5,400 on the map screen, each costing the PS Vita's FIFO thread 15 to 50 us.
//
// A source is an array of positions (x, y, z as f32 in the host's byte order) and the matrix (rows)
// that takes them to the position matrix's space. The positions must stay as they are until the GPU
// has drawn them, as GXSetArray's arrays must. A primitive names a source, a GXPrimitive (triangles,
// strips, fans and quads; lines and line strips), a colour (RGBA) and `count` 8-bit indices into the
// source, which follow its record.
//
// Triangles are drawn as GX draws them (Aurora's order: a strip's odd triangles turned, a fan's
// about its first vertex). Lines are drawn as quads one pixel of the frame wide, across their major
// axis, as GXM draws its lines (the Vita's lines are all a pixel wide), and are never culled. The
// colour is the vertices' own, as if one TEV stage passed the unlit vertex colour on. Everything
// else is the current state: blending, depth, culling, alpha test, fog, the position matrix, the
// projection, the viewport and the scissor.
//
// GXPortDrawBatchAvailable is false where a batch cannot be drawn (no Vita path, or a display list
// being recorded). GXPortDrawBatch returns false when it wrote nothing for that reason.

#include <dolphin/types.h>
#include <dolphin/gx/GXEnum.h>

// Payload: u32 the sources' count, u32 the primitives' bytes, the sources (GXPortBatchSource, as the
// host lays them out), the primitives (GXPortBatchPrim records, each followed by its 8-bit indices).
#define GX_AURORA_PORT_DRAW_BATCH 0x0078

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GXPortBatchSource {
  const f32* positions;
  u32 count;
  f32 mtx[3][4];
} GXPortBatchSource;

typedef struct GXPortBatchPrim {
  u8 primitive; // GXPrimitive
  u8 source;
  u16 count; // indices after the record
  u8 color[4]; // RGBA
} GXPortBatchPrim;

GXBool GXPortDrawBatchAvailable(void);
GXBool GXPortDrawBatch(const GXPortBatchSource* sources, u32 sourceCount, const void* prims, u32 primBytes);

#ifdef __cplusplus
}
#endif

#endif
