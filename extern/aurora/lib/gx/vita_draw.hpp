#pragma once

// The Vita's draws through vitaGL (../vita_gl.hpp, docs/VITA_PHASE2.md), on the FIFO thread.
// Milestone 2: the retained display lists (the retail world and models, MP_RETAIL_RESIDENT) are
// decoded once into triangles and drawn each time they are called, with GX's position matrix and
// projection. Lists with per-vertex matrices or vertices made at run time are not drawn yet.

#include "resident.hpp"

namespace aurora::gx::vita {
#ifdef AURORA_VITA_GL
// A call of a retained display list (call_resident_dl).
void draw_resident(const void* key, const resident::Entry& entry, u32 size) noexcept;
// The list was released (GX_AURORA_RESIDENT_RELEASE): its decoded triangles go.
void release(const void* key) noexcept;
// Logs what became of the calls since the last call (FIFO thread, at a frame end).
void log_stats() noexcept;
#else
inline void draw_resident(const void*, const resident::Entry&, u32) noexcept {}
inline void release(const void*) noexcept {}
inline void log_stats() noexcept {}
#endif
} // namespace aurora::gx::vita
