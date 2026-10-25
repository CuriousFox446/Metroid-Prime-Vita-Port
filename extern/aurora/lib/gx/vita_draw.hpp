#pragma once

// The Vita's draws through vitaGL (../vita_gl.hpp, docs/VITA_PHASE2.md), on the FIFO thread.
// The retained display lists (the retail world and models, MP_RETAIL_RESIDENT) are decoded once
// into triangles and drawn each time they are called, with GX's position matrix and projection;
// a call whose positions the CPU makes each frame (skinned models) and the immediate draws are
// decoded each time. Lists with per-vertex matrices, lines and points are not drawn yet.

#include "resident.hpp"

#include <span>

namespace aurora::gx::vita {
#ifdef AURORA_VITA_GL
// A call of a retained display list (call_resident_dl); false when this path cannot draw it.
bool draw_resident(const void* key, const resident::Entry& entry, u32 size) noexcept;
// An immediate draw: one primitive's vertices as the FIFO carries them (draw_prim), or an
// indexed draw's vertices and its host-order 16-bit indices (GX_AURORA_DRAW_INDEXED). False when
// this path cannot draw it.
bool draw_immediate(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, std::span<const u8> vertices) noexcept;
bool draw_indexed(GXVtxFmt fmt, u16 vtxCount, std::span<const u8> vertices, std::span<const u8> indices) noexcept;
// The list was released (GX_AURORA_RESIDENT_RELEASE): its decoded triangles go.
void release(const void* key) noexcept;
// A list was retained (GX_AURORA_RESIDENT_RETAIN): which arrays are resident may have changed.
void retained() noexcept;
// An EFB copy into a texture (BP 0x52): the copy of its source from the screen, in the frame's
// draws at this point, into the destination's Vita texture (a depth copy is not made: transparent
// black stands in for it); then, when it clears, the clear of its source.
void record_copy() noexcept;
void record_copy_clear() noexcept;
// Logs what became of the calls since the last call (FIFO thread, at a frame end).
void log_stats() noexcept;
// Frees the cached materials holding a texture nothing else holds (FIFO thread, at a frame end).
void drop_lone_textures() noexcept;
#else
inline bool draw_resident(const void*, const resident::Entry&, u32) noexcept { return false; }
inline bool draw_immediate(GXPrimitive, GXVtxFmt, u16, std::span<const u8>) noexcept { return false; }
inline bool draw_indexed(GXVtxFmt, u16, std::span<const u8>, std::span<const u8>) noexcept { return false; }
inline void release(const void*) noexcept {}
inline void retained() noexcept {}
inline void record_copy() noexcept {}
inline void record_copy_clear() noexcept {}
inline void log_stats() noexcept {}
inline void drop_lone_textures() noexcept {}
#endif
} // namespace aurora::gx::vita
