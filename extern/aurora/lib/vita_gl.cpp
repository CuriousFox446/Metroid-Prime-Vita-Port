#include "vita_gl.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"

#include <vitaGL.h>

#include "backends/imgui_impl_opengl2.h"

#include <fmt/format.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace aurora::vita_gl {
namespace {
constexpr Module Log{"aurora::vita_gl"};
constexpr int kWidth = 960;
constexpr int kHeight = 544;
// vitaGL takes the RAM the game's heap leaves; this much of it stays free for the system's
// own allocations (thread stacks, audio, file I/O).
constexpr int kRamThreshold = 32 * 1024 * 1024;
// For glBegin/glEnd; nothing here uses them.
constexpr int kLegacyPool = 256 * 1024;

bool g_active = false;
std::atomic_bool g_fontsChanged{false};

// The game's textures on the GPU, by format: count and bytes (uploaded on the render worker,
// freed wherever their last holder lets go).
std::atomic<int64_t> g_textureCount[2];
std::atomic<int64_t> g_textureBytes[2];

// vitaGL's memory by pool, free of total (MiB), and the game's heap left to it (newlib).
void log_memory(const char* when) {
  constexpr double kMiB = 1024.0 * 1024.0;
  const auto pool = [](vglMemType type) {
    return fmt::format("{:.1f}/{:.1f}", vglMemFree(type) / kMiB, vglMemTotal(type) / kMiB);
  };
  Log.info("{}: vitaGL memory free of total (MiB): VRAM {}, RAM {}, phycont {}, budget {}; newlib free {:.1f}; "
           "textures on the GPU: {} RGBA8 ({:.1f} MiB), {} BC1 ({:.1f} MiB)",
           when, pool(VGL_MEM_VRAM), pool(VGL_MEM_RAM), pool(VGL_MEM_PHYCONT), pool(VGL_MEM_BUDGET),
           vglMemFree(VGL_MEM_EXTERNAL) / kMiB, g_textureCount[0].load(), g_textureBytes[0].load() / kMiB,
           g_textureCount[1].load(), g_textureBytes[1].load() / kMiB);
}

// The frames' draws: recorded on the FIFO thread, handed over at each frame's end, drawn by the
// render worker's present for that frame, in order.
std::vector<Draw> g_recording;
std::mutex g_framesMutex;
std::deque<std::vector<Draw>> g_frames;

// Milestone 2 draws with vitaGL's fixed-function pipeline (the one ImGui's OpenGL 2 backend
// uses): GX's matrices go in through glLoadMatrixf, and the shading is in the vertex colours
// (gx/vita_draw.cpp). A custom Cg program crashed inside glLinkProgram on hardware (a null
// pointer in SceGxm, with both shaders reported compiled); generated Cg is milestone 4's.

// A GX matrix given as rows (3x4 or 4x4) as GL's column-major 4x4.
void to_column_major(const float* rows, int rowCount, float out[16]) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[c * 4 + r] = r < rowCount ? rows[r * 4 + c] : (r == c ? 1.f : 0.f);
    }
  }
}
} // namespace

class Mesh {
public:
  explicit Mesh(std::vector<Vertex> triangles) noexcept
  : m_cpu(std::move(triangles)), m_count(static_cast<GLsizei>(m_cpu.size())) {}
  ~Mesh() {
    if (m_vbo != 0) {
      const GLuint vbo = m_vbo;
      gfx::render_worker::enqueue_work([vbo] { glDeleteBuffers(1, &vbo); });
    }
  }

  // Render worker: binds the vertex buffer (made from the CPU copy at the first draw).
  GLsizei bind() noexcept {
    if (m_vbo == 0) {
      glGenBuffers(1, &m_vbo);
      glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_cpu.size() * sizeof(Vertex)), m_cpu.data(),
                   GL_STATIC_DRAW);
      std::vector<Vertex>().swap(m_cpu);
    } else {
      glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    }
    return m_count;
  }

private:
  std::vector<Vertex> m_cpu;
  GLuint m_vbo = 0;
  GLsizei m_count;
};

std::shared_ptr<Mesh> make_mesh(std::vector<Vertex> triangles) noexcept {
  return std::make_shared<Mesh>(std::move(triangles));
}

class Texture {
public:
  Texture(uint32_t width, uint32_t height, TexelFormat format, std::vector<uint8_t> level0) noexcept
  : m_cpu(std::move(level0)), m_width(width), m_height(height), m_format(format) {}
  ~Texture() {
    if (m_id != 0) {
      const GLuint id = m_id;
      gfx::render_worker::enqueue_work([id] { glDeleteTextures(1, &id); });
      const int kind = static_cast<int>(m_format);
      g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
    }
  }

  // Render worker: binds it to the active unit (made from the CPU copy at the first draw) and
  // sets its sampler state where it changed.
  void bind(const Sampler& sampler) noexcept {
    if (m_id == 0) {
      glGenTextures(1, &m_id);
      glBindTexture(GL_TEXTURE_2D, m_id);
      const auto width = static_cast<GLsizei>(m_width);
      const auto height = static_cast<GLsizei>(m_height);
      if (m_format == TexelFormat::BC1) {
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, width, height, 0,
                               static_cast<GLsizei>(m_cpu.size()), m_cpu.data());
      } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_cpu.data());
      }
      m_bytes = static_cast<int64_t>(m_cpu.size());
      const int kind = static_cast<int>(m_format);
      g_textureCount[kind].fetch_add(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_add(m_bytes, std::memory_order_relaxed);
      std::vector<uint8_t>().swap(m_cpu);
    } else {
      glBindTexture(GL_TEXTURE_2D, m_id);
    }
    if (!m_samplerSet || !(sampler == m_sampler)) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler.wrapS);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler.wrapT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, sampler.minFilter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sampler.magFilter);
      m_sampler = sampler;
      m_samplerSet = true;
    }
  }

private:
  std::vector<uint8_t> m_cpu;
  uint32_t m_width;
  uint32_t m_height;
  TexelFormat m_format;
  GLuint m_id = 0;
  int64_t m_bytes = 0;
  Sampler m_sampler{};
  bool m_samplerSet = false;
};

std::shared_ptr<Texture> make_texture(uint32_t width, uint32_t height, TexelFormat format,
                                      std::vector<uint8_t> level0) noexcept {
  return std::make_shared<Texture>(width, height, format, std::move(level0));
}

void record(Draw draw) noexcept { g_recording.push_back(std::move(draw)); }

void end_frame_draws() noexcept {
  const size_t drawn = g_recording.size();
  {
    std::lock_guard lock{g_framesMutex};
    g_frames.push_back(std::move(g_recording));
    // A frame the render worker never presented must not hold on to its meshes for good.
    while (g_frames.size() > 3) {
      g_frames.pop_front();
    }
  }
  g_recording = {};
  g_recording.reserve(drawn);
}

namespace {
// Render worker: the oldest frame handed over and not yet drawn.
std::vector<Draw> take_frame() {
  std::lock_guard lock{g_framesMutex};
  if (g_frames.empty()) {
    return {};
  }
  std::vector<Draw> draws = std::move(g_frames.front());
  g_frames.pop_front();
  return draws;
}

// For the log: presents and draws since the last report (render worker).
struct PresentStats {
  uint32_t presents = 0;
  uint32_t framesWithDraws = 0;
  uint64_t draws = 0;
};
PresentStats g_presentStats;

void set_enabled(GLenum cap, bool on) {
  on ? glEnable(cap) : glDisable(cap);
}

// Sets the state where it differs from `current` (all of it when `force`).
void apply_state(const RenderState& state, RenderState& current, bool force) {
  if (force || state.blend != current.blend) {
    set_enabled(GL_BLEND, state.blend);
  }
  if (force || state.blendEquation != current.blendEquation) {
    glBlendEquation(state.blendEquation);
  }
  if (force || state.blendSrc != current.blendSrc || state.blendDst != current.blendDst) {
    glBlendFunc(state.blendSrc, state.blendDst);
  }
  const bool alphaTest = state.alphaFunc != GL_ALWAYS;
  if (force || alphaTest != (current.alphaFunc != GL_ALWAYS)) {
    set_enabled(GL_ALPHA_TEST, alphaTest);
  }
  if (force || state.alphaFunc != current.alphaFunc || state.alphaRef != current.alphaRef) {
    glAlphaFunc(state.alphaFunc, state.alphaRef);
  }
  if (force || state.depthTest != current.depthTest) {
    set_enabled(GL_DEPTH_TEST, state.depthTest);
  }
  if (force || state.depthFunc != current.depthFunc) {
    glDepthFunc(state.depthFunc);
  }
  if (force || state.depthWrite != current.depthWrite) {
    glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
  }
  if (force || state.colorWrite != current.colorWrite || state.alphaWrite != current.alphaWrite) {
    const GLboolean color = state.colorWrite ? GL_TRUE : GL_FALSE;
    glColorMask(color, color, color, state.alphaWrite ? GL_TRUE : GL_FALSE);
  }
  if (force || (state.cullFace != 0) != (current.cullFace != 0)) {
    set_enabled(GL_CULL_FACE, state.cullFace != 0);
  }
  // GL keeps the last face culled while culling is off: 0 records culling off, not the face.
  const uint16_t heldFace = current.cullFace != 0 ? current.cullFace : 0xFFFF;
  if (state.cullFace != 0 && (force || state.cullFace != heldFace)) {
    glCullFace(state.cullFace);
  }
  current = state;
}

void draw_world(const std::vector<Draw>& draws) {
  if (draws.empty()) {
    return;
  }
  ++g_presentStats.framesWithDraws;
  g_presentStats.draws += draws.size();
  glUseProgram(0);
  // GX's front faces are clockwise (Aurora's WebGPU pipelines say so too).
  glFrontFace(GL_CW);
  RenderState current;
  apply_state(current, current, true);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  for (int unit = 0; unit < kTexUnits; ++unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glClientActiveTexture(GL_TEXTURE0 + unit);
    glDisable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  }
  bool unitOn[kTexUnits] = {};
  float matrix[16];
  for (const Draw& draw : draws) {
    const GLsizei count = draw.mesh->bind();
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, pos)));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
    // Both sets point into this mesh, a unit that is off too: no pointer is left into another buffer.
    for (int unit = 0; unit < kTexUnits; ++unit) {
      glClientActiveTexture(GL_TEXTURE0 + unit);
      glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex),
                        reinterpret_cast<const void*>(offsetof(Vertex, uv) + unit * sizeof(float[2])));
    }
    for (int unit = 0; unit < kTexUnits; ++unit) {
      const TexUnit& texUnit = draw.units[unit];
      const bool on = texUnit.texture != nullptr;
      if (!on && !unitOn[unit]) {
        continue;
      }
      glActiveTexture(GL_TEXTURE0 + unit);
      if (on != unitOn[unit]) {
        on ? glEnable(GL_TEXTURE_2D) : glDisable(GL_TEXTURE_2D);
        unitOn[unit] = on;
      }
      if (!on) {
        continue;
      }
      texUnit.texture->bind(texUnit.sampler);
      glMatrixMode(GL_TEXTURE);
      to_column_major(texUnit.matrix, 2, matrix);
      glLoadMatrixf(matrix);
    }
    glMatrixMode(GL_PROJECTION);
    to_column_major(draw.projection, 4, matrix);
    glLoadMatrixf(matrix);
    glMatrixMode(GL_MODELVIEW);
    to_column_major(draw.modelView, 3, matrix);
    glLoadMatrixf(matrix);
    apply_state(draw.state, current, false);
    glDrawArrays(GL_TRIANGLES, 0, count);
  }
  // Back to GL's defaults for the overlay (ImGui's backend sets blending, depth and culling
  // itself, but not the alpha test, the masks or the winding).
  apply_state(RenderState{.depthTest = false}, current, true);
  glFrontFace(GL_CCW);
  for (int unit = kTexUnits - 1; unit >= 0; --unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glClientActiveTexture(GL_TEXTURE0 + unit);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
  }
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

// An orange triangle in the screen's top-left corner, drawn every frame the same way with
// identity matrices: it shows that vitaGL's drawing reaches the screen whatever the game draws.
void draw_test_triangle() {
  static std::shared_ptr<Mesh> sMesh = [] {
    std::vector<Vertex> triangle(3);
    const float corners[3][2] = {{-0.97f, 0.97f}, {-0.80f, 0.97f}, {-0.97f, 0.70f}};
    for (int i = 0; i < 3; ++i) {
      triangle[i] = Vertex{{corners[i][0], corners[i][1], 0.f}, {}, {255, 140, 0, 255}};
    }
    return make_mesh(std::move(triangle));
  }();
  Draw draw{.mesh = sMesh};
  constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(draw.modelView, kIdentity, sizeof(draw.modelView));
  std::memcpy(draw.projection, kIdentity, sizeof(draw.projection));
  const std::vector<Draw> draws{draw};
  draw_world(draws);
  --g_presentStats.framesWithDraws;
  --g_presentStats.draws;
}
} // namespace

bool initialize() noexcept {
  // vitaGL compiles the shaders behind its fixed-function calls (the overlay's) with the Vita's
  // own compiler, which takes a while; it keeps them here, in the game's folder.
  static const std::string cachePath = std::string{g_config.userPath} + "/vitagl_shader_cache";
  std::error_code ec;
  std::filesystem::create_directories(cachePath, ec);
  vglSetShaderCachePath(cachePath.c_str());
  // Not a success flag: this vitaGL returns true when the requested size was beyond the display's
  // and it started at the display's own size instead, false when it started as asked. Either way
  // GXM is now vitaGL's, so nothing else (SDL's renderer) may initialise it after this.
  const bool clamped = vglInitExtended(kLegacyPool, kWidth, kHeight, kRamThreshold, SCE_GXM_MULTISAMPLE_NONE);
  g_active = true;
  if (clamped) {
    Log.warn("vitaGL started at the display's maximum size, not {}x{}", kWidth, kHeight);
  } else {
    Log.info("vitaGL started at {}x{}", kWidth, kHeight);
  }
  log_memory("started");
  return true;
}

bool active() noexcept { return g_active; }

void init_imgui() noexcept {
  // No GL calls: the backend makes its font texture at its first NewFrame (present).
  ImGui_ImplOpenGL2_Init();
}

void shutdown_imgui() noexcept {
  gfx::render_worker::enqueue_work([] { ImGui_ImplOpenGL2_Shutdown(); });
  gfx::render_worker::synchronize();
}

void fonts_changed() noexcept { g_fontsChanged.store(true, std::memory_order_release); }

void present(ImDrawData* overlay) noexcept {
  // The font texture is made here, on the render worker, from the atlas the game thread built;
  // the atlas only changes when the fonts do (fonts_changed).
  if (g_fontsChanged.exchange(false, std::memory_order_acq_rel)) {
    ImGui_ImplOpenGL2_DestroyFontsTexture();
  }
  ImGui_ImplOpenGL2_NewFrame();
  glViewport(0, 0, kWidth, kHeight);
  glDisable(GL_SCISSOR_TEST);
  // Dark blue, so a screen drawn by this path is told apart from a black one at a glance.
  glClearColor(0.05f, 0.08f, 0.16f, 1.f);
  glClearDepth(1.0);
  // The clear follows the masks; a draw's state may have left them off.
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  draw_world(take_frame());
  draw_test_triangle();
  if (++g_presentStats.presents == 300) {
    Log.info("presents: {}, {} with game draws, {:.1f} draws a frame", g_presentStats.presents,
             g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0);
    g_presentStats = {};
    log_memory("presents");
  }
  if (overlay != nullptr && overlay->CmdListsCount > 0) {
    ImGui_ImplOpenGL2_RenderDrawData(overlay);
  }
  vglSwapBuffers(GL_FALSE);
}

ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* rgba8) noexcept {
  GLuint texture = 0;
  gfx::render_worker::enqueue_work([&texture, width, height, rgba8] {
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba8);
  });
  gfx::render_worker::synchronize();
  return static_cast<ImTextureID>(static_cast<intptr_t>(texture));
}

void remove_texture(ImTextureID id) noexcept {
  const auto texture = static_cast<GLuint>(static_cast<intptr_t>(id));
  gfx::render_worker::enqueue_work([texture] { glDeleteTextures(1, &texture); });
}
} // namespace aurora::vita_gl
