#pragma once

#include "vita_fifo_clock.hpp"

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
// The screen vitaGL draws to, in pixels.
constexpr int kScreenWidth = 960;
constexpr int kScreenHeight = 544;

// A rectangle of the screen as GL takes it (glViewport, glScissor): pixels, origin at the bottom left.
struct ScreenRect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = kScreenWidth;
  int16_t height = kScreenHeight;
  bool operator==(const ScreenRect&) const = default;
};

// The texture units a fixed-function draw can use: vitaGL's fixed-function pipeline has two
// texture coordinate sets, each with its own texture matrix.
constexpr int kTexUnits = 2;
// The texture coordinate sets a mesh carries, a generated program's coordinate slots (the first
// kTexCoords may read the mesh's sets; the rest are made from the position or the normal) and the
// textures it samples. Seven slots and two lit channels are GXM's ten TEXCOORD varyings; the
// water's surfaces take up to seven textures and coordinates.
constexpr int kTexCoords = 4;
constexpr int kProgramCoords = 7;
constexpr int kSamplers = 8;

// A vertex of the Vita path's meshes: position, its texture coordinate sets, the colour the
// fixed-function draw uses (the face's shading in it), the vertex colour as GX has it (white
// without one), its normal (model space, unit length times 127; zero without one: for a
// generated program's lighting and texture coordinates) and the face's shading on its own.
struct Vertex {
  float pos[3];
  float uv[kTexCoords][2];
  uint8_t color[4];
  uint8_t raw[4];
  int8_t normal[3];
  uint8_t shade;
};
static_assert(sizeof(Vertex) == 56);

// What made a mesh (gx/vita_draw.cpp), for the log: a display list decoded once and kept, a
// skinned list call (positions made at run time, a mesh for that frame), an immediate draw or
// an indexed one (each a mesh for that frame), or anything else.
enum class MeshKind : uint8_t { List, Skinned, Immediate, Indexed, Other, Count };

// Triangles decoded from GX's geometry (gx/vita_draw.cpp). Uploaded to the GPU on the render
// worker at its first draw; freed once no frame draws it. A mesh made for one frame and drawn with
// a program (`clientMemory`) has no buffer: vitaGL copies it into its per-frame pool at the draw
// (one copy for a program's interleaved attributes), with no buffer to make, fill and delete. The
// fixed-function path copies each attribute apart, so its meshes keep a buffer.
// A mesh's primitives: triangles, or lines (two vertices each).
enum class MeshPrimitive : uint8_t { Triangles, Lines };
class Mesh;
std::shared_ptr<Mesh> make_mesh(std::vector<Vertex> triangles, MeshKind kind, bool clientMemory = false,
                                MeshPrimitive primitive = MeshPrimitive::Triangles) noexcept;
// Triangles drawn by index: `vertices` once each, `indices` (three a triangle, shared by every
// mesh made from them, padded by pad_triangle_indices) into them. A skinned call's mesh
// (gx/vita_draw.cpp): its template's distinct vertices posed for this frame, the template's
// corners as indices.
std::shared_ptr<Mesh> make_indexed_mesh(std::vector<Vertex> vertices,
                                        std::shared_ptr<const std::vector<uint16_t>> indices, MeshKind kind,
                                        bool clientMemory) noexcept;
// Pads triangle indices to a multiple of 12 with triangles of one vertex (they draw nothing).
// vitaGL copies a draw's indices from client memory into its per-frame pool, which hands its
// memory out unaligned, one allocation after another: 6 bytes a triangle left what the frame put
// there after them off by 2, 4 or 6 bytes, and a BC1 texture's upload, staged there and read 8
// bytes at a time by sceGxmTransferCopy, came out as noise or black (b154f27's skinned calls).
// Whole multiples of 8 bytes keep the alignment the 56-byte vertices keep.
void pad_triangle_indices(std::vector<uint16_t>& indices) noexcept;

// A GX texture's copy for the Vita's GPU, made beside the WebGPU one (gfx/texture.cpp): its first
// level only for now, as RGBA8 or as BC1 (DXT1, from an opaque CMPR texture). Uploaded on the
// render worker at its first draw; freed there once nothing holds it.
enum class TexelFormat : uint8_t { RGBA8, BC1 };
class Texture;
std::shared_ptr<Texture> make_texture(uint32_t width, uint32_t height, TexelFormat format,
                                      std::vector<uint8_t> level0) noexcept;
// An EFB copy's texture: filled from the screen on the render worker, at each copy draw to it.
std::shared_ptr<Texture> make_copy_texture() noexcept;
// The memory a texture's level holds (on the GPU once uploaded), for Aurora's cache budgets.
uint64_t texture_bytes(const Texture& texture) noexcept;

// How an EFB copy reads the screen into its RGBA texture, from GX's copy format: the colour
// (alpha opaque, as the game's EFB has none), the colour and alpha, intensity (GX's YUV luma,
// in all four channels, as Aurora samples an I8 texture), intensity with alpha, or one channel
// in all four.
enum class CopyKind : uint8_t { Rgb, Rgba, Intensity, IntensityAlpha, Red, RedAlpha, Green, Blue, Alpha };

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
// the alpha compare's references (x, y); for indirect texturing, each coordinate slot's texel scale
// (GX's SU scale), each texture slot's size and the three indirect matrices (two rows each). A
// program declares only those its source reads, packed in this order (gx/vita_tev.cpp).
constexpr int kProgramConstants = 13 + kProgramCoords + kSamplers + 6;
// The vertex program's (gx/vita_tev.cpp, tev_vertex_rows): the projection times the position
// matrix (4 rows); each texture coordinate slot's matrix (3 rows of 4 for a projective set, else 2)
// and its post-transform matrix (3 more, when it has one); for a lit program the position and
// normal matrices (3 rows each); and each lit channel's lights, five rows apiece (position,
// direction, colour, angle and distance attenuation), up to GX's eight for each of four channels.
constexpr int kProgramVertexConstants = 4 + 6 * kProgramCoords + 6 + 4 * 8 * 5;

// A draw's generated program and what it reads: its textures (sampler slot order), where its
// vertex and fragment constants are among its frame's constant rows (record_constants; as many as
// the program declares), and the fixed-function state it draws with (the program does the alpha
// test). A draw that repeats its program's last constants in the frame points at the same rows,
// and the render worker, seeing the rows it sent last, sends nothing.
struct ProgramInputs {
  std::shared_ptr<Program> program;
  std::shared_ptr<Texture> textures[kSamplers];
  Sampler samplers[kSamplers];
  uint32_t vertexAt = 0;
  uint32_t constantsAt = 0;
  uint8_t vertexCount = 0;
  uint8_t constantCount = 0;
  RenderState state;
};

// One draw: a mesh with GX's position matrix (3x4, rows) and projection (4x4, rows, already in
// GL's depth range); its fixed-function texture units and state; and its generated program, when
// it has one (drawn with it once the program is ready). GX's viewport and scissor, on the screen.
// Or, with `clear` (and no mesh), the clear of an EFB copy: the scissor rectangle set to the clear
// colour and depth, where GX's colour, alpha and depth updates allow (as Dolphin clears).
// Or, with `copyTo` (and no mesh), an EFB copy: the scissor rectangle as the draws before it left
// the screen, into that texture as `copyKind` reads it, halved when `copyHalf` (GX's box filter).
struct Draw {
  std::shared_ptr<Mesh> mesh;
  float modelView[12];
  float projection[16];
  TexUnit units[kTexUnits];
  RenderState state;
  ProgramInputs program;
  ScreenRect viewport;
  ScreenRect scissor;
  bool clear = false;
  bool clearColor = false;
  bool clearAlpha = false;
  bool clearDepth = false;
  float clearRgba[4] = {};
  float clearZ = 1.f;
  std::shared_ptr<Texture> copyTo;
  CopyKind copyKind = CopyKind::Rgb;
  bool copyHalf = false;
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
// FIFO thread: rows of four floats added to the frame being recorded, for its draws' program
// constants; returns where they start. The rows recorded at `at` in that frame, and the frame's
// serial (a new one each frame).
uint32_t record_constants(const float (*rows)[4], int count) noexcept;
const float* recorded_constants(uint32_t at) noexcept;
uint32_t recording_serial() noexcept;
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
inline uint32_t record_constants(const float (*)[4], int) noexcept { return 0; }
inline const float* recorded_constants(uint32_t) noexcept { return nullptr; }
inline uint32_t recording_serial() noexcept { return 0; }
inline void present(ImDrawData*) noexcept {}
inline ImTextureID add_texture(uint32_t, uint32_t, const uint8_t*) noexcept { return ImTextureID{}; }
inline void remove_texture(ImTextureID) noexcept {}
#endif
} // namespace aurora::vita_gl
