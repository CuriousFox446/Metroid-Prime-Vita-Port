#pragma once

// The PS Vita's screen through vitaGL (a GL layer over GXM), in place of SDL's GXM renderer:
// Phase 2 of the Vita port (docs/VITA_PHASE2.md). The retained display lists are drawn here
// (gx/vita_draw.cpp) with vitaGL's fixed-function pipeline, up to two textures each, then the
// ImGui overlay with ImGui's OpenGL 2 backend.
//
// vitaGL is not thread-safe: after initialize (game thread) every GL call is made on Aurora's
// render worker, where frames are presented.

#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

namespace aurora::vita_gl {
// The texture units a draw can use: vitaGL's fixed-function pipeline has two texture coordinate
// sets, each with its own texture matrix.
constexpr int kTexUnits = 2;

// A vertex of the Vita path's meshes: position, a texture coordinate set per unit, and a colour
// with the face's shading in it.
struct Vertex {
  float pos[3];
  float uv[kTexUnits][2];
  uint8_t color[4];
};
static_assert(sizeof(Vertex) == 32);

// Triangles decoded once from a GX display list (gx/vita_draw.cpp). Uploaded to the GPU on the
// render worker at its first draw; freed there once no frame draws it.
class Mesh;
std::shared_ptr<Mesh> make_mesh(std::vector<Vertex> triangles) noexcept;

// A GX texture's copy for the Vita's GPU, made beside the WebGPU one (gfx/texture.cpp): its first
// level only for now, as RGBA8 or as BC1 (DXT1, from an opaque CMPR texture). Uploaded on the
// render worker at its first draw; freed there once nothing holds it.
enum class TexelFormat : uint8_t { RGBA8, BC1 };
class Texture;
std::shared_ptr<Texture> make_texture(uint32_t width, uint32_t height, TexelFormat format,
                                      std::vector<uint8_t> level0) noexcept;

// How a unit samples its texture, as GL enums.
struct Sampler {
  uint16_t wrapS;
  uint16_t wrapT;
  uint16_t minFilter;
  uint16_t magFilter;
  bool operator==(const Sampler&) const = default;
};

// A texture unit of a draw: its texture (null: unit off), the texture coordinates' matrix (GX's
// rows 0 and 1, folded for GL's (s, t, 0, 1) input) and the sampler. Units modulate in order:
// colour * unit 0 * unit 1.
struct TexUnit {
  std::shared_ptr<Texture> texture;
  float matrix[8];
  Sampler sampler;
};

// One draw: a mesh with GX's position matrix (3x4, rows) and projection (4x4, rows, already in
// GL's depth range), and its texture units.
struct Draw {
  std::shared_ptr<Mesh> mesh;
  float modelView[12];
  float projection[16];
  TexUnit units[kTexUnits];
};

#ifdef AURORA_VITA_GL
// Starts vitaGL at the Vita's 960x544 (true). After it GXM is vitaGL's: SDL's renderer, GXM
// too, must never be created.
bool initialize() noexcept;
bool active() noexcept;
// ImGui's renderer backend (game thread, after ImGui's context exists).
void init_imgui() noexcept;
void shutdown_imgui() noexcept;
// The font atlas was rebuilt: its texture is uploaded again before the next present.
void fonts_changed() noexcept;
// FIFO thread: a draw of the frame being processed, and that frame's end (its draws go to the
// render worker's next present).
void record(Draw draw) noexcept;
void end_frame_draws() noexcept;
// Render worker: clears the screen, draws the overlay (null: none) and swaps.
void present(ImDrawData* overlay) noexcept;
// An RGBA8 texture for ImGui. Game thread; waits for the render worker to make it.
ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* rgba8) noexcept;
void remove_texture(ImTextureID id) noexcept;
#else
inline bool initialize() noexcept { return false; }
inline bool active() noexcept { return false; }
inline void init_imgui() noexcept {}
inline void shutdown_imgui() noexcept {}
inline void fonts_changed() noexcept {}
inline void record(Draw) noexcept {}
inline void end_frame_draws() noexcept {}
inline void present(ImDrawData*) noexcept {}
inline ImTextureID add_texture(uint32_t, uint32_t, const uint8_t*) noexcept { return ImTextureID{}; }
inline void remove_texture(ImTextureID) noexcept {}
#endif
} // namespace aurora::vita_gl
