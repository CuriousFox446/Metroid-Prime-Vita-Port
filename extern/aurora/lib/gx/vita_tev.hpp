#pragma once

// PS Vita: Cg programs generated from GX's TEV configuration (milestone 4, docs/VITA_PHASE2.md),
// on the FIFO thread when a material is worked out (vita_draw.cpp). A program evaluates the TEV
// stages as GX does: colour and alpha combiners (lerp, add, subtract, compares, bias, scale,
// clamp) over the registers, konst colours, textures and raster colours, the swap tables, and the
// alpha compare. What it cannot do yet (indirect textures, texture coordinates made from positions
// or normals, more than four textures or coordinate sets) leaves the material without one, to the
// fixed-function draw.

#include "gx.hpp"
#include "../vita_gl.hpp"

#include <array>
#include <memory>

namespace aurora::gx::vita {
// The texture coordinate sets and textures the current TEV stages sample, as a program's slots.
struct TevLayout {
  bool supported = false;
  int coordCount = 0;
  std::array<GXTexCoordID, vita_gl::kTexCoords> coords{};
  int samplerCount = 0;
  std::array<GXTexMapID, vita_gl::kSamplers> maps{};
  // Each stage's coordinate and texture slots (-1: the stage samples none).
  std::array<s8, MaxTevStages> stageCoord{};
  std::array<s8, MaxTevStages> stageMap{};
};

// The layout of the current GX state (supported false when a program cannot draw it).
TevLayout tev_layout() noexcept;
// The program for the current GX state with that layout, generated once (cached by its source).
std::shared_ptr<vita_gl::Program> tev_program(const TevLayout& layout) noexcept;
// The constants a program reads (vita_gl::kProgramConstants), from the current GX state.
void tev_constants(float out[vita_gl::kProgramConstants][4]) noexcept;
// A coordinate generator's matrix as three rows of four, for GX's (s, t, 1, 1) input.
void tev_texgen_rows(const TcgConfig& tcg, float out[12]) noexcept;
// Programs generated so far.
size_t tev_program_count() noexcept;
// False with MP_VITA_TEV=0: every draw fixed-function.
bool tev_enabled() noexcept;
} // namespace aurora::gx::vita
