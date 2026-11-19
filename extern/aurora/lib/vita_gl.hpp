#pragma once

#include "vita_fifo_clock.hpp"

// The PS Vita's screen through vitaGL (a GL layer over GXM), in place of SDL's GXM renderer:
// Phase 2 of the Vita port (docs/VITA_PHASE2.md). The retained display lists are drawn here
// (gx/vita_draw.cpp) with vitaGL's fixed-function pipeline, up to two textures each, then the
// ImGui overlay with ImGui's OpenGL 2 backend.
//
// vitaGL is not thread-safe: after initialize (game thread) every GL call is made on Aurora's
// render worker, where frames are presented.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
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

// Where GX's framebuffer (fbWidth x fbHeight, the render mode's) is drawn on the screen: pixels, origin
// at the top left. One of the screen's own shape, near enough (within 1%: the "follow window" and
// 16:9 framebuffers), fills it. One of another shape (4:3, the aspect setting's) keeps its shape, as
// large as fits, centred on whole pixels, black beside it (pillarboxed), as Aurora presents a
// framebuffer in a PC window (webgpu::calculate_present_viewport). Until 0.46.0 test 33 every
// framebuffer was stretched over the whole screen, so 4:3 looked like 16:9 widened.
struct ScreenFit {
  float left = 0.f;
  float top = 0.f;
  float width = static_cast<float>(kScreenWidth);
  float height = static_cast<float>(kScreenHeight);
};
inline ScreenFit fit_to_screen(uint32_t fbWidth, uint32_t fbHeight) noexcept {
  constexpr float kW = static_cast<float>(kScreenWidth);
  constexpr float kH = static_cast<float>(kScreenHeight);
  if (fbWidth == 0 || fbHeight == 0) {
    return {};
  }
  const float fw = static_cast<float>(fbWidth);
  const float fh = static_cast<float>(fbHeight);
  float w = kW;
  float h = std::min(kH, std::max(1.f, std::round(w * fh / fw)));
  if (h == kH) {
    w = std::min(kW, std::max(1.f, std::round(h * fw / fh)));
  }
  if (w >= kW * 0.99f && h >= kH * 0.99f) {
    return {};
  }
  return {std::floor((kW - w) / 2.f), std::floor((kH - h) / 2.f), w, h};
}

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
// A mesh's primitives: triangles, lines (two vertices each) or points.
enum class MeshPrimitive : uint8_t { Triangles, Lines, Points };
class Mesh;
std::shared_ptr<Mesh> make_mesh(std::vector<Vertex> triangles, MeshKind kind, bool clientMemory = false,
                                MeshPrimitive primitive = MeshPrimitive::Triangles) noexcept;
// Pads triangle indices to a multiple of 12 with triangles of one vertex (they draw nothing).
// vitaGL copies a draw's indices from client memory into its per-frame pool, which hands its
// memory out unaligned, one allocation after another: 6 bytes a triangle left what the frame put
// there after them off by 2, 4 or 6 bytes, and a BC1 texture's upload, staged there and read 8
// bytes at a time by sceGxmTransferCopy, came out as noise or black (b154f27's skinned calls).
// Whole multiples of 8 bytes keep the alignment the 56-byte vertices keep.
void pad_triangle_indices(std::vector<uint16_t>& indices) noexcept;

// A mesh made for one frame alone and drawn with a program (gx/vita_draw.cpp: immediate draws,
// GX's lines, skinned calls): its vertices among the frame's own (frame_vertices) and, drawn by
// index, its indices among the frame's (frame_indices, padded as pad_triangle_indices pads). The
// frame holds them till it is drawn, and vitaGL copies a draw's into its per-frame pool: no Mesh
// to make and free for each (a frame's 500 or more in Tallon Overworld, two allocations each on
// the FIFO thread and two frees on the reaper's, all through one heap lock).
struct FrameMesh {
  uint32_t vertexAt = 0;
  uint32_t vertexCount = 0; // 0: none, the draw's mesh is its Mesh
  uint32_t indexAt = 0;
  uint32_t indexCount = 0; // 0: its vertices drawn in order
  MeshKind kind = MeshKind::Other;
  MeshPrimitive primitive = MeshPrimitive::Triangles;
};

// A GX texture's copy for the Vita's GPU, made beside the WebGPU one (gfx/texture.cpp): as RGBA8
// (each level's texels in rows) or as BC1 (DXT1, from an opaque CMPR texture: each level's blocks
// in rows), `levels` of them back to back in `texels`, halving down (RGBA8's first level alone
// before 0.46.0 test 14). Since test 15 an intensity texture (GX's I4, I8) as I8, a byte a texel
// sampled as (I, I, I, I) (GL_INTENSITY: GXM's U8_RRRR), and one with alpha (IA4, IA8) as IA8, the
// intensity then the alpha, sampled as (I, I, I, A) (GL_LUMINANCE_ALPHA: GXM's A8L8): GX's own
// values, in a quarter and a half of RGBA8's bytes. Uploaded on the render worker at its first draw;
// freed there once nothing holds it. One with a single level samples it with a sampler's mipmapped
// filters as with their plain ones.
enum class TexelFormat : uint8_t { RGBA8, BC1, I8, IA8 };
inline constexpr int kTexelFormats = 4;
class Texture;
std::shared_ptr<Texture> make_texture(uint32_t width, uint32_t height, TexelFormat format,
                                      std::vector<uint8_t> texels, uint32_t levels = 1) noexcept;
// An EFB copy's texture: drawn into from the screen on the GPU, at each copy draw to it (render
// worker); transparent black before its first.
std::shared_ptr<Texture> make_copy_texture() noexcept;
// The memory a texture's levels hold (on the GPU once uploaded), for Aurora's cache budgets.
uint64_t texture_bytes(const Texture& texture) noexcept;

// How an EFB copy reads the screen into its RGBA texture, from GX's copy format: the colour
// (alpha opaque: a format without alpha), the colour and alpha, intensity (GX's YUV luma, in all
// four channels, as Aurora samples an I8 texture), intensity with alpha, or one channel in all
// four. The EFB's alpha reads as one where it has none (a Draw's copyAlpha).
// Or a copy read as a palette texture, converted through its palette: the copy's 8-bit value as a
// 4-bit index (C4: the value's top four bits, as GX's I4 copy keeps) or an 8-bit one (C8).
enum class CopyKind : uint8_t {
  Rgb,
  Rgba,
  Intensity,
  IntensityAlpha,
  Red,
  RedAlpha,
  Green,
  Blue,
  Alpha,
  Palette4,
  Palette8,
};

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

// A TexUnit as a draw takes it: its texture (null: unit off) held by the draw's frame (keep).
struct DrawUnit {
  Texture* texture = nullptr;
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
  // The alpha blended by factors of its own (glBlendFuncSeparate's), where GXSetDstAlpha's constant
  // is written (gx/vita_draw.cpp, dst_alpha_mode); else by the colour's.
  bool alphaBlendOwn = false;
  uint16_t alphaBlendSrc = 1; // GL_ONE
  uint16_t alphaBlendDst = 0; // GL_ZERO
  bool operator==(const RenderState&) const = default;
};

// A Cg program generated from a GX material's TEV configuration (gx/vita_tev.cpp). Built on the
// render worker at its first draw, one a frame at most: loaded from the memory card when a session
// before compiled it (by `key`, a hash of its source), else compiled with vitaGL's Cg compiler and
// saved there. Until it is ready, or if it fails, its draws use the fixed-function path.
class Program;
std::shared_ptr<Program> make_program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept;
// The binaries' format: the seed of a generated program's key (gx/vita_tev.cpp), changed when the same
// sources would compile to other binaries (the compiler's options, the attribute layout). The card's
// binaries are kept from build to build while it stays the same (0.42.0 test 20).
inline constexpr uint64_t kProgramBinaryVersion = 3;

// The constants a generated program may read, as vec4s: the konst colours (4), the TEV registers
// (PREV, REG0-2), the two channels' material and ambient colours (mat0, amb0, mat1, amb1), and
// the alpha compare's references (x, y); for indirect texturing, each coordinate slot's texel scale
// (GX's SU scale), each texture slot's size and the three indirect matrices (two rows each); GX's
// fog colour, and its A, B and C (x, y, z). A program declares only those its source reads, packed
// in this order (gx/vita_tev.cpp).
constexpr int kProgramConstants = 13 + kProgramCoords + kSamplers + 6 + 2;
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
// and the render worker, seeing the rows it sent last, sends nothing. The program and the
// textures are held by the draw's frame (keep).
// `wanted`: the draw's own program when `program` stands in for it (another light count's of the
// same material, linked while its own is not: gx/vita_tev.cpp, tev_stand_in); the render worker
// builds it on as it would at a draw of its own.
struct ProgramInputs {
  Program* program = nullptr;
  Program* wanted = nullptr;
  Texture* textures[kSamplers] = {};
  Sampler samplers[kSamplers];
  uint32_t vertexAt = 0;
  uint32_t constantsAt = 0;
  uint8_t vertexCount = 0;
  uint8_t constantCount = 0;
  RenderState state;
};

// One draw: a mesh (its Mesh, or a mesh of its frame's own) with GX's position matrix (3x4, rows)
// and projection (4x4, rows, already in GL's depth range); its fixed-function texture units and
// state; and its generated program, when it has one (drawn with it once the program is ready).
// GX's viewport and scissor, on the screen; a line or point mesh's width in the screen's pixels
// (GX's line width or point size; GXM takes whole pixels, 1 to 16).
// Or, with `clear` (and no mesh), the clear of an EFB copy: the scissor rectangle set to the clear
// colour and depth, where GX's colour, alpha and depth updates allow (as Dolphin clears); the
// colour and the alpha both or neither (one alone is a draw: gx/vita_draw.cpp, record_masked_clear).
// Or, with `copyTo` (and no mesh), an EFB copy: the scissor rectangle as the draws before it left
// the screen, into that texture as `copyKind` reads it, halved when `copyHalf` (GX's box filter);
// the screen's alpha read where `copyAlpha` (the EFB has alpha), else one. With a palette kind, a
// copy's conversion instead: units[0]'s texture (a copy) through units[1]'s (its palette, one row)
// into `copyTo`, at the copy's size.
// What it draws with (its mesh, textures and program) is held by its frame, once for all the draws
// using it (keep): held by each draw instead, as shared pointers, a draw took up to 13 of them,
// each copy an atomic count change behind two barriers on the FIFO thread and another as the
// reaper let it go (0.42.0 test 7's heavy room: some 1,150 draws a frame, the FIFO thread the one
// that set the frame rate).
struct Draw {
  Mesh* mesh = nullptr;
  FrameMesh frameMesh;
  float modelView[12];
  float projection[16];
  DrawUnit units[kTexUnits];
  RenderState state;
  ProgramInputs program;
  ScreenRect viewport;
  ScreenRect scissor;
  uint8_t width = 1;
  bool clear = false;
  bool clearColor = false;
  bool clearAlpha = false;
  bool clearDepth = false;
  float clearRgba[4] = {};
  float clearZ = 1.f;
  Texture* copyTo = nullptr;
  CopyKind copyKind = CopyKind::Rgb;
  bool copyHalf = false;
  bool copyAlpha = false;
};
// Nothing to let go of: a frame's draws are freed as one block.
static_assert(std::is_trivially_destructible_v<Draw> && std::is_trivially_copyable_v<Draw>);

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
void record(Draw&& draw) noexcept;
void end_frame_draws() noexcept;
// FIFO thread: the frame being recorded holds on to `object` till the render worker has drawn it
// and the reaper lets the frame go (once each frame, however many of its draws use it); returns
// the pointer a Draw takes. Null stays null.
Mesh* keep(const std::shared_ptr<Mesh>& object) noexcept;
Texture* keep(const std::shared_ptr<Texture>& object) noexcept;
Program* keep(const std::shared_ptr<Program>& object) noexcept;
// FIFO thread: rows of four floats added to the frame being recorded, for its draws' program
// constants; returns where they start. The rows recorded at `at` in that frame, and the frame's
// serial (a new one each frame).
uint32_t record_constants(const float (*rows)[4], int count) noexcept;
const float* recorded_constants(uint32_t at) noexcept;
uint32_t recording_serial() noexcept;
// FIFO thread: the draws recorded so far in the frame being recorded.
uint32_t recorded_draw_count() noexcept;
// FIFO thread: room for `count` vertices of a FrameMesh among the frame's own, `at` set to where
// it starts: the caller writes each whole (none is cleared first), before its next call here. The
// frame's vertices cut back to `at`'s first (a draw that could not be decoded after all).
Vertex* frame_vertices(uint32_t count, uint32_t& at) noexcept;
void trim_frame_vertices(uint32_t at) noexcept;
// FIFO thread: how many more vertices frame_vertices hands out before the frame's vertices move
// (they grow by doubling): a vertex written later by another thread (the core 0 helper) must be
// written before they do.
uint32_t frame_vertex_room() noexcept;
// FIFO thread: a FrameMesh's indices copied to the frame's own; returns where they start.
uint32_t frame_indices(const uint16_t* indices, uint32_t count) noexcept;
// The warm-up's side here (gx/vita_tev.cpp has the rest): the material records on the card, read at
// initialize, each a 32-bit length and its bytes; and a new one, appended there (FIFO thread; the
// card thread writes them a batch at a time).
const std::vector<uint8_t>& saved_materials() noexcept;
void save_material(const void* record, size_t size) noexcept;
// FIFO thread: whether the program `key` is on the card, being compiled or waiting to be; whether
// the warm-up has room for another (64 wait at most); and a program the card lacks, to compile
// ahead of its first draw (the render worker builds them one at a time, each dropped once saved).
bool program_known(uint64_t key) noexcept;
bool warm_up_room() noexcept;
void warm_up(uint64_t key, std::string vertexSource, std::string fragmentSource) noexcept;
// Any thread (the FIFO thread's tev_stand_in): whether `program` is linked, so a draw recorded with
// it now is drawn with it (a program once linked stays so).
bool program_ready(const Program* program) noexcept;
// Render worker: clears the screen, draws the overlay (null: none) and swaps.
void present(ImDrawData* overlay) noexcept;
// An RGBA8 texture for ImGui. Game thread; waits for the render worker to make it.
ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* rgba8) noexcept;
void remove_texture(ImTextureID id) noexcept;
// The system keyboard (SceImeDialog), the Vita's only one, for a text field. Game thread: asked
// for with the field's text (UTF-8, at most maxLength characters; false while one is up), then
// its outcome: 0 while it is up, 1 confirmed (its text in `text`), -1 cancelled or not opened,
// -2 none asked. While it is up, open (any thread) is true.
bool open_keyboard(const char* text, uint32_t maxLength) noexcept;
int keyboard_result(std::string& text) noexcept;
bool keyboard_open() noexcept;
// Any thread: the generated programs sent to the compiler thread since the start, and those it
// has finished (compiled or failed); the warm-up's waiting their turn count as sent.
void program_compiles(uint32_t& sent, uint32_t& finished) noexcept;
// The resolution the game's frames are drawn at: Aurora's frame buffer scale (window.cpp), on the
// Vita a multiple of its screen (1, and 0 for "native": 960x544). Any other is a framebuffer of its
// own, drawn to the screen at each frame's end (vita_gl.cpp, draw_world_to_display). The size a
// scale gives (its width a multiple of eight, its height of two; from a quarter of the screen to
// kMaxRenderScale times it), and the scale asked for (any thread; the next present takes it).
constexpr float kMaxRenderScale = 2.f;
void render_size_for_scale(float scale, int& width, int& height) noexcept;
void set_render_scale(float scale) noexcept;
// Sharpening of a frame drawn up from fewer pixels (aurora_set_present_sharpness; 0 off, to 1).
void set_present_sharpness(float sharpness) noexcept;
// The GPU's busy time a frame over the last half second or so, ms (vita_gl.cpp's GpuClock); 0
// before the first. Any thread.
float gpu_busy_ms() noexcept;
// vitaGL's swap interval from the next present on (any thread; applied on the render worker):
// the vblanks a frame stays up at least, 1 to 4.
void set_swap_interval(uint32_t vblanks) noexcept;
// The GPU cost test (0.46.0 test 14), from the next present on: each kind of draw left out in turn,
// what the GPU's busy time a frame is without it logged (vita_gl.cpp, gpu_test). Any thread.
void start_gpu_cost_test() noexcept;
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
inline uint32_t recorded_draw_count() noexcept { return 0; }
inline Vertex* frame_vertices(uint32_t, uint32_t&) noexcept { return nullptr; }
inline void trim_frame_vertices(uint32_t) noexcept {}
inline uint32_t frame_vertex_room() noexcept { return 0; }
inline uint32_t frame_indices(const uint16_t*, uint32_t) noexcept { return 0; }
inline const std::vector<uint8_t>& saved_materials() noexcept {
  static const std::vector<uint8_t> none;
  return none;
}
inline void save_material(const void*, size_t) noexcept {}
inline bool program_known(uint64_t) noexcept { return true; }
inline bool program_ready(const Program*) noexcept { return false; }
inline bool warm_up_room() noexcept { return false; }
inline void warm_up(uint64_t, std::string, std::string) noexcept {}
inline void present(ImDrawData*) noexcept {}
inline ImTextureID add_texture(uint32_t, uint32_t, const uint8_t*) noexcept { return ImTextureID{}; }
inline void remove_texture(ImTextureID) noexcept {}
inline bool open_keyboard(const char*, uint32_t) noexcept { return false; }
inline int keyboard_result(std::string&) noexcept { return -2; }
inline bool keyboard_open() noexcept { return false; }
inline void program_compiles(uint32_t& sent, uint32_t& finished) noexcept { sent = finished = 0; }
constexpr float kMaxRenderScale = 2.f;
inline void render_size_for_scale(float, int& width, int& height) noexcept {
  width = kScreenWidth;
  height = kScreenHeight;
}
inline void set_render_scale(float) noexcept {}
inline void set_present_sharpness(float) noexcept {}
inline float gpu_busy_ms() noexcept { return -1.f; }
inline void set_swap_interval(uint32_t) noexcept {}
inline void start_gpu_cost_test() noexcept {}
#endif
} // namespace aurora::vita_gl
