#include "vita_gl.hpp"
#include "vita_fifo_clock.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"

#include <vitaGL.h>
#include <vitashark.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

// vitaGL's display depth surface (its gxm.c), which its API does not give: keep_display_depth.
extern "C" SceGxmDepthStencilSurface gxm_depth_stencil_surface;

#include "backends/imgui_impl_opengl2.h"

#include <cstdio>

#include <fmt/format.h>

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace aurora::vita_gl {
namespace {
constexpr Module Log{"aurora::vita_gl"};
// The screen's size (screen_width): the Vita's, or resolution.txt's (read_resolution).
int g_width = 960;
int g_height = 544;
// vitaGL takes the RAM the game's heap leaves; this much of it stays free for the system's
// own allocations (thread stacks, audio, file I/O).
constexpr int kRamThreshold = 32 * 1024 * 1024;
// For glBegin/glEnd; nothing here uses them.
constexpr int kLegacyPool = 256 * 1024;
// The scenes a frame vitaGL makes the display's render target and each framebuffer's for (each EFB
// copy ends the display's scene and draws two of a framebuffer's: initialize); draw_world and
// copy_on_gpu keep each frame within them. The display's, GXM's most. A framebuffer's, the most a
// frame was seen to need (the copies' 480x272 scratch on the pause screen's first frame): GXM
// sizes the memory of each target it makes by them, from the system's (initialize logs how
// much), and at 8 the pause screen's copy came back black (93518bf), its target likely not made.
constexpr uint8_t kDisplayScenesPerFrame = 8;
constexpr uint8_t kFramebufferScenesPerFrame = 3;

bool g_active = false;
std::atomic_bool g_fontsChanged{false};

// The game's textures on the GPU, by format: count and bytes (uploaded on the render worker,
// freed wherever their last holder lets go).
std::atomic<int64_t> g_textureCount[2];
std::atomic<int64_t> g_textureBytes[2];
// Textures made and not yet uploaded (no draw of theirs bound them): their pixels on the heap.
std::atomic<int64_t> g_pendingCount;
std::atomic<int64_t> g_pendingBytes;
// Copy textures' render targets made (Texture::render_target, one a size), for the log.
std::atomic<uint32_t> g_renderTargets;

// A present's costs that are not every draw's (render worker), for the line a slow present logs
// (present): program builds (loads from the card, links of background compiles, compiles here),
// textures' and meshes' first uploads, fixed-function draws (vitaGL compiles a shader for a
// combination its cache lacks inside one), EFB copies and the swap. The timed presents' sections
// (one in kTimedEvery) missed the 1.4 s presents of a room's arrival.
struct PresentCosts {
  uint64_t start = 0;
  uint64_t buildUs = 0;
  uint32_t loads = 0;
  uint32_t links = 0;
  uint32_t compiles = 0;
  uint64_t textureUs = 0;
  uint32_t textures = 0;
  uint64_t textureBytes = 0;
  uint64_t meshUs = 0;
  uint32_t meshes = 0;
  uint64_t fixedUs = 0;
  uint64_t fixedMaxUs = 0;
  uint32_t fixedDraws = 0;
  uint64_t copyUs = 0;
  uint32_t copies = 0;
  uint64_t swapUs = 0;
};
PresentCosts g_presentCosts;
constexpr uint64_t kSlowPresentUs = 120000;

// Presents so far (render worker), and render targets let go waiting for their delete. vitaGL
// frees a texture's memory at once once nothing references it, and the GPU runs a frame's scenes
// behind the render worker: deleted as soon as its size changed, a copy's target was drawn into by
// a copy pass still queued after its memory went to another allocation, screen pixels over vitaGL's
// pool's own records (two crashes in its allocator: 2635657). So a target waits kRetiredPresents
// presents first.
uint32_t g_presentSerial = 0;
constexpr uint32_t kRetiredPresents = 8;
struct RetiredTarget {
  GLuint texture;
  GLuint framebuffer;
  uint32_t retiredAt;
};
std::deque<RetiredTarget> g_retiredTargets;

// Render worker.
void retire_target(GLuint texture, GLuint framebuffer) noexcept {
  g_retiredTargets.push_back({texture, framebuffer, g_presentSerial});
}

void delete_retired_targets() noexcept {
  while (!g_retiredTargets.empty() && g_presentSerial - g_retiredTargets.front().retiredAt >= kRetiredPresents) {
    const RetiredTarget& target = g_retiredTargets.front();
    if (target.framebuffer != 0) {
      glDeleteFramebuffers(1, &target.framebuffer);
    }
    if (target.texture != 0) {
      glDeleteTextures(1, &target.texture);
    }
    g_retiredTargets.pop_front();
  }
}

// vitaGL's memory by pool, free of total (MiB). The game's heap is in the [heap] lines.
void log_memory(const char* when) {
  constexpr double kMiB = 1024.0 * 1024.0;
  const auto pool = [](vglMemType type) {
    return fmt::format("{:.1f}/{:.1f}", vglMemFree(type) / kMiB, vglMemTotal(type) / kMiB);
  };
  // The system's own, outside vitaGL's pools: GXM makes the framebuffers' render targets from it.
  SceKernelFreeMemorySizeInfo system{};
  system.size = sizeof(system);
  sceKernelGetFreeMemorySize(&system);
  Log.info("{}: vitaGL memory free of total (MiB): VRAM {}, RAM {}, phycont {}, budget {}; the system's free: "
           "user {:.1f}, CDRAM {:.1f}, phycont {:.1f}; textures on the GPU: {} RGBA8 ({:.1f} MiB), {} BC1 ({:.1f} "
           "MiB); not yet uploaded: {} ({:.1f} MiB)",
           when, pool(VGL_MEM_VRAM), pool(VGL_MEM_RAM), pool(VGL_MEM_PHYCONT), pool(VGL_MEM_BUDGET),
           system.size_user / kMiB, system.size_cdram / kMiB, system.size_phycont / kMiB, g_textureCount[0].load(),
           g_textureBytes[0].load() / kMiB, g_textureCount[1].load(), g_textureBytes[1].load() / kMiB,
           g_pendingCount.load(), g_pendingBytes.load() / kMiB);
}

// A frame's draws and their programs' constant rows (ProgramInputs), and its serial, a new one
// each frame (from 1), by which the render worker tells one frame's rows from another's.
struct Frame {
  std::vector<Draw> draws;
  std::vector<std::array<float, 4>> constants;
  uint32_t serial = 0;
};

// The frames: recorded on the FIFO thread, handed over at each frame's end, drawn by the render
// worker's present for that frame, in order.
Frame g_recording{.serial = 1};
std::mutex g_framesMutex;
std::deque<Frame> g_frames;

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
  Mesh(std::vector<Vertex> triangles, MeshKind kind, bool clientMemory, MeshPrimitive primitive) noexcept
  : m_cpu(std::move(triangles)), m_count(static_cast<GLsizei>(m_cpu.size())), m_kind(kind), m_client(clientMemory),
    m_mode(primitive == MeshPrimitive::Lines    ? GL_LINES
           : primitive == MeshPrimitive::Points ? GL_POINTS
                                                : GL_TRIANGLES) {}
  ~Mesh() {
    if (m_vbo != 0) {
      const GLuint vbo = m_vbo;
      gfx::render_worker::enqueue_work([vbo] { glDeleteBuffers(1, &vbo); });
    }
  }

  // Render worker: binds the vertex buffer (made from the CPU copy at the first draw), or for a
  // mesh in client memory unbinds any, its arrays then pointing at base().
  GLsizei bind() noexcept {
    if (m_client) {
      glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else if (m_vbo == 0) {
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

  // Render worker: whether its buffer is made yet (bind makes it; a mesh in client memory is
  // copied at every draw, so never).
  bool uploaded() const noexcept { return m_vbo != 0; }
  GLsizei count() const noexcept { return m_count; }
  MeshKind kind() const noexcept { return m_kind; }
  // What glDrawArrays draws it as: GL_TRIANGLES or GL_LINES.
  GLenum mode() const noexcept { return m_mode; }
  // What its arrays' offsets are from: its vertices in client memory, else the bound buffer's start.
  const uint8_t* base() const noexcept {
    return m_client ? reinterpret_cast<const uint8_t*>(m_cpu.data()) : nullptr;
  }

private:
  std::vector<Vertex> m_cpu;
  GLuint m_vbo = 0;
  GLsizei m_count;
  MeshKind m_kind;
  bool m_client;
  GLenum m_mode;
};

std::shared_ptr<Mesh> make_mesh(std::vector<Vertex> triangles, MeshKind kind, bool clientMemory,
                                MeshPrimitive primitive) noexcept {
  return std::make_shared<Mesh>(std::move(triangles), kind, clientMemory, primitive);
}

class Texture {
public:
  Texture(uint32_t width, uint32_t height, TexelFormat format, std::vector<uint8_t> level0) noexcept
  : m_cpu(std::move(level0)), m_width(width), m_height(height), m_format(format) {
    m_pending = static_cast<int64_t>(m_cpu.size());
    if (m_pending != 0) {
      g_pendingCount.fetch_add(1, std::memory_order_relaxed);
      g_pendingBytes.fetch_add(m_pending, std::memory_order_relaxed);
    }
  }
  ~Texture() {
    settle_pending();
    if (!m_targets.empty()) {
      // A copy texture's targets: retired, the GPU perhaps still drawing into one.
      const int kind = static_cast<int>(m_format);
      for (const Target& target : m_targets) {
        const GLuint texture = target.texture;
        const GLuint framebuffer = target.framebuffer;
        gfx::render_worker::enqueue_work([texture, framebuffer] { retire_target(texture, framebuffer); });
        g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
        g_textureBytes[kind].fetch_sub(target.bytes(), std::memory_order_relaxed);
      }
      return;
    }
    if (m_id != 0) {
      const GLuint id = m_id;
      gfx::render_worker::enqueue_work([id] { glDeleteTextures(1, &id); });
      const int kind = static_cast<int>(m_format);
      g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
    }
  }

  uint64_t bytes() const noexcept {
    const uint64_t width = m_width;
    const uint64_t height = m_height;
    return m_format == TexelFormat::BC1 ? ((width + 3) / 4) * ((height + 3) / 4) * 8 : width * height * 4;
  }

  // Render worker: true when binding it again with `sampler` would change nothing.
  bool holds(const Sampler& sampler) const noexcept { return m_id != 0 && m_samplerSet && sampler == m_sampler; }

  // Render worker: binds it to the active unit (made from the CPU copy at the first draw) and
  // sets its sampler state where it changed.
  void bind(const Sampler& sampler) noexcept {
    if (m_id == 0) {
      const uint64_t start = sceKernelGetProcessTimeWide();
      if (m_width == 0) {
        // A copy texture no copy has filled (its rectangle was empty): transparent black.
        m_width = m_height = 1;
        m_cpu.assign(4, 0);
      }
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
      settle_pending();
      g_presentCosts.textureUs += sceKernelGetProcessTimeWide() - start;
      ++g_presentCosts.textures;
      g_presentCosts.textureBytes += static_cast<uint64_t>(m_bytes);
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

  // Render worker: makes it a `width` x `height` RGBA8 texture with a framebuffer to draw into (an
  // EFB copy's texture, or a copy's scratch) and returns the framebuffer. A destination copied at
  // several sizes (the game's spare buffer serves several effects) keeps a target for each, its
  // texture the latest copy's: draws after a copy bind it, those before keep the one they bound.
  // A size unused a while is retired (retire_target) when the pool is full.
  GLuint render_target(int width, int height) noexcept {
    const int kind = static_cast<int>(m_format);
    if (m_id != 0 && m_targets.empty()) {
      // The stand-in bind made before any copy (a transparent black texel): let go.
      retire_target(m_id, 0);
      g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
      m_bytes = 0;
      m_id = 0;
    }
    std::vector<uint8_t>().swap(m_cpu);
    settle_pending();
    Target* target = nullptr;
    for (Target& t : m_targets) {
      if (t.width == width && t.height == height) {
        target = &t;
        break;
      }
    }
    if (target == nullptr) {
      if (m_targets.size() >= kTargetsPerTexture) {
        // The least recently used size, if no copy of the last presents used it (whatever is
        // younger may be in a queued scene; it is retired anyway, its delete waits).
        auto oldest = std::min_element(m_targets.begin(), m_targets.end(),
                                       [](const Target& a, const Target& b) { return a.lastUsed < b.lastUsed; });
        if (g_presentSerial - oldest->lastUsed >= 2) {
          retire_target(oldest->texture, oldest->framebuffer);
          g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
          g_textureBytes[kind].fetch_sub(oldest->bytes(), std::memory_order_relaxed);
          m_targets.erase(oldest);
        }
      }
      Target made{.width = width, .height = height};
      glGenTextures(1, &made.texture);
      glBindTexture(GL_TEXTURE_2D, made.texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glGenFramebuffers(1, &made.framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, made.framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, made.texture, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      g_textureCount[kind].fetch_add(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_add(made.bytes(), std::memory_order_relaxed);
      g_renderTargets.fetch_add(1, std::memory_order_relaxed);
      m_targets.push_back(made);
      target = &m_targets.back();
    }
    target->scenes = target->lastUsed == g_presentSerial ? target->scenes + 1 : 1;
    target->lastUsed = g_presentSerial;
    if (m_id != target->texture) {
      // Another GL texture: its sampler state is its own.
      m_id = target->texture;
      m_samplerSet = false;
    }
    m_width = static_cast<uint32_t>(width);
    m_height = static_cast<uint32_t>(height);
    return target->framebuffer;
  }

  // Render worker: the scenes its `width` x `height` target has been drawn in this present (a copy
  // texture's or the copies' scratch: copy_on_gpu keeps them within kFramebufferScenesPerFrame).
  int scenes(int width, int height) const noexcept {
    for (const Target& t : m_targets) {
      if (t.width == width && t.height == height) {
        return t.lastUsed == g_presentSerial ? t.scenes : 0;
      }
    }
    return 0;
  }

  GLuint id() const noexcept { return m_id; }
  // Its size now (a copy texture's: its latest copy's, in the screen's pixels), and how many render
  // targets it keeps (for the log).
  uint32_t width() const noexcept { return m_width; }
  uint32_t height() const noexcept { return m_height; }
  size_t targets() const noexcept { return m_targets.size(); }
  // For the log: its size, then each render target's, how many presents ago a copy drew into it and
  // in how many scenes then, and which is bound.
  std::string describe() const {
    std::string text = fmt::format("{} {}x{}", static_cast<const void*>(this), m_width, m_height);
    for (const Target& t : m_targets) {
      text += fmt::format("; target {}x{} copied {} presents ago in {} scenes{}", t.width, t.height,
                          g_presentSerial - t.lastUsed, t.scenes, t.texture == m_id ? " (bound)" : "");
    }
    return text;
  }

private:
  void settle_pending() noexcept {
    if (m_pending != 0) {
      g_pendingCount.fetch_sub(1, std::memory_order_relaxed);
      g_pendingBytes.fetch_sub(m_pending, std::memory_order_relaxed);
      m_pending = 0;
    }
  }

  std::vector<uint8_t> m_cpu;
  int64_t m_pending = 0; // bytes counted in g_pendingBytes, until the upload
  uint32_t m_width;
  uint32_t m_height;
  TexelFormat m_format;
  // A copy texture's render targets, one a size (render_target).
  struct Target {
    int width = 0;
    int height = 0;
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32_t lastUsed = 0; // g_presentSerial at its last copy
    int scenes = 0;        // the scenes drawn into it in that present
    int64_t bytes() const noexcept { return static_cast<int64_t>(width) * height * 4; }
  };
  static constexpr size_t kTargetsPerTexture = 4;

  GLuint m_id = 0;
  int64_t m_bytes = 0;
  Sampler m_sampler{};
  bool m_samplerSet = false;
  std::vector<Target> m_targets;
};

std::shared_ptr<Texture> make_texture(uint32_t width, uint32_t height, TexelFormat format,
                                      std::vector<uint8_t> level0) noexcept {
  return std::make_shared<Texture>(width, height, format, std::move(level0));
}

std::shared_ptr<Texture> make_copy_texture() noexcept {
  return std::make_shared<Texture>(0, 0, TexelFormat::RGBA8, std::vector<uint8_t>());
}

uint64_t texture_bytes(const Texture& texture) noexcept { return texture.bytes(); }

namespace {
// For the log: generated programs built, failed, and the time spent compiling on the render worker
// (a compile there blocks the frame: the synchronous fallback); of them those loaded from the
// card, and those compiled on the background compiler (its time, and the render worker's to link
// them).
struct ProgramStats {
  uint32_t compiled = 0; // built, from the compiler or the card
  uint32_t failed = 0;
  double compileMs = 0.0;
  uint32_t loaded = 0; // of them, loaded from the card
  double loadMs = 0.0;
  uint32_t background = 0; // of them, compiled in the background
  double backgroundMs = 0.0;
  double linkMs = 0.0;
  uint32_t fallbacks = 0; // background compiles that failed or did not link, compiled again here
  uint32_t failuresLogged = 0;
};
ProgramStats g_programStats;

// For the log: program draws' uniform arrays sent, and left alone as the program held them already;
// textures bound, and left alone as their unit held them already (render worker, since the last
// report).
struct UniformStats {
  struct Side {
    uint64_t uploaded = 0;
    uint64_t skipped = 0;
  };
  Side vertex;
  Side fragment;
  uint64_t textureBinds = 0;
  uint64_t texturesKept = 0;
};
UniformStats g_uniformStats;

// Where the render worker's present goes, for the log: one frame in kTimedEvery is timed, section
// by section (the timer costs about 0.7 microseconds a call, too much for every draw of every
// frame). Each mark ends a section where the last one ended, so the sections add up to the present.
enum Section {
  kSetup, // the clear, ImGui's frame, taking the frame, draw_world's state reset
  kBuild, // programs' readiness checks, and the builds (compiles, loads) made in the frame
  kProgramSwitch,   // glUseProgram and the generic arrays turned on and off
  kProgramUpload,   // the mesh's buffer bound, made and filled at its first draw
  kProgramPointers, // the attribute pointers
  kProgramCompare,  // the draw's uniform arrays against what the program holds
  kProgramSend,     // glUniform4fv where they differ
  kProgramTextures,
  kProgramState,
  kProgramDraw, // glDrawArrays
  kFixedSwitch, // back to the fixed-function arrays
  kFixedUpload,
  kFixedPointers,
  kFixedUnits, // texture units: enables, binds, env modes, texture matrices
  kFixedMatrices,
  kFixedState,
  kFixedDraw,
  kCopy,    // EFB copies: the screen read back (the GPU waited for), converted and uploaded
  kFinish,  // draw_world's state reset for the overlay, the frame handed to the reaper
  kOther,   // the stats log
  kOverlay, // ImGui
  kSwap,    // vglSwapBuffers
  kSectionCount
};
constexpr const char* kSectionNames[kSectionCount] = {
    "setup",  "build",  "switch",   "upload",   "pointers", "compare", "send",  "textures", "state",   "draw", "switch",
    "upload", "pointers", "units", "matrices", "state",    "draw",    "copies", "finish", "other",   "overlay", "swap"};
constexpr uint32_t kTimedEvery = 8;

// What made a draw's mesh (make_mesh), and the two draw paths, for the uploads' line.
constexpr const char* kMeshKindNames[static_cast<int>(MeshKind::Count)] = {"lists", "skinned", "immediate",
                                                                           "indexed", "other"};
enum DrawPath { kPathProgram, kPathFixed, kPathCount };

struct SectionClock {
  bool on = false; // timing this present
  uint64_t last = 0;
  uint64_t us[kSectionCount] = {};
  uint32_t frames = 0; // timed presents since the last report
  uint64_t draws = 0;
  uint64_t programDraws = 0;
  uint64_t calls = 0; // timer calls, for their cost
  uint64_t reportStart = 0;
  uint32_t presents = 0;
  double callUs = -1.0; // one timer call, measured once
  // Timed presents' draws by path and mesh kind: draws, buffers made and their bytes, the upload
  // section's time.
  struct Uploads {
    uint64_t draws = 0;
    uint64_t made = 0;
    uint64_t bytes = 0;
    uint64_t us = 0;
  };
  Uploads uploads[kPathCount][static_cast<int>(MeshKind::Count)] = {};
  // Timed presents' fixed-function draws: waiting for their program, or without one.
  uint64_t fixedPending = 0;
  uint64_t fixedUnsupported = 0;
  // The largest mesh made for a fixed-function draw since the last report.
  uint32_t largestFixedVertices = 0;
  MeshKind largestFixedKind = MeshKind::Other;

  void start() noexcept {
    if (on) {
      last = sceKernelGetProcessTimeWide();
      ++calls;
    }
  }
  // The section's time (0 when this present is not timed).
  uint64_t mark(Section section) noexcept {
    if (!on) {
      return 0;
    }
    const uint64_t now = sceKernelGetProcessTimeWide();
    const uint64_t spent = now - last;
    us[section] += spent;
    last = now;
    ++calls;
    return spent;
  }
};
SectionClock g_clock;

// vitaGL's uniform locations are its uniform records' addresses negated, so any value but -1
// (not found) is one; attribute locations are plain indices.
constexpr GLint kNoUniform = -1;

// The vertex attributes a generated program may declare (gx/vita_tev.cpp), by name.
enum ProgramAttr { kAttrPos, kAttrRaw, kAttrShade, kAttrNrm, kAttrUv0, kAttrCount = kAttrUv0 + kTexCoords };
constexpr const char* kAttrNames[kAttrCount] = {"aPos", "aRaw", "aShade", "aNrm", "aUv0", "aUv1", "aUv2", "aUv3"};

// Compiles Cg source. The GL_CG_* types matter: vitaGL takes GL_VERTEX_SHADER and
// GL_FRAGMENT_SHADER source for GLSL, translates it at glLinkProgram (VGL_MODE_POSTPONED), and a
// Cg source that fails there leaves an invalid GXM program that glLinkProgram reads (the crash of
// milestone 2's first program). With the Cg types the compile happens here, and its status holds.
// (Calling vitashark directly for the GXP failed every compile with "fatal internal error", even
// after a glCompileShader had started the compiler: runs of 685daf0 and a48830e.)
GLuint compile_cg(GLenum type, const std::string& source, std::string& error) {
  const GLuint shader = glCreateShader(type);
  const char* text = source.c_str();
  const auto length = static_cast<GLint>(source.size());
  glShaderSource(shader, 1, &text, &length);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    error = log;
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

// Generated programs on the memory card, by their source's hash (vita_tev.cpp): vitaGL's shader
// cache does not keep these Cg programs (a session compiled the same 61 again, 23 s of hitches),
// so a program is compiled once and loaded after. A file holds a header (magic, both sources'
// lengths, which a hash collision would have to match as well, the binary's format and size) and
// the program binary from glGetProgramBinary, which glProgramBinary takes back: vitaGL's own
// format, both shaders' GXPs with their parameter tables, relinked on load without the compiler.
constexpr uint32_t kCacheMagic = 0x32505456; // "VTP2"
constexpr uint32_t kMaxBinary = 1u << 20;
std::string g_programCacheDir;

std::string cache_path(uint64_t key) { return fmt::format("{}/{:016x}.gxp", g_programCacheDir, key); }

bool load_cached(uint64_t key, uint32_t vertexLength, uint32_t fragmentLength, GLenum& format,
                 std::vector<uint8_t>& binary) {
  std::FILE* file = std::fopen(cache_path(key).c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  uint32_t header[5] = {};
  bool ok = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == kCacheMagic &&
            header[1] == vertexLength && header[2] == fragmentLength && header[4] != 0 && header[4] < kMaxBinary;
  if (ok) {
    format = header[3];
    binary.resize(header[4]);
    ok = std::fread(binary.data(), binary.size(), 1, file) == 1;
  }
  std::fclose(file);
  return ok;
}

// A linked program's binary (render worker: GL calls). vitaGL's glGetProgramBinary writes the
// whole binary whatever the buffer's size, so the buffer is the length vitaGL gives first, with
// room to spare.
bool program_binary(GLuint program, GLenum& format, std::vector<uint8_t>& binary) {
  GLint length = 0;
  glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
  if (length <= 0 || static_cast<uint32_t>(length) >= kMaxBinary) {
    return false;
  }
  binary.resize(static_cast<size_t>(length) + 4096);
  GLsizei written = 0;
  glGetProgramBinary(program, static_cast<GLsizei>(binary.size()), &written, &format, binary.data());
  if (written <= 0 || static_cast<size_t>(written) > binary.size()) {
    return false;
  }
  binary.resize(static_cast<size_t>(written));
  return true;
}

// A binary to the card (the card thread), written aside and renamed, so a file cut short (the
// power, a crash) is never read.
void write_cached(uint64_t key, uint32_t vertexLength, uint32_t fragmentLength, GLenum format,
                  const std::vector<uint8_t>& binary) {
  const std::string path = cache_path(key);
  const std::string temp = path + ".tmp";
  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (file == nullptr) {
    return;
  }
  const uint32_t header[5] = {kCacheMagic, vertexLength, fragmentLength, format, static_cast<uint32_t>(binary.size())};
  const bool ok = std::fwrite(header, sizeof(header), 1, file) == 1 &&
                  std::fwrite(binary.data(), binary.size(), 1, file) == 1;
  std::fclose(file);
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(temp, path, ec);
  } else {
    std::filesystem::remove(temp, ec);
  }
}

// The card's reads and writes of program binaries, on a thread of their own. On the render worker
// a load's read, and a compile's write with its rename, held the frame: a compiled program's link
// took 426 ms with its write (4abc21d's run), and a room bringing dozens of programs read them
// there. A program's binary is read as soon as the FIFO thread makes the program (Program's
// constructor), so it is in memory by its first draws; the render worker only gives it to vitaGL.
enum CardReadState : int { kCardReading, kCardRead, kCardAbsent };

struct CardRead {
  uint64_t key = 0;
  uint32_t vertexLength = 0;
  uint32_t fragmentLength = 0;
  GLenum format = 0;
  std::vector<uint8_t> binary;
  std::atomic<int> state{kCardReading};
};

class CardIo {
public:
  void read(std::shared_ptr<CardRead> request) noexcept {
    Job job;
    job.kind = Job::Read;
    job.read = std::move(request);
    push(std::move(job));
  }

  void write(uint64_t key, uint32_t vertexLength, uint32_t fragmentLength, GLenum format,
             std::vector<uint8_t> binary) noexcept {
    Job job;
    job.kind = Job::Write;
    job.key = key;
    job.vertexLength = vertexLength;
    job.fragmentLength = fragmentLength;
    job.format = format;
    job.binary = std::move(binary);
    push(std::move(job));
  }

  // A binary that no longer links (another vitaGL, a damaged card): a compile next time.
  void remove(uint64_t key) noexcept {
    Job job;
    job.kind = Job::Remove;
    job.key = key;
    push(std::move(job));
  }

private:
  struct Job {
    enum Kind : uint8_t { Read, Write, Remove } kind = Read;
    std::shared_ptr<CardRead> read;
    uint64_t key = 0;
    uint32_t vertexLength = 0;
    uint32_t fragmentLength = 0;
    GLenum format = 0;
    std::vector<uint8_t> binary;
  };

  void push(Job job) noexcept {
    {
      std::lock_guard lock{m_mutex};
      if (!m_started) {
        m_started = start();
      }
      if (!m_started) {
        // No thread: done here, as before the card thread.
        run_job(job);
        return;
      }
      m_jobs.push_back(std::move(job));
    }
    m_ready.notify_one();
  }

  bool start() noexcept {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const auto run = [](void* self) -> void* {
      static_cast<CardIo*>(self)->run();
      return nullptr;
    };
    const bool ok = pthread_create(&thread, &attr, run, this) == 0;
    pthread_attr_destroy(&attr);
    return ok;
  }

  static void run_job(Job& job) noexcept {
    switch (job.kind) {
    case Job::Read: {
      CardRead& read = *job.read;
      const bool ok = load_cached(read.key, read.vertexLength, read.fragmentLength, read.format, read.binary);
      if (!ok) {
        std::vector<uint8_t>().swap(read.binary);
      }
      read.state.store(ok ? kCardRead : kCardAbsent, std::memory_order_release);
      break;
    }
    case Job::Write:
      write_cached(job.key, job.vertexLength, job.fragmentLength, job.format, job.binary);
      break;
    case Job::Remove: {
      std::error_code ec;
      std::filesystem::remove(cache_path(job.key), ec);
      break;
    }
    }
  }

  void run() noexcept {
    for (;;) {
      Job job;
      {
        std::unique_lock lock{m_mutex};
        m_ready.wait(lock, [this] { return !m_jobs.empty(); });
        job = std::move(m_jobs.front());
        m_jobs.pop_front();
      }
      run_job(job);
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::deque<Job> m_jobs;
  bool m_started = false;
};
CardIo g_cardIo;

// The Cg compiler on a thread of its own. A compile took the render worker 1 to 3.8 s, the frame
// frozen that long (an enemy's death, an effect seen for the first time: 2635657's run), so a
// program not on the card is compiled in the background to its GXP binaries, and the render worker
// only registers and links them (vglShaderGxpBinary, a few milliseconds, as a card load); its
// draws go on fixed-function meanwhile.
//
// vitashark keeps a compile's state in globals (its input, options and output), and vitaGL compiles
// through it too: its fixed-function shaders, at a draw needing one its cache does not hold. So
// every compile holds g_compilerMutex from shark_compile_shader_extended to shark_clear_output; the
// linker wraps both (CMakeLists: --wrap), vitaGL's calls as well as ours (the wrappers at the end
// of this file). A thread holding it already (a vitaGL compile not cleared) does not lock again.
std::mutex g_compilerMutex;
std::atomic<std::thread::id> g_compilerOwner;

void compiler_lock() noexcept {
  if (g_compilerOwner.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    return;
  }
  g_compilerMutex.lock();
  g_compilerOwner.store(std::this_thread::get_id(), std::memory_order_release);
}

void compiler_unlock() noexcept {
  if (g_compilerOwner.load(std::memory_order_acquire) != std::this_thread::get_id()) {
    return;
  }
  g_compilerOwner.store(std::thread::id{}, std::memory_order_release);
  g_compilerMutex.unlock();
}

// Started here (initialize), not by vitaGL: vitaGL starts it with its own pools' allocator
// (shark_set_allocators(vglMalloc, vglFree)), which another thread must not use, and vitashark
// keeps the allocator it started with. With the C library's (thread-safe), an instance vitaGL
// started first ended. vitaGL's own start later finds it running (shark_init does nothing then).
bool g_compilerReady = false;

void start_compiler() noexcept {
  shark_end();
  shark_set_allocators(malloc, free);
  g_compilerReady = shark_init(nullptr) >= 0 || shark_init("ur0:data/external/libshacccg.suprx") >= 0;
  if (g_compilerReady) {
    Log.info("the Cg compiler is started: programs compile in the background");
  } else {
    Log.warn("the Cg compiler did not start: programs compile on the render worker");
  }
}

// One program's compile: its sources in, its GXP binaries (or the compiler's messages) out.
struct CompileJob {
  std::string vertexSource;
  std::string fragmentSource;
  std::vector<uint8_t> vertexGxp;
  std::vector<uint8_t> fragmentGxp;
  std::string error;
  double ms = 0.0;
  std::atomic<bool> done{false};
};

// A Cg source to its GXP binary. vitashark takes the source's length in `size` (and gives the
// binary's back in it): passed 0, the compiler failed every time with "fatal internal error"
// (685daf0, a48830e). The options are vitaGL's (SHARK_OPT_FAST, fast math and integers).
bool compile_gxp(const std::string& source, shark_type type, std::vector<uint8_t>& gxp, std::string& error) noexcept {
  uint32_t size = static_cast<uint32_t>(source.size());
  const SceGxmProgram* program =
      shark_compile_shader_extended(source.c_str(), &size, type, SHARK_OPT_FAST, SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
  if (program != nullptr && size != 0) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(program);
    gxp.assign(bytes, bytes + size);
  } else {
    error = type == SHARK_VERTEX_SHADER ? "the vertex program did not compile" : "the fragment program did not compile";
    if (const SceShaccCgCompileOutput* output = shark_get_internal_compile_output(); output != nullptr) {
      for (int32_t i = 0; i < output->diagnosticCount && i < 4; ++i) {
        const SceShaccCgDiagnosticMessage& message = output->diagnostics[i];
        error += fmt::format("; line {}: {}", message.location != nullptr ? message.location->lineNumber : 0u,
                             message.message != nullptr ? message.message : "");
      }
    }
  }
  shark_clear_output();
  return !gxp.empty();
}

// The background compiler: one thread, its jobs in order. Its stack is the compiler's (it runs
// deep: 32 KiB overflowed), its priority below the game's threads, so it takes the time they leave.
class BackgroundCompiler {
public:
  void submit(std::shared_ptr<CompileJob> job) noexcept {
    {
      std::lock_guard lock{m_mutex};
      if (!m_started) {
        m_started = start();
      }
      if (!m_started) {
        job->error = "no thread for the compiler";
        job->done.store(true, std::memory_order_release);
        return;
      }
      m_jobs.push_back(std::move(job));
    }
    m_ready.notify_one();
  }

  size_t waiting() noexcept {
    std::lock_guard lock{m_mutex};
    return m_jobs.size();
  }

private:
  bool start() noexcept {
    constexpr size_t kCompilerStack = 2 * 1024 * 1024;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kCompilerStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const auto run = [](void* self) -> void* {
      static_cast<BackgroundCompiler*>(self)->run();
      return nullptr;
    };
    const bool ok = pthread_create(&thread, &attr, run, this) == 0;
    pthread_attr_destroy(&attr);
    return ok;
  }

  void run() noexcept {
    const SceUID self = sceKernelGetThreadId();
    sceKernelChangeThreadPriority(self, std::min(sceKernelGetThreadCurrentPriority() + 16, 191));
    for (;;) {
      std::shared_ptr<CompileJob> job;
      {
        std::unique_lock lock{m_mutex};
        m_ready.wait(lock, [this] { return !m_jobs.empty(); });
        job = std::move(m_jobs.front());
        m_jobs.pop_front();
      }
      const auto start = std::chrono::steady_clock::now();
      if (compile_gxp(job->vertexSource, SHARK_VERTEX_SHADER, job->vertexGxp, job->error)) {
        compile_gxp(job->fragmentSource, SHARK_FRAGMENT_SHADER, job->fragmentGxp, job->error);
      }
      job->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      job->done.store(true, std::memory_order_release);
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::deque<std::shared_ptr<CompileJob>> m_jobs;
  bool m_started = false;
};
BackgroundCompiler g_compiler;
} // namespace

class Program {
public:
  Program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept
  : m_vertexSource(std::move(vertexSource)), m_fragmentSource(std::move(fragmentSource)), m_key(key) {
    // Its binary from the card, read on the card thread from now on (CardIo).
    m_read = std::make_shared<CardRead>();
    m_read->key = key;
    m_read->vertexLength = static_cast<uint32_t>(m_vertexSource.size());
    m_read->fragmentLength = static_cast<uint32_t>(m_fragmentSource.size());
    g_cardIo.read(m_read);
  }
  ~Program() {
    if (m_id != 0) {
      const GLuint id = m_id;
      gfx::render_worker::enqueue_work([id] { glDeleteProgram(id); });
    }
  }

  // Render worker: true once linked. A program on the card is loaded, once the card thread has read
  // it, while the frame's budget allows (a few milliseconds each). One that is not goes to the
  // background compiler, and is linked here once its binaries are made, on the same budget (it
  // costs about as much); its draws go fixed-function meanwhile, as they do while its read is under
  // way. Without the background compiler (it did not start), it is compiled here, one a frame, the
  // frame waiting the compile out (a second or more).
  bool ready(int& compileBudget, int& loadBudget) noexcept {
    if (m_state == State::Pending) {
      const int read = m_read->state.load(std::memory_order_acquire);
      if (read == kCardReading) {
        return false;
      }
      if (read == kCardRead) {
        if (loadBudget > 0) {
          --loadBudget;
          build_on_compile_stack();
        }
      } else if (g_compilerReady) {
        m_job = std::make_shared<CompileJob>();
        m_job->vertexSource = m_vertexSource;
        m_job->fragmentSource = m_fragmentSource;
        g_compiler.submit(m_job);
        m_state = State::Compiling;
      } else if (compileBudget > 0) {
        --compileBudget;
        build_on_compile_stack();
      }
    } else if (m_state == State::Compiling && loadBudget > 0 && m_job->done.load(std::memory_order_acquire)) {
      --loadBudget;
      build_on_compile_stack();
    }
    return m_state == State::Ready;
  }

  GLuint id() const noexcept { return m_id; }
  GLint attr(int index) const noexcept { return m_attrs[index]; }
  GLint samplers[kSamplers] = {-1, -1, -1, -1, -1, -1, -1, -1};

  // Render worker, this program in use: sets its constants unless they are the rows it was sent
  // last. vitaGL keeps each program's uniforms, and sends a side's uniform buffer at a draw only
  // after a glUniform call (or glUseProgram) marked that side, so an unchanged side costs nothing.
  // The FIFO thread points a draw that repeats its program's last constants in a frame at the same
  // rows; and a program ready at a frame's first draw of it stays so all that frame (a build is
  // tried there, the budget only shrinks), so it is sent every row change in order.
  void set_constants(const ProgramInputs& inputs, const Frame& frame) noexcept {
    upload(m_vertexLocation, m_vertexSent, frame, inputs.vertexAt, inputs.vertexCount, g_uniformStats.vertex);
    upload(m_constantsLocation, m_constantsSent, frame, inputs.constantsAt, inputs.constantCount,
           g_uniformStats.fragment);
  }

  // Render worker, this program in use: its constants from `rows` (a pass of this file's own, not
  // a frame's draw), which a frame's draw sends over again after.
  void send_constants(const float (*rows)[4], int count) noexcept {
    if (m_constantsLocation != kNoUniform) {
      glUniform4fv(m_constantsLocation, count, rows[0]);
    }
    m_constantsSent = {};
  }

private:
  enum class State : uint8_t { Pending, Compiling, Ready, Failed };

  // Rows sent to a uniform array last: their frame's serial (0: none yet) and where they start.
  struct Sent {
    uint32_t serial = 0;
    uint32_t at = 0;
  };

  static void upload(GLint location, Sent& sent, const Frame& frame, uint32_t at, int count,
                     UniformStats::Side& stats) noexcept {
    if (location == kNoUniform || count == 0 || at + count > frame.constants.size()) {
      return;
    }
    const bool same = sent.serial == frame.serial && sent.at == at;
    g_clock.mark(kProgramCompare);
    if (same) {
      ++stats.skipped;
      return;
    }
    sent = {frame.serial, at};
    glUniform4fv(location, count, frame.constants[at].data());
    g_clock.mark(kProgramSend);
    ++stats.uploaded;
  }

  // The Cg compiler (SceShaccCg) runs deep: it overflowed the render worker's 32 KiB stack (what
  // VitaSDK gives a std::thread) at the 18th program of the first run. So a build runs on a thread
  // of its own with a stack it fits in, while the render worker waits for it: the GL calls stay one
  // at a time, and vitaGL keeps no per-thread context.
  void build_on_compile_stack() noexcept {
    const uint64_t start = sceKernelGetProcessTimeWide();
    struct Timed {
      uint64_t start;
      ~Timed() { g_presentCosts.buildUs += sceKernelGetProcessTimeWide() - start; }
    } timed{start};
    constexpr size_t kCompileStack = 2 * 1024 * 1024;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kCompileStack);
    pthread_t thread;
    const auto run = [](void* self) -> void* {
      static_cast<Program*>(self)->build();
      return nullptr;
    };
    if (pthread_create(&thread, &attr, run, this) == 0) {
      pthread_join(thread, nullptr);
    } else {
      Log.error("no thread for a program's compile: it stays fixed-function");
      m_state = State::Failed;
      ++g_programStats.failed;
    }
    pthread_attr_destroy(&attr);
  }

  // A linked program from its two shaders (deleted after: the program keeps them).
  void link(GLuint vs, GLuint fs, std::string& error) noexcept {
    // No glBindAttribLocation: glLinkProgram looks every bound name up in the compiled program
    // and passes the result on unchecked, so a name the compiler dropped would crash it. The
    // locations are asked for after the link instead.
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_TRUE) {
      m_id = program;
    } else {
      error = "the link failed";
      glDeleteProgram(program);
    }
  }

  // The sources compiled here, through vitaGL's glCompileShader with the Cg types, and linked.
  void compile_here(std::string& error) noexcept {
    const GLuint vs = compile_cg(GL_CG_VERTEX_SHADER_EXT, m_vertexSource, error);
    const GLuint fs = vs != 0 ? compile_cg(GL_CG_FRAGMENT_SHADER_EXT, m_fragmentSource, error) : 0;
    if (vs != 0 && fs != 0) {
      link(vs, fs, error);
    }
    if (vs != 0) {
      glDeleteShader(vs);
    }
    if (fs != 0) {
      glDeleteShader(fs);
    }
  }

  // The background compiler's GXP binaries, registered with vitaGL's shader patcher
  // (vglShaderGxpBinary, as its compile does) and linked.
  bool link_gxp(const CompileJob& job) noexcept {
    const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    vglShaderGxpBinary(1, &vs, job.vertexGxp.data(), static_cast<GLsizei>(job.vertexGxp.size()));
    const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    vglShaderGxpBinary(1, &fs, job.fragmentGxp.data(), static_cast<GLsizei>(job.fragmentGxp.size()));
    std::string error;
    link(vs, fs, error);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return m_id != 0;
  }

  void build() noexcept {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const auto vertexLength = static_cast<uint32_t>(m_vertexSource.size());
    const auto fragmentLength = static_cast<uint32_t>(m_fragmentSource.size());
    bool cached = false;
    bool background = false;
    if (m_job) {
      // Compiled in the background. A compile that failed there, or binaries that do not link, are
      // compiled here after all (the frame waits, as before the background compiler).
      if (m_job->error.empty() && !m_job->vertexGxp.empty() && !m_job->fragmentGxp.empty() && link_gxp(*m_job)) {
        background = true;
        ++g_programStats.background;
        g_programStats.backgroundMs += m_job->ms;
      } else {
        ++g_programStats.fallbacks;
        if (g_programStats.failuresLogged++ < 3) {
          Log.warn("a program compiled in the background did not link ({}): compiled on the render worker",
                   m_job->error.empty() ? "its binaries" : m_job->error);
        }
        compile_here(error);
      }
      m_job.reset();
    } else if (m_read->state.load(std::memory_order_acquire) == kCardRead) {
      // The card thread's read: glProgramBinary rebuilds both shaders from their GXPs and links them.
      cached = true;
      const GLuint program = glCreateProgram();
      glProgramBinary(program, m_read->format, m_read->binary.data(), static_cast<GLsizei>(m_read->binary.size()));
      GLint ok = GL_FALSE;
      glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (ok == GL_TRUE) {
        m_id = program;
      } else {
        glDeleteProgram(program);
      }
    } else {
      compile_here(error);
    }
    m_read.reset();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (cached) {
      g_programStats.loadMs += ms;
      ++g_programStats.loaded;
      ++g_presentCosts.loads;
      if (m_id == 0) {
        g_cardIo.remove(m_key);
        error = "its binary from the card did not link (removed)";
      }
    } else {
      // Its binary to the card, written on the card thread.
      GLenum format = 0;
      std::vector<uint8_t> binary;
      if (m_id != 0 && program_binary(m_id, format, binary)) {
        g_cardIo.write(m_key, vertexLength, fragmentLength, format, std::move(binary));
      }
      (background ? g_programStats.linkMs : g_programStats.compileMs) +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      ++(background ? g_presentCosts.links : g_presentCosts.compiles);
    }
    if (m_id == 0) {
      m_state = State::Failed;
      ++g_programStats.failed;
      if (g_programStats.failuresLogged++ < 3) {
        Log.error("a generated program did not build: {}\n-- vertex --\n{}\n-- fragment --\n{}", error,
                  m_vertexSource, m_fragmentSource);
      }
    } else {
      for (int i = 0; i < kAttrCount; ++i) {
        m_attrs[i] = glGetAttribLocation(m_id, kAttrNames[i]);
      }
      m_vertexLocation = glGetUniformLocation(m_id, "uVert");
      m_constantsLocation = glGetUniformLocation(m_id, "uConst");
      static constexpr const char* kSamplerNames[kSamplers] = {"uTex0", "uTex1", "uTex2", "uTex3",
                                                               "uTex4", "uTex5", "uTex6", "uTex7"};
      glUseProgram(m_id);
      for (int i = 0; i < kSamplers; ++i) {
        samplers[i] = glGetUniformLocation(m_id, kSamplerNames[i]);
        if (samplers[i] != kNoUniform) {
          glUniform1i(samplers[i], i);
        }
      }
      m_state = State::Ready;
      ++g_programStats.compiled;
    }
    std::string().swap(m_vertexSource);
    std::string().swap(m_fragmentSource);
  }

  std::string m_vertexSource;
  std::string m_fragmentSource;
  uint64_t m_key;
  std::shared_ptr<CardRead> m_read;   // its binary's read from the card, till built
  std::shared_ptr<CompileJob> m_job; // its background compile, while State::Compiling
  GLuint m_id = 0;
  GLint m_attrs[kAttrCount] = {-1, -1, -1, -1, -1, -1, -1, -1};
  GLint m_vertexLocation = kNoUniform;
  GLint m_constantsLocation = kNoUniform;
  Sent m_vertexSent;
  Sent m_constantsSent;
  State m_state = State::Pending;
};

std::shared_ptr<Program> make_program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept {
  return std::make_shared<Program>(std::move(vertexSource), std::move(fragmentSource), key);
}

void record(Draw draw) noexcept { g_recording.draws.push_back(std::move(draw)); }

uint32_t record_constants(const float (*rows)[4], int count) noexcept {
  const auto at = static_cast<uint32_t>(g_recording.constants.size());
  for (int i = 0; i < count; ++i) {
    g_recording.constants.push_back({rows[i][0], rows[i][1], rows[i][2], rows[i][3]});
  }
  return at;
}

const float* recorded_constants(uint32_t at) noexcept { return g_recording.constants[at].data(); }

uint32_t recording_serial() noexcept { return g_recording.serial; }

void end_frame_draws() noexcept {
  const size_t drawn = g_recording.draws.size();
  const size_t rows = g_recording.constants.size();
  const uint32_t serial = g_recording.serial;
  {
    std::lock_guard lock{g_framesMutex};
    g_frames.push_back(std::move(g_recording));
    // A frame the render worker never presented must not hold on to its meshes for good.
    while (g_frames.size() > 3) {
      g_frames.pop_front();
    }
  }
  g_recording = {};
  g_recording.serial = serial + 1 != 0 ? serial + 1 : 1;
  g_recording.draws.reserve(drawn);
  g_recording.constants.reserve(rows);
}

namespace {
// Render worker: the oldest frame handed over and not yet drawn.
Frame take_frame() {
  std::lock_guard lock{g_framesMutex};
  if (g_frames.empty()) {
    return {};
  }
  Frame frame = std::move(g_frames.front());
  g_frames.pop_front();
  return frame;
}

// Frames drawn, destroyed on a thread of their own: a heavy frame's 1700 draws took the render
// worker 4 ms to destroy (their meshes, textures and programs released, the meshes made for that
// frame alone deleted). A mesh's or texture's destructor queues its GL delete for the render
// worker, which runs it between presents.
class FrameReaper {
public:
  void reap(Frame frame) noexcept {
    {
      std::lock_guard lock{m_mutex};
      if (!m_started) {
        m_started = true;
        std::thread([this] { run(); }).detach();
      }
      m_frames.push_back(std::move(frame));
    }
    m_ready.notify_one();
  }

private:
  void run() noexcept {
    for (;;) {
      Frame frame;
      {
        std::unique_lock lock{m_mutex};
        m_ready.wait(lock, [this] { return !m_frames.empty(); });
        frame = std::move(m_frames.front());
        m_frames.pop_front();
      }
      // `frame` is destroyed here, outside the lock.
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::deque<Frame> m_frames;
  bool m_started = false;
};
FrameReaper g_reaper;

// For the log: presents and draws since the last report (render worker).
struct PresentStats {
  uint32_t presents = 0;
  uint32_t framesWithDraws = 0;
  uint64_t draws = 0;
  // Draws with their generated program, and draws whose program was not ready (fixed-function).
  uint64_t programDraws = 0;
  uint64_t programPending = 0;
  // EFB copies made on the GPU, those left for the pass's program to be built, the screen pixels
  // copied, and the render worker's time for them (the GPU's is not in it).
  uint32_t copies = 0;
  uint32_t copiesWaiting = 0;
  uint64_t copyPixels = 0;
  uint64_t copyUs = 0;
  // The most draws in one of the display's scenes (an EFB copy ends one): what GXM's ring buffers
  // hold until the scene is drawn (initialize).
  uint32_t maxSceneDraws = 0;
  // The most scenes a frame drew into the display and into one framebuffer (a copy's scratch or
  // texture), and the copies left for the next frame to keep within them (draw_world, copy_on_gpu).
  uint32_t maxDisplayScenes = 0;
  uint32_t maxFramebufferScenes = 0;
  uint32_t copiesOverDisplayScenes = 0;
  uint32_t copiesOverFramebufferScenes = 0;
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

// Generated programs compiled a frame at most: vitaGL's Cg compiler takes from a few to a hundred
// milliseconds a program; one compiled in a session before loads from the card in a few.
constexpr int kCompilesPerFrame = 1;
constexpr int kLoadsPerFrame = 8;

// The texture each unit holds, in a frame's draws (render worker): binding one again is skipped.
struct BoundTextures {
  const Texture* units[kSamplers] = {};

  // Binds `texture` to `unit`, which must be the active unit when `active` (else it is made so).
  void bind(int unit, Texture& texture, const Sampler& sampler, bool active) noexcept {
    if (units[unit] == &texture && texture.holds(sampler)) {
      ++g_uniformStats.texturesKept;
      return;
    }
    if (!active) {
      glActiveTexture(GL_TEXTURE0 + unit);
    }
    texture.bind(sampler);
    units[unit] = &texture;
    ++g_uniformStats.textureBinds;
  }
};

// EFB copies on the GPU (milestone 5). The display's rectangle is blitted into a scratch texture
// of the copy's size (vitaGL's blit samples the display surface itself; bilinear for a halved copy,
// which at exactly half is GX's 2x2 box filter), then a pass of this file's draws the scratch into
// the copy's texture through a colour matrix, the copy format's (opaque colour, intensity, one
// channel...), with the rows flipped to GX's (a copy's first row is its rectangle's top). Each is a
// scene of its own: the display's scene ends at the copy (its depth kept: initialize) and the next
// draw begins it again. Nothing waits: the GPU runs the scenes in order. (The CPU readback this
// replaces, glReadPixels, waited for the GPU: 7 to 22 ms a copy.)
constexpr bool kFlipCopies = true;

constexpr const char* kCopyVertex = "void main(float3 aPos, float2 aUv0, float4 out vPos : POSITION,\n"
                                    "          float2 out vUv0 : TEXCOORD0) {\n"
                                    "  vPos = float4(aPos, 1.0);\n"
                                    "  vUv0 = aUv0;\n"
                                    "}\n";
constexpr const char* kCopyFragment =
    "float4 main(float2 vUv0 : TEXCOORD0, uniform sampler2D uTex0 : TEXUNIT0, uniform float4 uConst[5]) : COLOR {\n"
    "  float4 t = tex2D(uTex0, vUv0);\n"
    "  return float4(dot(uConst[0], t), dot(uConst[1], t), dot(uConst[2], t), dot(uConst[3], t)) + uConst[4];\n"
    "}\n";
// The pass's program by a key of its own (its binary goes on the card like a generated one's).
constexpr uint64_t kCopyProgramKey = 0x7669746163707931ull; // "vitacpy1"
std::shared_ptr<Program> g_copyProgram;
std::shared_ptr<Texture> g_copyScratch;

// A copy format's colour matrix (four rows, each dotted with the source's RGBA) and offset, as
// GX's copy converts: intensity is its YUV luma (with 16's offset), in all four channels as Aurora
// samples an I8 texture; the opaque formats' alpha is 1 (the game's EFB has none).
void copy_matrix(CopyKind kind, float rows[5][4]) noexcept {
  std::memset(rows, 0, sizeof(float[5][4]));
  constexpr float kLuma[4] = {66.f / 256.f, 129.f / 256.f, 25.f / 256.f, 0.f};
  constexpr float kLumaOffset = 16.f / 255.f;
  const auto set_row = [&rows](int r, const float (&v)[4]) { std::memcpy(rows[r], v, sizeof(v)); };
  const auto channel = [](int c) {
    std::array<float, 4> v{};
    v[c] = 1.f;
    return v;
  };
  switch (kind) {
  case CopyKind::Rgba:
    for (int r = 0; r < 4; ++r) {
      rows[r][r] = 1.f;
    }
    break;
  case CopyKind::Intensity:
  case CopyKind::IntensityAlpha:
    for (int r = 0; r < 4; ++r) {
      set_row(r, kLuma);
      rows[4][r] = kLumaOffset;
    }
    if (kind == CopyKind::IntensityAlpha) {
      std::memset(rows[3], 0, sizeof(rows[3]));
      rows[3][3] = 1.f;
      rows[4][3] = 0.f;
    }
    break;
  case CopyKind::Red:
  case CopyKind::RedAlpha:
  case CopyKind::Green:
  case CopyKind::Blue:
  case CopyKind::Alpha: {
    const int c = kind == CopyKind::Green ? 1 : kind == CopyKind::Blue ? 2 : kind == CopyKind::Alpha ? 3 : 0;
    const auto v = channel(c);
    for (int r = 0; r < 4; ++r) {
      std::memcpy(rows[r], v.data(), sizeof(float[4]));
    }
    if (kind == CopyKind::RedAlpha) {
      std::memset(rows[3], 0, sizeof(rows[3]));
      rows[3][3] = 1.f;
    }
    break;
  }
  default: // Rgb
    for (int r = 0; r < 3; ++r) {
      rows[r][r] = 1.f;
    }
    rows[4][3] = 1.f;
    break;
  }
}

// For the card (VITA_PHASE2.md: the pause screen's backdrop): the screen at half its size, as a PPM
// in the game's folder. glReadPixels ends the display's scene and waits for the GPU (a framebuffer's
// own texture came back black that way: show_texture draws it on the screen first).
void dump_screen(const std::string& name) noexcept {
  std::vector<uint8_t> rgba(static_cast<size_t>(g_width) * g_height * 4);
  glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  const int width = g_width / 2;
  const int height = g_height / 2;
  const std::string path = std::string{g_config.userPath} + "/" + name;
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * 3);
    for (int row = height - 1; row >= 0; --row) { // GL's rows run bottom up
      const uint8_t* line = &rgba[static_cast<size_t>(row) * 2 * g_width * 4];
      for (int col = 0; col < width; ++col) {
        std::memcpy(&rgb[static_cast<size_t>(col) * 3], line + static_cast<size_t>(col) * 2 * 4, 3);
      }
      std::fwrite(rgb.data(), 1, rgb.size(), file);
    }
    std::fclose(file);
  }
  Log.info("[probe] {}: the screen at {}x{}", path, width, height);
}

// The copy pass's quad: a position in clip space and a texture coordinate.
struct CopyVertex {
  float pos[3];
  float uv[2];
};

void draw_copy_quad(Program& program, const CopyVertex (&quad)[6]) noexcept {
  const GLint pos = program.attr(kAttrPos);
  const GLint uv = program.attr(kAttrUv0);
  if (pos >= 0 && uv >= 0) {
    glEnableVertexAttribArray(pos);
    glEnableVertexAttribArray(uv);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, sizeof(CopyVertex), &quad[0].pos);
    glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, sizeof(CopyVertex), &quad[0].uv);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(pos);
    glDisableVertexAttribArray(uv);
  }
}

// For the card (dump_screen): `texture` drawn over the whole screen as it holds it (a copy's first
// row, GX's, at the top), with the copy pass's program. Leaves what copy_on_gpu leaves.
void show_texture(const Texture& texture) noexcept {
  int compiles = 1;
  int loads = 1;
  if (!g_copyProgram || !g_copyProgram->ready(compiles, loads)) {
    return;
  }
  glActiveTexture(GL_TEXTURE0);
  glViewport(0, 0, g_width, g_height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  Program& program = *g_copyProgram;
  glUseProgram(program.id());
  float rows[5][4];
  copy_matrix(CopyKind::Rgba, rows);
  program.send_constants(rows, 5);
  glBindTexture(GL_TEXTURE_2D, texture.id());
  const CopyVertex quad[6] = {
      {{-1.f, -1.f, 0.f}, {0.f, 1.f}}, {{1.f, -1.f, 0.f}, {1.f, 1.f}}, {{1.f, 1.f, 0.f}, {1.f, 0.f}},
      {{-1.f, -1.f, 0.f}, {0.f, 1.f}}, {{1.f, 1.f, 0.f}, {1.f, 0.f}},  {{-1.f, 1.f, 0.f}, {0.f, 0.f}},
  };
  draw_copy_quad(program, quad);
  glUseProgram(0);
}

// What copy_on_gpu did with a copy: made it, found its rectangle empty, left it for the pass's
// program to be built, or left it for the next frame (Over: copy_on_gpu).
enum class CopyResult : uint8_t { Made, Empty, Waiting, Over };

// Render worker, in the fixed-function arrays' absence (draw_world turns them off around it):
// makes `draw`'s copy. Its blit draws a scene into the copies' scratch and its pass one into the
// copy's texture; a framebuffer drawn in more scenes in a frame than vitaGL made its render target
// for came back with stale and foreign tiles (VITA_PHASE2.md: the pause screen's backdrop, its one
// copy the third into the scratch that frame), so a copy that would is left for the next frame.
// Unless Made, the copy's texture keeps what it had. Leaves the display's framebuffer bound, no
// program in use, and the scissor test, blending, depth, culling and the colour mask as the pass
// left them (draw_world sets its own again).
CopyResult copy_on_gpu(const Draw& draw) noexcept {
  const ScreenRect& src = draw.scissor;
  if (src.width <= 0 || src.height <= 0) {
    return CopyResult::Empty;
  }
  if (!g_copyProgram) {
    g_copyProgram = make_program(kCopyVertex, kCopyFragment, kCopyProgramKey);
  }
  int compiles = 1;
  int loads = 1;
  if (!g_copyProgram->ready(compiles, loads)) {
    return CopyResult::Waiting;
  }
  const bool half = draw.copyHalf && src.width > 1 && src.height > 1;
  const int width = half ? src.width / 2 : src.width;
  const int height = half ? src.height / 2 : src.height;
  if (!g_copyScratch) {
    g_copyScratch = make_copy_texture();
  }
  const int scenes = std::max(g_copyScratch->scenes(width, height), draw.copyTo->scenes(width, height));
  if (scenes >= kFramebufferScenesPerFrame) {
    return CopyResult::Over;
  }
  g_presentStats.maxFramebufferScenes =
      std::max(g_presentStats.maxFramebufferScenes, static_cast<uint32_t>(scenes + 1));
  glActiveTexture(GL_TEXTURE0);
  const GLuint scratch = g_copyScratch->render_target(width, height);
  const GLuint target = draw.copyTo->render_target(width, height);

  // The blit: a draw of vitaGL's, which leaves blending, the scissor and the colour mask as they are.
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBlitNamedFramebuffer(0, scratch, src.x, src.y, src.x + src.width, src.y + src.height, 0, 0, width, height,
                         GL_COLOR_BUFFER_BIT, half ? GL_LINEAR : GL_NEAREST);

  // The conversion, into the copy's texture.
  glBindFramebuffer(GL_FRAMEBUFFER, target);
  glViewport(0, 0, width, height);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  Program& program = *g_copyProgram;
  glUseProgram(program.id());
  float rows[5][4];
  copy_matrix(draw.copyKind, rows);
  program.send_constants(rows, 5);
  glBindTexture(GL_TEXTURE_2D, g_copyScratch->id());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // The output's bottom row (GL's first) takes the scratch's top: GX's first row.
  const float bottom = kFlipCopies ? 1.f : 0.f;
  const float top = 1.f - bottom;
  const CopyVertex quad[6] = {
      {{-1.f, -1.f, 0.f}, {0.f, bottom}}, {{1.f, -1.f, 0.f}, {1.f, bottom}}, {{1.f, 1.f, 0.f}, {1.f, top}},
      {{-1.f, -1.f, 0.f}, {0.f, bottom}}, {{1.f, 1.f, 0.f}, {1.f, top}},     {{-1.f, 1.f, 0.f}, {0.f, top}},
  };
  draw_copy_quad(program, quad);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return CopyResult::Made;
}

// Binds a draw's mesh (render worker), counting for the log what its path and kind upload.
GLsizei bind_mesh(Mesh& mesh, DrawPath path, Section section) noexcept {
  const bool fresh = !mesh.uploaded();
  const uint64_t start = fresh ? sceKernelGetProcessTimeWide() : 0;
  const GLsizei count = mesh.bind();
  if (fresh) {
    g_presentCosts.meshUs += sceKernelGetProcessTimeWide() - start;
    ++g_presentCosts.meshes;
  }
  const uint64_t us = g_clock.mark(section);
  if (g_clock.on) {
    SectionClock::Uploads& uploads = g_clock.uploads[path][static_cast<int>(mesh.kind())];
    ++uploads.draws;
    uploads.us += us;
    if (fresh) {
      ++uploads.made;
      uploads.bytes += static_cast<uint64_t>(count) * sizeof(Vertex);
    }
  }
  if (fresh && path == kPathFixed && static_cast<uint32_t>(count) > g_clock.largestFixedVertices) {
    g_clock.largestFixedVertices = static_cast<uint32_t>(count);
    g_clock.largestFixedKind = mesh.kind();
  }
  return count;
}

// A draw with its generated program (render worker). Leaving the fixed-function path turns its
// arrays off; a program switch turns on the generic arrays the new one reads.
void draw_with_program(const Draw& draw, const Frame& frame, Program& program, GLuint& usedProgram,
                       uint32_t& attribsOn, RenderState& current, BoundTextures& bound) {
  if (usedProgram != program.id()) {
    if (usedProgram == 0) {
      glDisableClientState(GL_VERTEX_ARRAY);
      glDisableClientState(GL_COLOR_ARRAY);
      for (int unit = 0; unit < kTexUnits; ++unit) {
        glClientActiveTexture(GL_TEXTURE0 + unit);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      }
    }
    glUseProgram(program.id());
    usedProgram = program.id();
    uint32_t wanted = 0;
    for (int i = 0; i < kAttrCount; ++i) {
      if (program.attr(i) >= 0) {
        wanted |= 1u << program.attr(i);
      }
    }
    for (int i = 0; i < 32; ++i) {
      const uint32_t bit = 1u << i;
      if ((attribsOn & bit) != 0 && (wanted & bit) == 0) {
        glDisableVertexAttribArray(i);
      } else if ((attribsOn & bit) == 0 && (wanted & bit) != 0) {
        glEnableVertexAttribArray(i);
      }
    }
    attribsOn = wanted;
  }
  g_clock.mark(kProgramSwitch);
  const GLsizei count = bind_mesh(*draw.mesh, kPathProgram, kProgramUpload);
  const auto base = reinterpret_cast<uintptr_t>(draw.mesh->base());
  const auto pointer = [base](size_t offset) { return reinterpret_cast<const void*>(base + offset); };
  if (program.attr(kAttrPos) >= 0) {
    glVertexAttribPointer(program.attr(kAttrPos), 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), pointer(offsetof(Vertex, pos)));
  }
  if (program.attr(kAttrRaw) >= 0) {
    glVertexAttribPointer(program.attr(kAttrRaw), 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          pointer(offsetof(Vertex, raw)));
  }
  if (program.attr(kAttrShade) >= 0) {
    glVertexAttribPointer(program.attr(kAttrShade), 1, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          pointer(offsetof(Vertex, shade)));
  }
  if (program.attr(kAttrNrm) >= 0) {
    glVertexAttribPointer(program.attr(kAttrNrm), 3, GL_BYTE, GL_TRUE, sizeof(Vertex), pointer(offsetof(Vertex, normal)));
  }
  for (int coord = 0; coord < kTexCoords; ++coord) {
    if (program.attr(kAttrUv0 + coord) >= 0) {
      glVertexAttribPointer(program.attr(kAttrUv0 + coord), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            pointer(offsetof(Vertex, uv) + coord * sizeof(float[2])));
    }
  }
  g_clock.mark(kProgramPointers);
  const ProgramInputs& inputs = draw.program;
  program.set_constants(inputs, frame);
  for (int i = 0; i < kSamplers; ++i) {
    if (inputs.textures[i] != nullptr && program.samplers[i] != kNoUniform) {
      bound.bind(i, *inputs.textures[i], inputs.samplers[i], false);
    }
  }
  g_clock.mark(kProgramTextures);
  apply_state(inputs.state, current, false);
  g_clock.mark(kProgramState);
  glDrawArrays(draw.mesh->mode(), 0, count);
  g_clock.mark(kProgramDraw);
}

void draw_world(const Frame& frame) {
  const std::vector<Draw>& draws = frame.draws;
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
  GLint unitEnv[kTexUnits];
  for (GLint& env : unitEnv) {
    env = GL_MODULATE;
  }
  // Generated programs: the one in use (0: the fixed-function path) and the generic attribute
  // arrays it has on. A frame compiles a few new ones at most; their draws go fixed-function till then.
  int compileBudget = kCompilesPerFrame;
  int loadBudget = kLoadsPerFrame;
  GLuint usedProgram = 0;
  uint32_t attribsOn = 0;
  const auto enter_fixed_function = [&]() {
    if (usedProgram == 0) {
      return;
    }
    for (int i = 0; i < 32; ++i) {
      if ((attribsOn & (1u << i)) != 0) {
        glDisableVertexAttribArray(i);
      }
    }
    attribsOn = 0;
    glUseProgram(0);
    usedProgram = 0;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    for (int unit = 0; unit < kTexUnits; ++unit) {
      glClientActiveTexture(GL_TEXTURE0 + unit);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
  };
  float matrix[16];
  // vitaGL's matrix calls are not free: the projection and the texture matrices change seldom
  // between draws (the modelview nearly always), so they are loaded only when they differ.
  GLenum matrixMode = 0;
  const auto set_matrix_mode = [&matrixMode](GLenum mode) {
    if (mode != matrixMode) {
      glMatrixMode(mode);
      matrixMode = mode;
    }
  };
  float lastProjection[16];
  bool haveProjection = false;
  float lastTexMatrix[kTexUnits][8];
  bool haveTexMatrix[kTexUnits] = {};
  BoundTextures bound;
  // GX's viewport and scissor as the draws carry them, set where they change; the scissor test
  // stays on through the draws (present's clear and the overlay run without it).
  ScreenRect viewport;
  ScreenRect scissor;
  glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
  glScissor(scissor.x, scissor.y, scissor.width, scissor.height);
  glEnable(GL_SCISSOR_TEST);
  // The line and point width, set where a line or point draw changes it (GXM's context keeps it).
  uint8_t width = 1;
  glLineWidth(1.f);
  glPointSize(1.f);
  const auto set_rects = [&viewport, &scissor](const ScreenRect& v, const ScreenRect& s) {
    if (!(v == viewport)) {
      glViewport(v.x, v.y, v.width, v.height);
      viewport = v;
    }
    if (!(s == scissor)) {
      glScissor(s.x, s.y, s.width, s.height);
      scissor = s;
    }
  };
  g_clock.mark(kSetup);
  if (g_clock.on) {
    g_clock.draws += draws.size();
  }
  // Around a pass of this file's own (an EFB copy, a texture shown for the card), which draws with
  // a program and without the fixed-function arrays: the program in use, its uniforms, the
  // matrices, the rectangles, the line width and the state are all set again at the next draw.
  const auto leave_draws = [&]() {
    enter_fixed_function();
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    for (int unit = 0; unit < kTexUnits; ++unit) {
      glClientActiveTexture(GL_TEXTURE0 + unit);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
  };
  const auto return_to_draws = [&]() {
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    for (int unit = 0; unit < kTexUnits; ++unit) {
      glClientActiveTexture(GL_TEXTURE0 + unit);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glEnable(GL_SCISSOR_TEST);
    viewport = scissor = ScreenRect{0, 0, 0, 0};
    apply_state(current, current, true);
    bound.units[0] = nullptr;
    haveProjection = false;
    for (bool& have : haveTexMatrix) {
      have = false;
    }
    width = 0;
  };
  // The display's scenes this frame: present's clear begins the first, and the first draw or clear
  // after a copy's blit (or a readback) ended one begins the next. Its render target is made for
  // kDisplayScenesPerFrame, GXM's most, and a frame drawn in more came back with stale tiles (the
  // Phazon Suit's effect alone begins four; the cinematics with Samus in hers, more): a copy that
  // would end the last is left for the next frame, its texture keeping the last frame's.
  int displayScenes = 1;
  bool displayOpen = true;
  const auto begin_display = [&displayScenes, &displayOpen]() {
    if (!displayOpen) {
      ++displayScenes;
      displayOpen = true;
    }
  };
  // The pause screen's steps for the card (gx/vita_draw.cpp's PauseDump), each a readback: it ends
  // the display's scene, so only with two of its scenes to spare (its own draw's, the next draw's).
  const auto can_save = [&displayScenes]() { return displayScenes + 2 <= kDisplayScenesPerFrame; };
  const auto save_screen = [&](const std::string& name) {
    dump_screen(name);
    displayOpen = false;
    return_to_draws();
  };
  // The name a dump's step saves under after its draw (vita_gl.hpp, Draw::dump).
  const auto after_name = [](int8_t dump) {
    return dump >= kDumpBlurBegin ? fmt::format("blur{}_after.ppm", dump - kDumpBlurBegin + 1)
                                  : fmt::format("pause_{}_after.ppm", dump);
  };
  bool saveAtEnd = false;
  // This frame's copies so far, for the camera blur's first frame (kDumpBlurBegin).
  std::vector<std::string> frameCopies;
  uint32_t sceneDraws = 0;
  for (const Draw& draw : draws) {
    if (draw.clear) {
      // An EFB copy's clear: its rectangle (the whole screen for the viewport, so the clear is
      // the scissor's alone), with the masks GX's updates give, then the draws' masks again.
      begin_display();
      set_rects(ScreenRect{}, draw.scissor);
      const GLboolean color = draw.clearColor ? GL_TRUE : GL_FALSE;
      glColorMask(color, color, color, draw.clearAlpha ? GL_TRUE : GL_FALSE);
      glDepthMask(draw.clearDepth ? GL_TRUE : GL_FALSE);
      glClearColor(draw.clearRgba[0], draw.clearRgba[1], draw.clearRgba[2], draw.clearRgba[3]);
      glClearDepth(draw.clearZ);
      const GLbitfield bits = (draw.clearColor || draw.clearAlpha ? GL_COLOR_BUFFER_BIT : 0) |
                              (draw.clearDepth ? GL_DEPTH_BUFFER_BIT : 0);
      if (bits != 0) {
        glClear(bits);
      }
      const GLboolean drawColor = current.colorWrite ? GL_TRUE : GL_FALSE;
      glColorMask(drawColor, drawColor, drawColor, current.alphaWrite ? GL_TRUE : GL_FALSE);
      glDepthMask(current.depthWrite ? GL_TRUE : GL_FALSE);
      continue;
    }
    if (draw.copyTo != nullptr) {
      // An EFB copy (copy_on_gpu): scenes of its own, its blit ending the display's.
      if (displayOpen && displayScenes >= kDisplayScenesPerFrame) {
        ++g_presentStats.copiesOverDisplayScenes;
        frameCopies.push_back(fmt::format("{}: left for the next frame (the display's last scene)",
                                          draw.copyTo->describe()));
        if (draw.probe >= 0) {
          Log.info("[probe] render {}: copy left for the next frame (the display's last scene)", draw.probe);
        }
        continue;
      }
      const uint64_t start = sceKernelGetProcessTimeWide();
      leave_draws();
      const CopyResult result = copy_on_gpu(draw);
      static constexpr const char* kResults[] = {"made", "of an empty rectangle", "waiting for its program",
                                                 "left for the next frame (a framebuffer's scenes)"};
      frameCopies.push_back(fmt::format("{}: {}", draw.copyTo->describe(), kResults[static_cast<int>(result)]));
      if (draw.probe >= 0) {
        Log.info("[probe] render {}: copy {} into {}x{} ({} targets)", draw.probe, kResults[static_cast<int>(result)],
                 draw.copyTo->width(), draw.copyTo->height(), draw.copyTo->targets());
      }
      return_to_draws();
      const bool made = result == CopyResult::Made;
      if (made) {
        displayOpen = false;
        sceneDraws = 0;
      }
      ++(result == CopyResult::Waiting ? g_presentStats.copiesWaiting
         : result == CopyResult::Over  ? g_presentStats.copiesOverFramebufferScenes
                                       : g_presentStats.copies);
      g_presentStats.copyPixels += made ? static_cast<uint64_t>(draw.scissor.width) * draw.scissor.height : 0;
      const uint64_t copyUs = sceKernelGetProcessTimeWide() - start;
      g_presentStats.copyUs += copyUs;
      g_presentCosts.copyUs += copyUs;
      ++g_presentCosts.copies;
      g_clock.mark(kCopy);
      continue;
    }
    set_rects(draw.viewport, draw.scissor);
    if (draw.dump == kDumpTexture || draw.dump >= kDumpBlurBegin) {
      // The draw's first texture (the camera blur's copy) on the screen, saved; the draw (the blur,
      // opaque over the whole screen) then covers it. At the blur's first frame, the texture and
      // the frame's copies to the log as well.
      const Texture* texture =
          draw.program.textures[0] != nullptr ? draw.program.textures[0].get() : draw.units[0].texture.get();
      if (draw.dump >= kDumpBlurBegin) {
        std::string copies;
        for (const std::string& c : frameCopies) {
          copies += fmt::format("\n  {}", c);
        }
        Log.info("[probe] the camera blur begins ({}): its texture {}; the display's scenes so far {}; the frame's "
                 "copies:{}",
                 draw.dump - kDumpBlurBegin + 1, texture != nullptr ? texture->describe() : "none", displayScenes,
                 copies.empty() ? " none" : copies);
      }
      if (texture != nullptr && can_save()) {
        begin_display();
        leave_draws();
        show_texture(*texture);
        return_to_draws();
        save_screen(draw.dump >= kDumpBlurBegin ? fmt::format("blur{}_texture.ppm", draw.dump - kDumpBlurBegin + 1)
                                                : fmt::format("pause_{}_texture.ppm", draw.dump));
        set_rects(draw.viewport, draw.scissor);
      }
    }
    saveAtEnd = saveAtEnd || draw.dump == kDumpFrameEnd;
    begin_display();
    g_presentStats.maxSceneDraws = std::max(g_presentStats.maxSceneDraws, ++sceneDraws);
    if (draw.width != width && draw.mesh->mode() != GL_TRIANGLES) {
      glLineWidth(static_cast<GLfloat>(draw.width));
      glPointSize(static_cast<GLfloat>(draw.width));
      width = draw.width;
    }
    Program* program = draw.program.program.get();
    if (program != nullptr) {
      const int budgetBefore = compileBudget + loadBudget;
      const bool ready = program->ready(compileBudget, loadBudget);
      if (compileBudget + loadBudget != budgetBefore) {
        // A program built just now was made current to set its samplers: back to the one in use.
        glUseProgram(usedProgram);
      }
      if (!ready) {
        program = nullptr;
      }
      g_clock.mark(kBuild);
    }
    if (draw.probe >= 0) {
      std::string textures;
      for (int i = 0; i < kSamplers && program != nullptr; ++i) {
        const Texture* texture = draw.program.textures[i].get();
        textures += texture != nullptr ? fmt::format(" t{} {} {}x{}", i, static_cast<const void*>(texture),
                                                     texture->width(), texture->height())
                                       : "";
      }
      for (int unit = 0; unit < kTexUnits && program == nullptr; ++unit) {
        const Texture* texture = draw.units[unit].texture.get();
        textures += texture != nullptr ? fmt::format(" u{} {} {}x{}", unit, static_cast<const void*>(texture),
                                                     texture->width(), texture->height())
                                       : "";
      }
      Log.info("[probe] render {}: {}, {} vertices;{}", draw.probe,
               program != nullptr                ? "program"
               : draw.program.program != nullptr ? "fixed-function, its program not ready"
                                                 : "fixed-function",
               draw.mesh->count(), textures);
    }
    if (program != nullptr) {
      ++g_presentStats.programDraws;
      g_clock.programDraws += g_clock.on ? 1 : 0;
      draw_with_program(draw, frame, *program, usedProgram, attribsOn, current, bound);
      if (((draw.dump >= 0 && draw.dump < kDumpTexture) || draw.dump >= kDumpBlurBegin) && can_save()) {
        save_screen(after_name(draw.dump));
      }
      continue;
    }
    if (draw.program.program != nullptr) {
      ++g_presentStats.programPending;
    }
    if (g_clock.on) {
      ++(draw.program.program != nullptr ? g_clock.fixedPending : g_clock.fixedUnsupported);
    }
    enter_fixed_function();
    g_clock.mark(kFixedSwitch);
    const GLsizei count = bind_mesh(*draw.mesh, kPathFixed, kFixedUpload);
    const auto base = reinterpret_cast<uintptr_t>(draw.mesh->base());
    const auto pointer = [base](size_t offset) { return reinterpret_cast<const void*>(base + offset); };
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), pointer(offsetof(Vertex, pos)));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), pointer(offsetof(Vertex, color)));
    // Both sets point into this mesh, a unit that is off too: no pointer is left into another buffer.
    for (int unit = 0; unit < kTexUnits; ++unit) {
      glClientActiveTexture(GL_TEXTURE0 + unit);
      glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), pointer(offsetof(Vertex, uv) + draw.units[unit].coord * sizeof(float[2])));
    }
    g_clock.mark(kFixedPointers);
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
      bound.bind(unit, *texUnit.texture, texUnit.sampler, true);
      if (texUnit.envMode != unitEnv[unit]) {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, texUnit.envMode);
        unitEnv[unit] = texUnit.envMode;
      }
      if (!haveTexMatrix[unit] || std::memcmp(lastTexMatrix[unit], texUnit.matrix, sizeof(texUnit.matrix)) != 0) {
        set_matrix_mode(GL_TEXTURE);
        to_column_major(texUnit.matrix, 2, matrix);
        glLoadMatrixf(matrix);
        std::memcpy(lastTexMatrix[unit], texUnit.matrix, sizeof(texUnit.matrix));
        haveTexMatrix[unit] = true;
      }
    }
    g_clock.mark(kFixedUnits);
    if (!haveProjection || std::memcmp(lastProjection, draw.projection, sizeof(draw.projection)) != 0) {
      set_matrix_mode(GL_PROJECTION);
      to_column_major(draw.projection, 4, matrix);
      glLoadMatrixf(matrix);
      std::memcpy(lastProjection, draw.projection, sizeof(draw.projection));
      haveProjection = true;
    }
    set_matrix_mode(GL_MODELVIEW);
    to_column_major(draw.modelView, 3, matrix);
    glLoadMatrixf(matrix);
    g_clock.mark(kFixedMatrices);
    apply_state(draw.state, current, false);
    g_clock.mark(kFixedState);
    const uint64_t fixedStart = sceKernelGetProcessTimeWide();
    glDrawArrays(draw.mesh->mode(), 0, count);
    const uint64_t fixedUs = sceKernelGetProcessTimeWide() - fixedStart;
    g_presentCosts.fixedUs += fixedUs;
    g_presentCosts.fixedMaxUs = std::max(g_presentCosts.fixedMaxUs, fixedUs);
    ++g_presentCosts.fixedDraws;
    g_clock.mark(kFixedDraw);
    if (((draw.dump >= 0 && draw.dump < kDumpTexture) || draw.dump >= kDumpBlurBegin) && can_save()) {
      save_screen(after_name(draw.dump));
    }
  }
  if (saveAtEnd) {
    dump_screen(fmt::format("pause_{}_frame.ppm", kDumpFrameEnd));
  }
  g_presentStats.maxDisplayScenes = std::max(g_presentStats.maxDisplayScenes, static_cast<uint32_t>(displayScenes));
  enter_fixed_function();
  // Back to GL's defaults for the overlay (ImGui's backend sets blending, depth and culling
  // itself, but not the alpha test, the masks or the winding).
  apply_state(RenderState{.depthTest = false}, current, true);
  glFrontFace(GL_CCW);
  glDisable(GL_SCISSOR_TEST);
  if (width != 1) {
    glLineWidth(1.f);
    glPointSize(1.f);
  }
  glViewport(0, 0, g_width, g_height);
  for (int unit = kTexUnits - 1; unit >= 0; --unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glClientActiveTexture(GL_TEXTURE0 + unit);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
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

// GXM keeps a depth surface's contents from one scene to the next only when told to (forced load
// and store), and this vitaGL's build does not (its STORE_DEPTH_STENCIL is off): each of the
// display's scenes began with its depth cleared. An EFB copy ends the display's scene within a
// frame (copy_on_gpu), and the draws after it would have tested against nothing. As vitaGL's own
// STORE_DEPTH_STENCIL does: the buffer's mask update bits set, then load and store forced, for a
// few MiB of the GPU's bandwidth at each scene's start and end. Off: kKeepDisplayDepth.
constexpr bool kKeepDisplayDepth = true;

void keep_display_depth() {
  SceGxmDepthStencilSurface& surface = gxm_depth_stencil_surface;
  if (!kKeepDisplayDepth) {
    return;
  }
  if (surface.depthData == nullptr) {
    Log.warn("vitaGL's display has no depth buffer of its own: its depth is not kept across scenes");
    return;
  }
  // As vitaGL allocates it: the size aligned to GXM's tiles, a word a sample (no multisampling).
  const auto align = [](size_t value, size_t to) { return (value + to - 1) / to * to; };
  const size_t bytes = align(g_width, SCE_GXM_TILE_SIZEX) * align(g_height, SCE_GXM_TILE_SIZEY) * 4;
  std::memset(surface.depthData, 0x80, bytes);
  sceGxmDepthStencilSurfaceSetForceLoadMode(&surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
  sceGxmDepthStencilSurfaceSetForceStoreMode(&surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
  Log.info("the display's depth is kept across scenes ({} KiB)", bytes / 1024);
}

// A smaller screen than the Vita's, which its display scales up (fewer pixels for the GPU to fill:
// its share of a frame, the EFB copies' most), where resolution.txt in the game's folder names one
// of the sizes the display takes ("720x408", the GameCube's pixels about; "640x368").
void read_resolution() noexcept {
  const std::string path = std::string{g_config.userPath} + "/resolution.txt";
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) {
    return;
  }
  int width = 0;
  int height = 0;
  const int read = std::fscanf(file, "%dx%d", &width, &height);
  std::fclose(file);
  constexpr int kSizes[][2] = {{960, 544}, {720, 408}, {640, 368}, {480, 272}};
  for (const auto& size : kSizes) {
    if (read == 2 && width == size[0] && height == size[1]) {
      g_width = width;
      g_height = height;
      Log.info("{}: the screen at {}x{}", path, width, height);
      return;
    }
  }
  Log.warn("{}: not one of the display's sizes (960x544, 720x408, 640x368, 480x272); the screen stays {}x{}", path,
           g_width, g_height);
}

} // namespace

int screen_width() noexcept { return g_width; }
int screen_height() noexcept { return g_height; }

bool initialize() noexcept {
  // vitaGL compiles the shaders behind its fixed-function calls (the overlay's) with the Vita's
  // own compiler, which takes a while; it keeps them here, in the game's folder.
  static const std::string cachePath = std::string{g_config.userPath} + "/vitagl_shader_cache";
  std::error_code ec;
  std::filesystem::create_directories(cachePath, ec);
  vglSetShaderCachePath(cachePath.c_str());
  // The generated programs' own cache (Program, load_cached).
  g_programCacheDir = std::string{g_config.userPath} + "/vita_programs";
  std::filesystem::create_directories(g_programCacheDir, ec);
  // GXM's ring buffers hold a scene's draws until the GPU draws the scene (the fragment ring each
  // draw's textures and fragment state, the VDM ring its commands). The defaults (512, 128 and 16
  // KiB) were made for scenes of a few hundred plain draws; the water's generated programs (four
  // to seven textures each) put 1100 to 4300 draws in one scene, and a run in Tallon Overworld
  // ended in a GPU hang with the water black at some angles (VITA_PHASE2.md). Four times each; the
  // vertex ring (2 MiB) twice.
  vglSetFragmentBufferSize(4 * SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE);
  vglSetVDMBufferSize(4 * SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE);
  vglSetVertexBufferSize(2 * SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE);
  vglSetUSSEBufferSize(4 * SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE);
  // Each EFB copy ends the display's scene and begins another (copy_on_gpu), and draws into two
  // framebuffers of its own: the pause screen's blur makes four a frame, the display five scenes
  // and more. vitaGL makes its render targets for one scene a frame unless told; GXM sizes a target's
  // per-scene state by it. The copies came back with stale tiles and black triangles (the charged
  // shot's warp, the pause screen's backdrop, the cinematics' blur) and the GPU hung twice.
  vglSetupRenderTargetScenesNum(kDisplayScenesPerFrame, kFramebufferScenesPerFrame);
  // Not a success flag: this vitaGL returns true when the requested size was beyond the display's
  // and it started at the display's own size instead, false when it started as asked. Either way
  // GXM is now vitaGL's, so nothing else (SDL's renderer) may initialise it after this.
  read_resolution();
  const bool clamped = vglInitExtended(kLegacyPool, g_width, g_height, kRamThreshold, SCE_GXM_MULTISAMPLE_NONE);
  g_active = true;
  if (clamped) {
    Log.warn("vitaGL started at the display's maximum size, not {}x{}", g_width, g_height);
    g_width = 960;
    g_height = 544;
  } else {
    Log.info("vitaGL started at {}x{}", g_width, g_height);
  }
  if (!clamped) {
    keep_display_depth();
  }
  // For the log: the memory GXM takes from the system for a render target of the screen's size
  // and of half of it, by its scenes a frame (vitaGL makes one for each framebuffer).
  for (const auto& [width, height] : {std::pair{g_width, g_height}, std::pair{g_width / 2, g_height / 2}}) {
    std::string sizes;
    for (const unsigned scenes : {1u, 2u, 3u, 4u, 8u}) {
      SceGxmRenderTargetParams params{};
      params.width = static_cast<uint16_t>(width);
      params.height = static_cast<uint16_t>(height);
      params.scenesPerFrame = static_cast<uint16_t>(scenes);
      params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
      params.driverMemBlock = -1;
      unsigned int bytes = 0;
      sceGxmGetRenderTargetMemSize(&params, &bytes);
      sizes += fmt::format("{}{} scenes {} KiB", sizes.empty() ? "" : ", ", scenes, bytes / 1024);
    }
    Log.info("a {}x{} render target's memory: {}", width, height, sizes);
  }
  start_compiler();
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

namespace {
// The timer's own cost, measured once: the log's sections each hold about one call of it a mark.
double timer_call_us() noexcept {
  constexpr int kCalls = 1000;
  const uint64_t start = sceKernelGetProcessTimeWide();
  uint64_t sink = 0;
  for (int i = 0; i < kCalls; ++i) {
    sink += sceKernelGetProcessTimeWide();
  }
  const uint64_t end = sceKernelGetProcessTimeWide();
  return sink != 0 ? static_cast<double>(end - start) / kCalls : 0.0;
}

// Once a second of the render worker's presents: the timed presents' sections, per present.
void report_sections() noexcept {
  SectionClock& c = g_clock;
  const uint64_t now = sceKernelGetProcessTimeWide();
  if (c.reportStart == 0) {
    c.reportStart = now;
    return;
  }
  if (now - c.reportStart < 1000000 || c.frames == 0) {
    return;
  }
  const double frames = c.frames;
  const auto ms = [frames](uint64_t us) { return static_cast<double>(us) / 1000.0 / frames; };
  uint64_t total = 0;
  for (const uint64_t us : c.us) {
    total += us;
  }
  std::string program;
  std::string fixed;
  std::string frame;
  for (int s = 0; s < kSectionCount; ++s) {
    std::string& list = s >= kProgramSwitch && s <= kProgramDraw ? program
                        : s >= kFixedSwitch && s <= kFixedDraw   ? fixed
                                                                 : frame;
    list += fmt::format("{}{} {:.2f}", list.empty() ? "" : ", ", kSectionNames[s], ms(c.us[s]));
  }
  Log.info("[draw path] ms/present over {} timed: {:.2f} in all, {:.0f} draws ({:.0f} with programs) | program "
           "draws: {} | fixed-function draws: {} | frame: {} | timer {:.2f} us a call, {:.0f} calls a present",
           c.frames, ms(total), c.draws / frames, c.programDraws / frames, program, fixed, frame, c.callUs,
           c.calls / frames);
  // By path and mesh kind, per timed present: draws, buffers made, their KiB, the upload section.
  std::string uploads;
  for (int path = 0; path < kPathCount; ++path) {
    uploads += path == kPathProgram ? "program draws:" : " | fixed-function draws:";
    for (int kind = 0; kind < static_cast<int>(MeshKind::Count); ++kind) {
      const SectionClock::Uploads& u = c.uploads[path][kind];
      if (u.draws != 0) {
        uploads += fmt::format(" {} {:.0f}/{:.1f}/{:.0f}/{:.2f}", kMeshKindNames[kind], u.draws / frames,
                               u.made / frames, static_cast<double>(u.bytes) / 1024.0 / frames, ms(u.us));
      }
    }
  }
  Log.info("[draw uploads] per timed present, draws/buffers made/KiB/ms by mesh kind: {} | fixed-function: {:.1f} "
           "waiting for their program, {:.1f} without one; the largest mesh made for one since the last line {} "
           "vertices ({})",
           uploads, c.fixedPending / frames, c.fixedUnsupported / frames, c.largestFixedVertices,
           kMeshKindNames[static_cast<int>(c.largestFixedKind)]);
  const double callUs = c.callUs;
  c = {};
  c.callUs = callUs;
  c.reportStart = now;
}
} // namespace

void present(ImDrawData* overlay) noexcept {
  if (g_clock.callUs < 0.0) {
    g_clock.callUs = timer_call_us();
  }
  g_clock.on = ++g_clock.presents % kTimedEvery == 0;
  g_clock.start();
  g_presentCosts = {};
  g_presentCosts.start = sceKernelGetProcessTimeWide();
  ++g_presentSerial;
  delete_retired_targets();
  // The font texture is made here, on the render worker, from the atlas the game thread built;
  // the atlas only changes when the fonts do (fonts_changed).
  if (g_fontsChanged.exchange(false, std::memory_order_acq_rel)) {
    ImGui_ImplOpenGL2_DestroyFontsTexture();
  }
  ImGui_ImplOpenGL2_NewFrame();
  glViewport(0, 0, g_width, g_height);
  glDisable(GL_SCISSOR_TEST);
  // Black, as the GameCube's frame starts (a dark blue told this path's screen apart at first).
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClearDepth(1.0);
  // The clear follows the masks; a draw's state may have left them off.
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  Frame frame = take_frame();
  draw_world(frame);
  g_reaper.reap(std::move(frame));
  g_clock.mark(kFinish);
  if (++g_presentStats.presents == 300) {
    Log.info("presents: {}, {} with game draws, {:.1f} draws a frame; generated programs: {} draws with theirs, {} "
             "fixed-function waiting for theirs; {} built ({} failed, {:.0f} ms compiling; {} loaded from the card, "
             "{:.0f} ms) in all; uniforms sent/kept: vertex {}/{}, fragment {}/{}; textures bound/kept: {}/{}; "
             "EFB copies on the GPU: {} ({} waiting for their pass's program, {} and {} left for the next frame by "
             "the display's scenes and a framebuffer's; {:.0f} kpixels; render worker {:.1f} ms, {:.2f} ms each; {} "
             "render targets made in all); most draws in a scene: {}; most scenes in a frame: the display's {}, a "
             "framebuffer's {}",
             g_presentStats.presents, g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0,
             g_presentStats.programDraws, g_presentStats.programPending, g_programStats.compiled,
             g_programStats.failed, g_programStats.compileMs, g_programStats.loaded, g_programStats.loadMs,
             g_uniformStats.vertex.uploaded, g_uniformStats.vertex.skipped, g_uniformStats.fragment.uploaded,
             g_uniformStats.fragment.skipped, g_uniformStats.textureBinds, g_uniformStats.texturesKept,
             g_presentStats.copies, g_presentStats.copiesWaiting, g_presentStats.copiesOverDisplayScenes,
             g_presentStats.copiesOverFramebufferScenes, g_presentStats.copyPixels / 1000.0,
             g_presentStats.copyUs / 1000.0,
             g_presentStats.copies != 0 ? g_presentStats.copyUs / 1000.0 / g_presentStats.copies : 0.0,
             g_renderTargets.load(std::memory_order_relaxed), g_presentStats.maxSceneDraws,
             g_presentStats.maxDisplayScenes, g_presentStats.maxFramebufferScenes);
    Log.info("background compiler: {} programs compiled ({:.0f} ms there; linked here in {:.0f} ms), {} compiled on "
             "the render worker after all, {} waiting",
             g_programStats.background, g_programStats.backgroundMs, g_programStats.linkMs, g_programStats.fallbacks,
             g_compiler.waiting());
    g_presentStats = {};
    g_uniformStats = {};
    log_memory("presents");
  }
  g_clock.mark(kOther);
  if (overlay != nullptr && overlay->CmdListsCount > 0) {
    ImGui_ImplOpenGL2_RenderDrawData(overlay);
  }
  g_clock.mark(kOverlay);
  const uint64_t swapStart = sceKernelGetProcessTimeWide();
  vglSwapBuffers(GL_FALSE);
  const uint64_t end = sceKernelGetProcessTimeWide();
  g_presentCosts.swapUs = end - swapStart;
  g_clock.mark(kSwap);
  if (g_clock.on) {
    ++g_clock.frames;
  }
  g_clock.on = false;
  const PresentCosts& c = g_presentCosts;
  if (const uint64_t total = end - c.start; total >= kSlowPresentUs) {
    const uint64_t named = c.buildUs + c.textureUs + c.meshUs + c.fixedUs + c.copyUs + c.swapUs;
    const auto ms = [](uint64_t us) { return static_cast<double>(us) / 1000.0; };
    Log.info("[slow present] {:.0f} ms: program builds {:.0f} ms ({} loaded, {} linked, {} compiled here); first "
             "uploads: textures {:.0f} ms ({}, {} KiB), meshes {:.0f} ms ({}); fixed-function draws {:.0f} ms ({}, "
             "the slowest {:.0f} ms); EFB copies {:.0f} ms ({}); swap {:.0f} ms; the rest {:.0f} ms",
             ms(total), ms(c.buildUs), c.loads, c.links, c.compiles, ms(c.textureUs), c.textures,
             c.textureBytes / 1024, ms(c.meshUs), c.meshes, ms(c.fixedUs), c.fixedDraws, ms(c.fixedMaxUs),
             ms(c.copyUs), c.copies, ms(c.swapUs), ms(total > named ? total - named : 0));
  }
  report_sections();
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

namespace fifo_clock {
namespace {
constexpr const char* kNames[kSectionCount] = {
    "idle", "state",  "copies",      "dispatch", "material", "lookup", "skinned", "immediate", "mesh",
    "record", "other draws", "aurora", "vita",     "textures", "finish", "frame"};
constexpr uint32_t kTimedEvery = 8;

uint64_t g_last = 0;
uint64_t g_us[kSectionCount] = {};
uint64_t g_calls = 0;
uint32_t g_timedFrames = 0;
uint32_t g_frames = 0;
uint64_t g_reportStart = 0;
} // namespace

bool g_on = false;

void mark_timed(Section section) noexcept {
  const uint64_t now = sceKernelGetProcessTimeWide();
  g_us[section] += now - g_last;
  g_last = now;
  ++g_calls;
}

void frame_end() noexcept {
  const uint64_t now = sceKernelGetProcessTimeWide();
  if (g_on) {
    g_us[kEndFrame] += now - g_last;
    ++g_calls;
    ++g_timedFrames;
  }
  if (g_reportStart == 0) {
    g_reportStart = now;
  } else if (now - g_reportStart >= 1000000 && g_timedFrames != 0) {
    const double frames = g_timedFrames;
    const auto ms = [frames](uint64_t us) { return static_cast<double>(us) / 1000.0 / frames; };
    uint64_t total = 0;
    std::string sections;
    for (int s = 0; s < kSectionCount; ++s) {
      total += g_us[s];
      sections += fmt::format("{}{} {:.2f}", s == 0 ? "" : ", ", kNames[s], ms(g_us[s]));
    }
    Log.info("[fifo path] ms/frame over {} timed: {:.2f} in all, {:.2f} busy | {} | {:.0f} timer calls a frame",
             g_timedFrames, ms(total), ms(total - g_us[kIdle]), sections, g_calls / frames);
    for (uint64_t& us : g_us) {
      us = 0;
    }
    g_calls = 0;
    g_timedFrames = 0;
    g_reportStart = now;
  }
  g_on = ++g_frames % kTimedEvery == 0;
  g_last = sceKernelGetProcessTimeWide();
}
} // namespace fifo_clock
} // namespace aurora::vita_gl

// vitashark's compile and its output's release, for every caller (vitaGL's and ours: the linker
// wraps both, CMakeLists): a compile holds the compiler from the one to the other
// (aurora::vita_gl::compiler_lock), as vitashark keeps a compile's state in globals.
extern "C" {
SceGxmProgram* __real_shark_compile_shader_extended(const char* src, uint32_t* size, shark_type type, shark_opt opt,
                                                    int32_t useFastmath, int32_t useFastprecision, int32_t useFastint);
void __real_shark_clear_output();

SceGxmProgram* __wrap_shark_compile_shader_extended(const char* src, uint32_t* size, shark_type type, shark_opt opt,
                                                    int32_t useFastmath, int32_t useFastprecision, int32_t useFastint) {
  aurora::vita_gl::compiler_lock();
  return __real_shark_compile_shader_extended(src, size, type, opt, useFastmath, useFastprecision, useFastint);
}

void __wrap_shark_clear_output() {
  __real_shark_clear_output();
  aurora::vita_gl::compiler_unlock();
}

// vitaGL's splash screen: its logo, which vglInit has a thread of its own draw on a GXM context of
// its own until the first frame's scene. Not started (the linker wraps its start, CMakeLists): its
// flag stays off, so that scene and the swaps take their usual paths, as in vitaGL's system app
// mode, which skips it too.
void __wrap_invoke_splashscreen() {}
}
