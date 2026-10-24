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
#include <string>
#include <vector>

#include <imgui.h>

namespace aurora::vita_gl {
// The texture units a fixed-function draw can use: vitaGL's fixed-function pipeline has two
// texture coordinate sets, each with its own texture matrix.
constexpr int kTexUnits = 2;
// The texture coordinate sets a mesh carries, and the textures a generated program samples.
constexpr int kTexCoords = 4;
constexpr int kSamplers = 4;

// A vertex of the Vita path's meshes: position, its texture coordinate sets, the colour the
// fixed-function draw uses (the face's shading in it), the vertex colour as GX has it (white
// without one) and the face's shading on its own (for a generated program's raster colour).
struct Vertex {
  float pos[3];
  float uv[kTexCoords][2];
  uint8_t color[4];
  uint8_t raw[4];
  uint8_t shade;
  uint8_t pad[3];
};
static_assert(sizeof(Vertex) == 56);

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
// rows 0 and 1, folded for GL's (s, t, 0, 1) input), the sampler, and how it combines with what
// came before it, as GL's texture environment mode: GL_MODULATE (colour * unit 0 * unit 1) or
// GL_ADD (a glow or incandescence pass added on top).
struct TexUnit {
  std::shared_ptr<Texture> texture;
  float matrix[8];
  Sampler sampler;
  uint16_t envMode = 0x2100; // GL_MODULATE
  uint8_t coord = 0;         // the mesh's texture coordinate set
};

// A draw's fixed-function state, as GL enums (gx/vita_draw.cpp maps GX's to these): blending,
// the alpha test, depth, the colour mask and culling. The defaults are an opaque, depth-tested draw.
struct RenderState {
  bool blend = false;
  uint16_t blendEquation = 0x8006; // GL_FUNC_ADD
  uint16_t blendSrc = 1;           // GL_ONE
  uint16_t blendDst = 0;           // GL_ZERO
  uint16_t alphaFunc = 0x0207;     // GL_ALWAYS: no alpha test
  float alphaRef = 0.f;
  bool depthTest = true;
  uint16_t depthFunc = 0x0203; // GL_LEQUAL
  bool depthWrite = true;
  bool colorWrite = true;
  bool alphaWrite = true;
  uint16_t cullFace = 0; // 0: none; else GL_FRONT, GL_BACK or GL_FRONT_AND_BACK (GX's front is clockwise)
  bool operator==(const RenderState&) const = default;
};

// A Cg program generated from a GX material's TEV configuration (gx/vita_tev.cpp). Built on the
// render worker at its first draw, one a frame at most: loaded from the memory card when a session
// before compiled it (by `key`, a hash of its source), else compiled with vitaGL's Cg compiler and
// saved there. Until it is ready, or if it fails, its draws use the fixed-function path.
class Program;
std::shared_ptr<Program> make_program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept;

// The constants a generated program may read, as vec4s: the konst colours (4), the TEV registers
// (PREV, REG0-2), the two channels' material and ambient colours (mat0, amb0, mat1, amb1), and
// the alpha compare's references (x, y). A program declares only those its source reads, packed
// in this order (gx/vita_tev.cpp).
constexpr int kProgramConstants = 13;
// The vertex program's: the projection times the position matrix (4 rows), then each texture
// coordinate set's matrix (3 rows of 4 for a projective set, else 2, for GX's (s, t, 1, 1) input).
constexpr int kProgramVertexConstants = 4 + 3 * kTexCoords;

// A draw's generated program and what it reads: its textures (sampler slot order), its vertex and
// fragment constants (the first vertexCount and constantCount of each, as the program declares
// them), and the fixed-function state it draws with (the program does the alpha test).
struct ProgramInputs {
  std::shared_ptr<Program> program;
  std::shared_ptr<Texture> textures[kSamplers];
  Sampler samplers[kSamplers];
  float vertex[kProgramVertexConstants][4];
  float constants[kProgramConstants][4];
  uint8_t vertexCount = 0;
  uint8_t constantCount = 0;
  RenderState state;
};

// One draw: a mesh with GX's position matrix (3x4, rows) and projection (4x4, rows, already in
// GL's depth range); its fixed-function texture units and state; and its generated program, when
// it has one (drawn with it once the program is ready).
struct Draw {
  std::shared_ptr<Mesh> mesh;
  float modelView[12];
  float projection[16];
  TexUnit units[kTexUnits];
  RenderState state;
  ProgramInputs program;
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
