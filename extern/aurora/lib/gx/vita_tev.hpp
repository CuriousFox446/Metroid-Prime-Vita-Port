#pragma once

// PS Vita: Cg programs generated from GX's TEV configuration (milestone 4, docs/VITA_PHASE2.md),
// on the FIFO thread when a material is worked out (vita_draw.cpp). A program evaluates the TEV
// stages as GX does: colour and alpha combiners (lerp, add, subtract, compares, bias, scale,
// clamp) over the registers, konst colours, textures and raster colours, the swap tables, the
// indirect stages (offset coordinates, bump alpha), fog and the alpha compare. What it cannot do
// yet (texture coordinates made from colours, binormals or another generator, more than eight
// textures or seven coordinate sets, more than four distinct vertex attributes for them) leaves the
// material without one, to the fixed-function draw.

#include "gx.hpp"
#include "../vita_gl.hpp"

#include <array>
#include <memory>

namespace aurora::gx::vita {
// The texture coordinate sets and textures the current TEV stages sample, as a program's slots.
struct TevLayout {
  bool supported = false;
  int coordCount = 0;
  std::array<GXTexCoordID, vita_gl::kProgramCoords> coords{};
  // The mesh's coordinate set each slot's generator reads (-1: made from the position or the
  // normal), and the vertex attribute each mesh set holds (GX_TG_TEX0 to 7; -1: unused). Slots
  // whose generators read the same attribute share its set (the blur's seven taps, all TEX0).
  std::array<s8, vita_gl::kProgramCoords> meshSet{};
  std::array<s8, vita_gl::kTexCoords> meshSetSource{};
  int samplerCount = 0;
  std::array<GXTexMapID, vita_gl::kSamplers> maps{};
  // Each stage's coordinate slot (-1: none: it neither samples nor offsets a coordinate of its own)
  // and texture slot (-1: the stage samples none).
  std::array<s8, MaxTevStages> stageCoord{};
  std::array<s8, MaxTevStages> stageMap{};
  // The indirect stages a TEV stage reads (its offset matrix, or the bump alpha as its raster
  // colour), by GX's index: their coordinate and texture slots (-1: not read).
  bool indirect = false;
  std::array<s8, MaxIndStages> indCoord{};
  std::array<s8, MaxIndStages> indMap{};
};

// A generated program and the constants it declares: the vertex program's count
// (vita_gl::kProgramVertexConstants' layout), and the fragment program's, as the slots of
// vita_gl::kProgramConstants they hold in order.
struct TevProgram {
  std::shared_ptr<vita_gl::Program> program;
  int vertexCount = 0;
  int constantCount = 0;
  std::array<u8, vita_gl::kProgramConstants> constantSlots{};
  // Its vertex rows' layout (tev_vertex_rows): which coordinate sets have a post-transform matrix,
  // and the lights each channel (GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1) sums, its mask's
  // first ones in order (none: the position and normal matrices are left out too).
  std::array<bool, vita_gl::kProgramCoords> post{};
  std::array<u8, 4> lightCounts{};
  // It reads an indirect stage (for the log).
  bool indirect = false;
  // FIFO thread (record_draw): where its last draw's constants went among the rows of the frame
  // being recorded (`recordedIn`, that frame's serial; another: none yet), for a draw repeating them.
  mutable u32 recordedIn = 0;
  mutable u32 vertexAt = 0;
  mutable u32 constantsAt = 0;
};

// The layout of the current GX state (supported false when a program cannot draw it).
TevLayout tev_layout() noexcept;
// The program for the current GX state with that layout, generated once (cached by its source;
// the pointer stays valid for the session).
const TevProgram* tev_program(const TevLayout& layout) noexcept;
// The fragment constants `program` declares, packed, from the current GX state; `coordIds` and
// `mapIds` are the layout's coordinate generators and textures by slot (TevLayout's coords, maps).
void tev_constants(const TevProgram& program, const u8* coordIds, const u8* mapIds,
                   float out[vita_gl::kProgramConstants][4]) noexcept;
// The vertex rows `program` declares after the 4 of the projection times the position matrix,
// written from out[4] on (vita_gl::kProgramVertexConstants' layout), from the current GX state:
// the coordinate sets' generators `coords`, GX's current position and normal matrices and lights.
// Returns the rows in all.
int tev_vertex_rows(const TevProgram& program, const TcgConfig* const coords[], int coordCount,
                    float (*out)[4]) noexcept;
// Programs generated so far.
size_t tev_program_count() noexcept;
// Of them those that light (a channel part sums lights), and the distinct combinations of the four
// parts' light counts among those; those that read an indirect stage.
struct TevProgramCounts {
  size_t all = 0;
  size_t lit = 0;
  size_t lightCombinations = 0;
  size_t indirect = 0;
};
TevProgramCounts tev_program_counts() noexcept;
// False with MP_VITA_TEV=0: every draw fixed-function.
bool tev_enabled() noexcept;
} // namespace aurora::gx::vita
