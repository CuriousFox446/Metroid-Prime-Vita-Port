#pragma once

// PS Vita: where the FIFO thread's frame goes, for the log (docs/VITA_PHASE2.md). One frame in
// eight is timed, section by section: each mark ends a section where the last one ended, so the
// sections add up to the frame, from the end of the last frame's end task to the end of this one's.
// Marks go on the FIFO thread (and on the game thread when it ends a frame itself), never two at
// once. The [fifo path] line gives them once a second, per timed frame.

#include <cstdint>

namespace aurora::vita_gl::fifo_clock {
enum Section : uint8_t {
  kIdle,        // waiting for the game to send commands
  kState,       // register writes (BP, CP, XF, indexed XF loads), the stream's own reading
  kCopies,      // EFB copies and clears (BP 0x52)
  kDispatch,    // a draw command up to the Vita path: its reading, a list call's find of its list
  kMaterial,    // the Vita path: a draw's material (current_material: texture copies, programs)
  kLookup,      // the Vita path: a list call's cached triangles found (decoded first when none match)
  kSkinned,     // the Vita path: a skinned list call's template found and its vertices posed (or the
                // call decoded whole)
  kSkinnedCopy, // the Vita path: a skinned call's posed vertices copied into the frame
  kImmediate,   // the Vita path: an immediate or indexed draw's vertices decoded and triangulated
  kMesh,        // the Vita path: the mesh made of them (or a cached list's taken) for record_draw
  kRecord,      // the Vita path: record_draw (state, matrices, rectangles, units, the draw into the frame)
  kRows,        // the Vita path: record_draw's program rows (vertex rows, constants, into the frame's)
  kOtherDraws,  // the rest of a draw command: draws the Vita path did not take, to Aurora's WebGPU path
  kAurora,      // Aurora's other extension commands (texture and array loads, resident lists' retains...)
  kEndVita,     // the frame end: the Vita frame handed over (end_frame_draws), the stats logs
  kEndTextures, // texture::end_frame
  kEndFinish,   // gfx::finish
  kEndFrame,    // gfx::end_frame and the rest of the frame end
  kSectionCount
};

#ifdef AURORA_VITA_GL
extern bool g_on; // timing this frame
void mark_timed(Section section) noexcept;
inline void mark(Section section) noexcept {
  if (g_on) {
    mark_timed(section);
  }
}
// The frame's end task is done: counts the frame if it was timed, decides on the next, and logs
// once a second.
void frame_end() noexcept;
#else
inline void mark(Section) noexcept {}
inline void frame_end() noexcept {}
#endif
} // namespace aurora::vita_gl::fifo_clock
