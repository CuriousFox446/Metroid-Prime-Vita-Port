#include "vita_gl.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"

#include <vitaGL.h>

#include "backends/imgui_impl_opengl2.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
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
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
