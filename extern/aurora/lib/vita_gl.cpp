#include "vita_gl.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"

#include <vitaGL.h>

#include "backends/imgui_impl_opengl2.h"

#include <atomic>
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

// The frames' draws: recorded on the FIFO thread, handed over at each frame's end, drawn by the
// render worker's present for that frame, in order.
std::vector<Draw> g_recording;
std::mutex g_framesMutex;
std::deque<std::vector<Draw>> g_frames;

// Milestone 2's one program: GX's position matrix and projection, shaded by the face normal
// as seen from the camera, times the vertex colour. Rows rather than matrices, so no layout
// question arises between GL and Cg.
constexpr const char* kVertexShader = R"(
void main(float3 aPos, float3 aNrm, float4 aColor,
          uniform float4 uMv0, uniform float4 uMv1, uniform float4 uMv2,
          uniform float4 uProj0, uniform float4 uProj1, uniform float4 uProj2, uniform float4 uProj3,
          float4 out vColor : COLOR, float4 out vPos : POSITION) {
  float4 p = float4(aPos, 1.0);
  float4 v = float4(dot(uMv0, p), dot(uMv1, p), dot(uMv2, p), 1.0);
  vPos = float4(dot(uProj0, v), dot(uProj1, v), dot(uProj2, v), dot(uProj3, v));
  float3 n = float3(dot(uMv0.xyz, aNrm), dot(uMv1.xyz, aNrm), dot(uMv2.xyz, aNrm));
  float len = length(n);
  float shade = len > 0.0001 ? 0.35 + 0.65 * abs(n.z / len) : 1.0;
  vColor = float4(aColor.rgb * shade, 1.0);
}
)";
constexpr const char* kFragmentShader = R"(
float4 main(float4 vColor : COLOR) : COLOR { return vColor; }
)";

struct Program {
  GLuint program = 0;
  GLint mv[3]{};
  GLint proj[4]{};
  bool tried = false;
};
Program g_program;

GLuint compile(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  const GLint length = static_cast<GLint>(std::strlen(source));
  glShaderSource(shader, 1, &source, &length);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    Log.error("{} shader did not compile: {}", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
  }
  return shader;
}

// Render worker. The first call compiles (several seconds on a Vita, then from the shader cache).
bool use_program() {
  if (!g_program.tried) {
    g_program.tried = true;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aNrm");
    glBindAttribLocation(program, 2, "aColor");
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
      Log.error("the milestone 2 program did not link");
      return false;
    }
    static constexpr const char* kMv[3] = {"uMv0", "uMv1", "uMv2"};
    static constexpr const char* kProj[4] = {"uProj0", "uProj1", "uProj2", "uProj3"};
    for (int i = 0; i < 3; ++i) {
      g_program.mv[i] = glGetUniformLocation(program, kMv[i]);
    }
    for (int i = 0; i < 4; ++i) {
      g_program.proj[i] = glGetUniformLocation(program, kProj[i]);
    }
    g_program.program = program;
    Log.info("the milestone 2 program is ready");
  }
  if (g_program.program == 0) {
    return false;
  }
  glUseProgram(g_program.program);
  return true;
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

void record(Draw draw) noexcept { g_recording.push_back(std::move(draw)); }

void end_frame_draws() noexcept {
  std::lock_guard lock{g_framesMutex};
  g_frames.push_back(std::move(g_recording));
  g_recording = {};
  // A frame the render worker never presented must not hold on to its meshes for good.
  while (g_frames.size() > 3) {
    g_frames.pop_front();
  }
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

void draw_world(const std::vector<Draw>& draws) {
  if (draws.empty() || !use_program()) {
    return;
  }
  ++g_presentStats.framesWithDraws;
  g_presentStats.draws += draws.size();
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  for (const Draw& draw : draws) {
    const GLsizei count = draw.mesh->bind();
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(12));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void*>(24));
    for (int i = 0; i < 3; ++i) {
      glUniform4fv(g_program.mv[i], 1, draw.modelView + i * 4);
    }
    for (int i = 0; i < 4; ++i) {
      glUniform4fv(g_program.proj[i], 1, draw.projection + i * 4);
    }
    glDrawArrays(GL_TRIANGLES, 0, count);
  }
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(0);
  glDisable(GL_DEPTH_TEST);
}

// An orange triangle in the screen's top-left corner, drawn every frame with the same program
// and identity matrices: it shows that vitaGL's drawing reaches the screen whatever the game draws.
void draw_test_triangle() {
  static std::shared_ptr<Mesh> sMesh = [] {
    std::vector<Vertex> triangle(3);
    const float corners[3][2] = {{-0.97f, 0.97f}, {-0.80f, 0.97f}, {-0.97f, 0.70f}};
    for (int i = 0; i < 3; ++i) {
      triangle[i] = Vertex{{corners[i][0], corners[i][1], 0.f}, {0.f, 0.f, 1.f}, {255, 140, 0, 255}};
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
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  draw_world(take_frame());
  draw_test_triangle();
  if (++g_presentStats.presents == 300) {
    Log.info("presents: {}, {} with game draws, {:.1f} draws a frame; program {}", g_presentStats.presents,
             g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0,
             g_program.program != 0 ? "ready" : g_program.tried ? "FAILED" : "not tried");
    g_presentStats = {};
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
