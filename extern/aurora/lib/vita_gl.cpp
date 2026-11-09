#include "vita_gl.hpp"
#include "vita_fifo_clock.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"
#include "thread.hpp"

#include <vitaGL.h>
#include <vitashark.h>
#include <SDL3/SDL_stdinc.h>
#include <psp2/common_dialog.h>
#include <psp2/gxm.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "backends/imgui_impl_opengl2.h"

// vitaGL's own (its shared.h; global in the library VitaSDK ships, 2026-10): the display's colour
// surfaces (each one's backgroundTex is the texture GXM loads it with at a scene's start), the one
// the frame draws into, and the display's depth and stencil surface. The EFB copies read the
// first (copy_on_gpu) and keep the last across the scenes they end (keep_depth).
// And its shader patcher, whose heaps the log reports (log_memory).
extern "C" {
extern SceGxmColorSurface gxm_color_surfaces[];
extern unsigned int gxm_back_buffer_index;
extern SceGxmDepthStencilSurface gxm_depth_stencil_surface;
extern SceGxmShaderPatcher* gxm_shader_patcher;
}

#include <cstdio>

#include <fmt/format.h>

#include <malloc.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

// The port's commit (its generated port_build_info.cpp), which names the build that wrote the card's
// program binaries (program_binaries_build). Weak: a target without it links, and keeps them.
extern const char kMpBuildRevision[] __attribute__((weak));

namespace aurora::vita_gl {
namespace {
constexpr Module Log{"aurora::vita_gl"};
constexpr int kWidth = kScreenWidth;
constexpr int kHeight = kScreenHeight;
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
// Textures made and not yet uploaded (no draw of theirs bound them): their pixels on the heap.
std::atomic<int64_t> g_pendingCount;
std::atomic<int64_t> g_pendingBytes;

// vitaGL's shader patcher's heaps (vglSetupShaderPatcher; 1 MiB each by default). Every program
// is patched once for each blend state it draws with (the colour mask and factors among them) and
// each stays for the session (vitaGL releases none), in the fragment USSE heap above all; vitaGL
// does not check the patch, so a full heap leaves a draw a broken program: a GPU crash. tallon19's
// map crashed so, likely: the pause screen and the map draw with an alpha channel, where
// GXSetDstAlpha's zero gives every program another blend state (dst_alpha_mode), and the map
// draws some 1,700 times a frame. The log reports what is in use (log_memory).
constexpr uint32_t kPatcherBuffer = 2 * 1024 * 1024;
constexpr uint32_t kPatcherVertexUsse = 2 * 1024 * 1024;
constexpr uint32_t kPatcherFragmentUsse = 4 * 1024 * 1024;

// vitaGL's memory by pool, free of total (MiB), and its shader patcher's heaps in use (KiB). The
// game's heap is in the [heap] lines.
void log_memory(const char* when) {
  constexpr double kMiB = 1024.0 * 1024.0;
  const auto pool = [](vglMemType type) {
    return fmt::format("{:.1f}/{:.1f}", vglMemFree(type) / kMiB, vglMemTotal(type) / kMiB);
  };
  std::string patcher;
  if (gxm_shader_patcher != nullptr) {
    patcher = fmt::format("; shader patcher in use (KiB): fragment USSE {} of {}, vertex USSE {} of {}, buffer {} of "
                          "{}, host {}",
                          sceGxmShaderPatcherGetFragmentUsseMemAllocated(gxm_shader_patcher) / 1024,
                          kPatcherFragmentUsse / 1024,
                          sceGxmShaderPatcherGetVertexUsseMemAllocated(gxm_shader_patcher) / 1024,
                          kPatcherVertexUsse / 1024, sceGxmShaderPatcherGetBufferMemAllocated(gxm_shader_patcher) / 1024,
                          kPatcherBuffer / 1024, sceGxmShaderPatcherGetHostMemAllocated(gxm_shader_patcher) / 1024);
  }
  Log.info("{}: vitaGL memory free of total (MiB): VRAM {}, RAM {}, phycont {}, budget {}; "
           "textures on the GPU: {} RGBA8 ({:.1f} MiB), {} BC1 ({:.1f} MiB); not yet uploaded: {} ({:.1f} MiB){}",
           when, pool(VGL_MEM_VRAM), pool(VGL_MEM_RAM), pool(VGL_MEM_PHYCONT), pool(VGL_MEM_BUDGET),
           g_textureCount[0].load(), g_textureBytes[0].load() / kMiB, g_textureCount[1].load(),
           g_textureBytes[1].load() / kMiB, g_pendingCount.load(), g_pendingBytes.load() / kMiB, patcher);
}

// A FrameArray's storage: its elements, and how many fit.
template <typename T>
struct ArrayStorage {
  std::unique_ptr<T[]> data;
  uint32_t capacity = 0;
};

// Frame arrays' storage kept for the frames after. A heavy frame's vertices run to megabytes (5 MB
// in a run of 0ee747a); allocated for each frame and freed a frame later on the reaper's thread,
// they left newlib's heap in pieces too small for the next one: std::bad_alloc on the FIFO thread
// with 55 MB free. Storage a frame lets go of comes here instead, the two largest pieces kept, and
// a frame needing room takes the smallest that fits.
template <typename T>
class SpareStorage {
public:
  static SpareStorage& get() noexcept {
    static SpareStorage& spares = *new SpareStorage;
    return spares;
  }
  void put(ArrayStorage<T>&& storage) noexcept {
    if (!storage.data) {
      return;
    }
    ArrayStorage<T> dropped; // freed after the lock
    std::lock_guard lock{m_mutex};
    m_spares.push_back(std::move(storage));
    if (m_spares.size() > kKept) {
      const auto smallest = std::min_element(m_spares.begin(), m_spares.end(),
                                             [](const auto& a, const auto& b) { return a.capacity < b.capacity; });
      dropped = std::move(*smallest);
      m_spares.erase(smallest);
    }
  }
  // The smallest spare holding at least `capacity`, or none.
  ArrayStorage<T> take(uint32_t capacity) noexcept {
    std::lock_guard lock{m_mutex};
    auto best = m_spares.end();
    for (auto it = m_spares.begin(); it != m_spares.end(); ++it) {
      if (it->capacity >= capacity && (best == m_spares.end() || it->capacity < best->capacity)) {
        best = it;
      }
    }
    if (best == m_spares.end()) {
      return {};
    }
    ArrayStorage<T> storage = std::move(*best);
    m_spares.erase(best);
    return storage;
  }

private:
  static constexpr size_t kKept = 2;
  std::mutex m_mutex;
  std::vector<ArrayStorage<T>> m_spares;
};

// A frame's own vertices or indices (FrameMesh), written by the FIFO thread as it records: grown
// by doubling, and left for the caller to write (nothing is cleared first, as a vector's resize
// would). Its storage comes from and goes back to the spares (SpareStorage).
template <typename T>
class FrameArray {
public:
  FrameArray() = default;
  FrameArray(FrameArray&& other) noexcept
  : m_data(std::move(other.m_data)), m_size(std::exchange(other.m_size, 0)),
    m_capacity(std::exchange(other.m_capacity, 0)) {}
  FrameArray& operator=(FrameArray&& other) noexcept {
    if (this != &other) {
      SpareStorage<T>::get().put({std::move(m_data), m_capacity});
      m_data = std::move(other.m_data);
      m_size = std::exchange(other.m_size, 0);
      m_capacity = std::exchange(other.m_capacity, 0);
    }
    return *this;
  }
  ~FrameArray() { SpareStorage<T>::get().put({std::move(m_data), m_capacity}); }

  // Room for `count` more, `at` set to where it starts; it holds till the next call.
  T* grow(uint32_t count, uint32_t& at) {
    if (m_size + count > m_capacity) {
      reserve(std::max(m_size + count, m_capacity * 2));
    }
    at = m_size;
    m_size += count;
    return m_data.get() + at;
  }
  void reserve(uint32_t capacity) {
    if (capacity <= m_capacity) {
      return;
    }
    ArrayStorage<T> storage = SpareStorage<T>::get().take(capacity);
    if (!storage.data) {
      storage.data.reset(new T[capacity]);
      storage.capacity = capacity;
    }
    if (m_size != 0) {
      std::memcpy(storage.data.get(), m_data.get(), sizeof(T) * m_size);
    }
    SpareStorage<T>::get().put({std::move(m_data), m_capacity});
    m_data = std::move(storage.data);
    m_capacity = storage.capacity;
  }
  void trim(uint32_t size) { m_size = std::min(m_size, size); }
  const T* at(uint32_t index) const { return m_data.get() + index; }
  T* at(uint32_t index) { return m_data.get() + index; }
  uint32_t size() const { return m_size; }

private:
  std::unique_ptr<T[]> m_data;
  uint32_t m_size = 0;
  uint32_t m_capacity = 0;
};

// A frame's draws, their programs' constant rows (ProgramInputs) and the vertices and indices of
// its own meshes (FrameMesh), and its serial, a new one each frame (from 1), by which the render
// worker tells one frame's rows from another's; and its EFB copies among the draws, counted.
struct Frame {
  std::vector<Draw> draws;
  std::vector<std::array<float, 4>> constants;
  FrameArray<Vertex> vertices;
  FrameArray<uint16_t> indices;
  // What its draws draw with, each held once (keep), let go with the frame.
  std::vector<std::shared_ptr<const void>> kept;
  uint32_t serial = 0;
  uint32_t copies = 0;
  uint32_t merged = 0; // draws joined to the one before (merge_into_last)
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

// What GL draws a mesh's primitives as.
GLenum gl_mode(MeshPrimitive primitive) {
  return primitive == MeshPrimitive::Lines    ? GL_LINES
         : primitive == MeshPrimitive::Points ? GL_POINTS
                                              : GL_TRIANGLES;
}
} // namespace

// What a frame's draws draw with (Mesh, Texture, Program): the serial of the last frame that keep
// held it for (FIFO thread), so a frame holds each once.
struct Kept {
  uint32_t keptIn = 0;
};

class Mesh : public Kept {
public:
  Mesh(std::vector<Vertex> triangles, MeshKind kind, bool clientMemory, MeshPrimitive primitive) noexcept
  : m_cpu(std::move(triangles)), m_count(static_cast<GLsizei>(m_cpu.size())), m_kind(kind), m_client(clientMemory),
    m_primitive(primitive) {}
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
  // Its vertices, which a draw draws.
  GLsizei count() const noexcept { return m_count; }
  MeshKind kind() const noexcept { return m_kind; }
  MeshPrimitive primitive() const noexcept { return m_primitive; }
  // What its arrays' offsets are from: its vertices in client memory, else the bound buffer's start.
  const uint8_t* base() const noexcept {
    return m_client ? reinterpret_cast<const uint8_t*>(m_cpu.data()) : nullptr;
  }

  // Render worker, its arrays set (bind): draws its `count` vertices.
  void draw(GLsizei count) const noexcept { glDrawArrays(gl_mode(m_primitive), 0, count); }

private:
  std::vector<Vertex> m_cpu;
  GLuint m_vbo = 0;
  GLsizei m_count;
  MeshKind m_kind;
  bool m_client;
  MeshPrimitive m_primitive;
};

std::shared_ptr<Mesh> make_mesh(std::vector<Vertex> triangles, MeshKind kind, bool clientMemory,
                                MeshPrimitive primitive) noexcept {
  return std::make_shared<Mesh>(std::move(triangles), kind, clientMemory, primitive);
}

void pad_triangle_indices(std::vector<uint16_t>& indices) noexcept {
  if (!indices.empty()) {
    indices.insert(indices.end(), (12 - indices.size() % 12) % 12, indices.back());
  }
}

class Texture : public Kept {
public:
  Texture(uint32_t width, uint32_t height, TexelFormat format, std::vector<uint8_t> texels, uint32_t levels) noexcept
  : m_cpu(std::move(texels)), m_width(width), m_height(height), m_format(format),
    m_levels(format == TexelFormat::BC1 ? std::max(levels, 1u) : 1) {
    m_pending = static_cast<int64_t>(m_cpu.size());
    if (m_pending != 0) {
      g_pendingCount.fetch_add(1, std::memory_order_relaxed);
      g_pendingBytes.fetch_add(m_pending, std::memory_order_relaxed);
    }
  }
  ~Texture() {
    settle_pending();
    const int kind = static_cast<int>(m_format);
    if (m_surfaces != nullptr) {
      // A copy texture: its texture and framebuffer for each size it was copied at, and the
      // stand-in it was bound as before its first copy, if that is still about.
      std::vector<GLuint> textures;
      std::vector<GLuint> framebuffers;
      if (m_id != 0 && !is_surface(m_id)) {
        textures.push_back(m_id);
      }
      for (const CopySurface& surface : *m_surfaces) {
        if (surface.texture != 0) {
          textures.push_back(surface.texture);
          framebuffers.push_back(surface.framebuffer);
        }
      }
      g_textureCount[kind].fetch_sub(static_cast<int64_t>(textures.size()), std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
      gfx::render_worker::enqueue_work([textures = std::move(textures), framebuffers = std::move(framebuffers)] {
        glDeleteFramebuffers(static_cast<GLsizei>(framebuffers.size()), framebuffers.data());
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
      });
      return;
    }
    if (m_id != 0) {
      const GLuint id = m_id;
      gfx::render_worker::enqueue_work([id] { glDeleteTextures(1, &id); });
      g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
    }
  }

  uint64_t bytes() const noexcept {
    if (m_format != TexelFormat::BC1) {
      return uint64_t{m_width} * m_height * 4;
    }
    uint64_t total = 0;
    for (uint32_t level = 0; level < m_levels; ++level) {
      total += bc1_level_bytes(level);
    }
    return total;
  }

  // Render worker: true when binding it again with `sampler` would change nothing.
  bool holds(const Sampler& sampler) const noexcept { return m_id != 0 && m_samplerSet && sampler == m_sampler; }

  // Render worker: its size (a copy texture's as its last copy made it), and whether a copy has
  // filled it.
  uint32_t width() const noexcept { return m_width; }
  uint32_t height() const noexcept { return m_height; }
  bool copied() const noexcept { return m_surfaces != nullptr; }

  // Render worker: binds it to the active unit (made from the CPU copy at the first draw) and
  // sets its sampler state where it changed.
  void bind(const Sampler& sampler) noexcept {
    if (m_id == 0) {
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
        // Level by level from the first: vitaGL grows the texture's mip chain for each, moving it
        // (sceClibMspaceRealloc) where it cannot grow in place, and copies a power-of-two level in
        // with a GPU transfer it does not wait for. A chain moved while that transfer still wrote
        // left the write landing in a freed block of vitaGL's VRAM heap, whose bookkeeping lives in
        // its free blocks: the heap broke (0.42.0 test 10, a data abort in SceLibKernel's heap code
        // on the render worker at the first room with mipmapped textures). So each level after the
        // first waits for the transfers before it, small copies all.
        size_t offset = 0;
        uint32_t uploaded = 0;
        for (uint32_t level = 0; level < m_levels; ++level) {
          const auto size = static_cast<size_t>(bc1_level_bytes(level));
          if (offset + size > m_cpu.size()) {
            break;
          }
          if (level != 0) {
            sceGxmTransferFinish();
          }
          glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                 std::max(width >> level, 1), std::max(height >> level, 1), 0,
                                 static_cast<GLsizei>(size), m_cpu.data() + offset);
          offset += size;
          ++uploaded;
        }
        static int sChainsLogged = 0;
        if (uploaded > 1 && sChainsLogged < 4) {
          ++sChainsLogged;
          Log.info("BC1 texture {}x{} uploaded with {} levels ({} bytes)", m_width, m_height, uploaded, offset);
        }
      } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_cpu.data());
      }
      m_bytes = static_cast<int64_t>(m_cpu.size());
      const int kind = static_cast<int>(m_format);
      g_textureCount[kind].fetch_add(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_add(m_bytes, std::memory_order_relaxed);
      std::vector<uint8_t>().swap(m_cpu);
      settle_pending();
    } else {
      glBindTexture(GL_TEXTURE_2D, m_id);
    }
    if (!m_samplerSet || !(sampler == m_sampler)) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler.wrapS);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler.wrapT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(sampler.minFilter));
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sampler.magFilter);
      m_sampler = sampler;
      m_samplerSet = true;
    }
  }

  // Render worker: this copy texture made `width` x `height`, and its framebuffer the one drawn to,
  // for a copy into it (copy_on_gpu); it is sampled as that size from then on. A destination
  // copied at more than one size keeps a texture for each of a few (the spare buffer takes the
  // particles' copy of the viewport and the beam change's of a quarter of it, frame after frame),
  // the one copied least lately made again at the new size. vitaGL gives each framebuffer a depth
  // buffer and a render target of its own at its first scene.
  void target(int width, int height) noexcept {
    const int kind = static_cast<int>(m_format);
    if (m_surfaces == nullptr) {
      m_surfaces = std::make_unique<std::array<CopySurface, kCopySurfaces>>();
    }
    if (m_id != 0 && !is_surface(m_id)) {
      // The 1x1 stand-in it was bound as before its first copy.
      glDeleteTextures(1, &m_id);
      g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
      m_bytes = 0;
      m_id = 0;
    }
    CopySurface* surface = nullptr;
    for (CopySurface& s : *m_surfaces) {
      if (s.texture != 0 && s.width == width && s.height == height) {
        surface = &s;
        break;
      }
    }
    if (surface == nullptr) {
      surface = &(*m_surfaces)[0];
      for (CopySurface& s : *m_surfaces) {
        if (s.texture == 0) {
          surface = &s;
          break;
        }
        if (s.used < surface->used) {
          surface = &s;
        }
      }
      if (surface->texture == 0) {
        glGenTextures(1, &surface->texture);
        glGenFramebuffers(1, &surface->framebuffer);
        g_textureCount[kind].fetch_add(1, std::memory_order_relaxed);
      }
      glBindTexture(GL_TEXTURE_2D, surface->texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      const int64_t bytes = (static_cast<int64_t>(width) * height - static_cast<int64_t>(surface->width) * surface->height) * 4;
      g_textureBytes[kind].fetch_add(bytes, std::memory_order_relaxed);
      m_bytes += bytes;
      surface->width = static_cast<uint16_t>(width);
      surface->height = static_cast<uint16_t>(height);
      glBindFramebuffer(GL_FRAMEBUFFER, surface->framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface->texture, 0);
    } else {
      glBindFramebuffer(GL_FRAMEBUFFER, surface->framebuffer);
    }
    surface->used = ++m_copies;
    if (m_id != surface->texture) {
      m_id = surface->texture;
      m_samplerSet = false;
    }
    m_width = static_cast<uint32_t>(width);
    m_height = static_cast<uint32_t>(height);
  }

private:
  // A copy texture's texture at one size, and the framebuffer drawing into it (target).
  static constexpr int kCopySurfaces = 3;
  struct CopySurface {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t used = 0; // the texture's copy count at its last copy
  };

  bool is_surface(GLuint id) const noexcept {
    for (const CopySurface& surface : *m_surfaces) {
      if (surface.texture == id) {
        return true;
      }
    }
    return false;
  }

  // A BC1 level's bytes: its 4x4 blocks, 8 bytes each.
  uint64_t bc1_level_bytes(uint32_t level) const noexcept {
    const uint64_t width = std::max(m_width >> level, 1u);
    const uint64_t height = std::max(m_height >> level, 1u);
    return ((width + 3) / 4) * ((height + 3) / 4) * 8;
  }

  // A sampler's min filter as this texture takes it: a mipmapped one samples its first level alone
  // where it has no others (vitaGL would look for levels it was never given).
  GLint min_filter(uint16_t filter) const noexcept { return m_levels > 1 ? filter : plain_filter(filter); }
  static GLint plain_filter(uint16_t filter) noexcept {
    switch (filter) {
    case GL_NEAREST_MIPMAP_NEAREST:
    case GL_NEAREST_MIPMAP_LINEAR:
      return GL_NEAREST;
    case GL_LINEAR_MIPMAP_NEAREST:
    case GL_LINEAR_MIPMAP_LINEAR:
      return GL_LINEAR;
    default:
      return filter;
    }
  }

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
  uint32_t m_levels; // a BC1 texture's levels in m_cpu (and on the GPU); 1 for the rest
  GLuint m_id = 0;
  int64_t m_bytes = 0;
  Sampler m_sampler{};
  bool m_samplerSet = false;
  // A copy texture's, from its first copy (target); null for the rest.
  std::unique_ptr<std::array<CopySurface, kCopySurfaces>> m_surfaces;
  uint32_t m_copies = 0;
};

std::shared_ptr<Texture> make_texture(uint32_t width, uint32_t height, TexelFormat format,
                                      std::vector<uint8_t> texels, uint32_t levels) noexcept {
  return std::make_shared<Texture>(width, height, format, std::move(texels), levels);
}

std::shared_ptr<Texture> make_copy_texture() noexcept {
  return std::make_shared<Texture>(0, 0, TexelFormat::RGBA8, std::vector<uint8_t>(), 1);
}

uint64_t texture_bytes(const Texture& texture) noexcept { return texture.bytes(); }

namespace {
// For the log: generated programs compiled, failed, and the time spent compiling (on the compiler
// thread, by the programs linked so far).
struct ProgramStats {
  uint32_t compiled = 0; // built, from the compiler or the card
  uint32_t failed = 0;
  uint32_t queued = 0;      // sent to the compiler thread
  uint32_t precompiled = 0; // linked from the precompiled pack's GXPs instead
  double compileMs = 0.0;
  uint32_t loaded = 0;   // of them, loaded from the card
  uint32_t reloaded = 0; // read back but not linking: compiled again
  double loadMs = 0.0;   // the card thread's reads and the links
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

// A thread of its own for work the render worker must not wait for, its jobs one at a time in
// order: started with the first job and ended when none is left, so its stack is held only while
// there is work.
class JobThread {
public:
  // `core` and `vitaPriority`: the Vita core it keeps to and its own priority there
  // (thread::Options::vitaCore, vitaPriority; -1 and 0: any core, `priority`'s).
  JobThread(const char* name, thread::Priority priority, size_t stack, int core = -1, int vitaPriority = 0) noexcept
  : m_name(name), m_priority(priority), m_stack(stack), m_core(core), m_vitaPriority(vitaPriority) {}

  // False when no thread could be started: the job is not taken.
  bool submit(std::function<void()> job) noexcept {
    std::lock_guard lock{m_mutex};
    if (!m_running) {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, m_stack);
      pthread_t thread;
      m_running = pthread_create(&thread, &attr, &JobThread::run, this) == 0;
      if (m_running) {
        pthread_detach(thread);
      }
      pthread_attr_destroy(&attr);
    }
    if (m_running) {
      m_jobs.push_back(std::move(job));
    }
    return m_running;
  }

private:
  static void* run(void* arg) {
    JobThread& self = *static_cast<JobThread*>(arg);
    thread::set_current(
        {.name = self.m_name, .priority = self.m_priority, .vitaCore = self.m_core, .vitaPriority = self.m_vitaPriority});
    for (;;) {
      std::function<void()> job;
      {
        std::lock_guard lock{self.m_mutex};
        if (self.m_jobs.empty()) {
          self.m_running = false;
          return nullptr;
        }
        job = std::move(self.m_jobs.front());
        self.m_jobs.pop_front();
      }
      job();
    }
  }

  const char* m_name;
  thread::Priority m_priority;
  size_t m_stack;
  int m_core;
  int m_vitaPriority;
  std::mutex m_mutex;
  std::deque<std::function<void()>> m_jobs;
  bool m_running = false;
};

// SceShaccCg runs deep: it overflowed the render worker's 32 KiB stack (what VitaSDK gives a
// std::thread) at the 18th program of the first run.
constexpr size_t kCompileStack = 2 * 1024 * 1024;

// The Cg compiler (SceShaccCg, through vitashark) takes one to two seconds a program here, and the
// render worker used to wait each out: a room's first visit froze for as long as its new programs
// took (a run through Tallon Overworld: 16 of them, 27 s). A program's two sources compile on a
// thread of their own at a low priority, one program at a time, to GXPs; the render worker links
// them once done, the program's draws fixed-function meanwhile.
JobThread& compiler_thread() noexcept {
  static JobThread& jobs = *new JobThread("program compiler", thread::Priority::Low, kCompileStack);
  return jobs;
}

// The memory card's side of the programs: reading a binary back, writing one, removing one. The
// render worker did these itself, a file's existence asked at each program's first draw too, and
// with the game streaming a room from the disc image on the same card each waited behind its
// reads: 250 to 850 ms stalls at room changes (a run through Tallon Overworld, 7dfaad5).
JobThread& card_thread() noexcept {
  // On the render worker's core, as the other helpers at the busy threads' priority (thread.cpp), and
  // just below it there as the frame reaper is: at its priority a card read held the core for as long
  // as it ran (test 8: 16% busy while the room's programs loaded).
  static JobThread& jobs = *new JobThread("program cache", thread::Priority::Normal, 128 * 1024, 0, 170);
  return jobs;
}

struct CompileJob {
  std::string vertexSource;
  std::string fragmentSource;
  std::vector<uint8_t> vertexGxp;
  std::vector<uint8_t> fragmentGxp;
  std::string error;
  double ms = 0.0;
  bool packed = false; // its GXPs read from the precompiled pack, not compiled
  std::atomic<bool> done{false};
};

// A binary read back from the card (card thread): ok when its header matched.
struct LoadJob {
  uint64_t key = 0;
  uint32_t vertexLength = 0;
  uint32_t fragmentLength = 0;
  GLenum format = 0;
  std::vector<uint8_t> binary;
  bool ok = false;
  double ms = 0.0;
  std::atomic<bool> done{false};
};

// The Cg compiler's memory. vitaGL hands vitashark vglMalloc, which takes newlib's malloc first,
// so a compile's many allocations took the heap lock every other thread takes too, from the
// compiler thread at a low priority, often preempted while holding it. In a run through Tallon
// Overworld (cdeac54) the FIFO thread's sections that allocate for each draw (the [fifo path]
// line's immediate and mesh) went from about 12 to 40-85 ms a frame through each compile's ten
// seconds, and the game from 15-20 to 8-12 FPS; the others held. SceShaccCg allocates from a heap
// of its own instead, carved once from newlib's; one compile runs at a time (shark_lock). What
// does not fit goes to vglMalloc as before, and a free goes back where its address came from (what
// SceShaccCg allocated before the switch included).
constexpr size_t kCompilerHeap = 8 * 1024 * 1024;
uint8_t* g_compilerHeap = nullptr;
SceClibMspace g_compilerSpace = nullptr;
// For the log: the heap's bytes in use (peak since the start), and the allocations it had no room for.
std::atomic<uint32_t> g_compilerInUse{0};
std::atomic<uint32_t> g_compilerPeak{0};
std::atomic<uint32_t> g_compilerOverflows{0};

void* compiler_malloc(size_t size) {
  if (void* p = sceClibMspaceMalloc(g_compilerSpace, size)) {
    const auto usable = static_cast<uint32_t>(sceClibMspaceMallocUsableSize(p));
    const uint32_t inUse = g_compilerInUse.fetch_add(usable, std::memory_order_relaxed) + usable;
    if (inUse > g_compilerPeak.load(std::memory_order_relaxed)) {
      g_compilerPeak.store(inUse, std::memory_order_relaxed);
    }
    return p;
  }
  g_compilerOverflows.fetch_add(1, std::memory_order_relaxed);
  return vglMalloc(size);
}

void compiler_free(void* p) {
  const auto* bytes = static_cast<const uint8_t*>(p);
  if (p != nullptr && bytes >= g_compilerHeap && bytes < g_compilerHeap + kCompilerHeap) {
    g_compilerInUse.fetch_sub(sceClibMspaceMallocUsableSize(p), std::memory_order_relaxed);
    sceClibMspaceFree(g_compilerSpace, p);
  } else {
    vglFree(p);
  }
}

// After vglInit, whose shark_init gave SceShaccCg vglMalloc, before any compile.
void use_compiler_heap() noexcept {
  g_compilerHeap = static_cast<uint8_t*>(memalign(64, kCompilerHeap));
  g_compilerSpace = g_compilerHeap != nullptr ? sceClibMspaceCreate(g_compilerHeap, kCompilerHeap) : nullptr;
  if (g_compilerSpace == nullptr) {
    std::free(g_compilerHeap);
    g_compilerHeap = nullptr;
    Log.warn("no heap of its own for the shader compiler: it allocates from vglMalloc");
    return;
  }
  shark_set_allocators(compiler_malloc, compiler_free);
  sceShaccCgSetDefaultAllocator(compiler_malloc, compiler_free);
  Log.info("the shader compiler allocates from a heap of its own ({} MiB)", kCompilerHeap / (1024 * 1024));
}

// A source compiled to a GXP, with vitaGL's own settings for a compile (custom_shaders.c: fast
// optimisation, fast math and integers, full precision). vitashark reads the source's length from
// `size` (and writes the binary's there): 685daf0 and a48830e passed 0, an empty source, and every
// compile failed with "fatal internal error".
bool compile_gxp(const std::string& source, shark_type type, std::vector<uint8_t>& gxp, std::string& error) {
  auto size = static_cast<uint32_t>(source.size());
  const SceGxmProgram* program =
      shark_compile_shader_extended(source.c_str(), &size, type, SHARK_OPT_FAST, SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
  if (program != nullptr && size != 0) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(program);
    gxp.assign(bytes, bytes + size);
  } else {
    const SceShaccCgCompileOutput* output = shark_get_internal_compile_output();
    error = type == SHARK_VERTEX_SHADER ? "the vertex program did not compile" : "the fragment program did not compile";
    for (int i = 0; output != nullptr && i < output->diagnosticCount && i < 4; ++i) {
      if (output->diagnostics[i].message != nullptr) {
        error += "; ";
        error += output->diagnostics[i].message;
      }
    }
  }
  shark_clear_output();
  return !gxp.empty();
}

// Programs sent to the compiler thread since the start, and those it has finished (compiled or
// not): the overlay's "Compiling shaders" toast (program_compiles).
std::atomic<uint32_t> g_compilesSent{0};
std::atomic<uint32_t> g_compilesFinished{0};

// A job's sources to the compiler thread. Without a thread the job is done at once with nothing
// compiled, and the program stays fixed-function.
void send_to_compiler(const std::shared_ptr<CompileJob>& job) noexcept {
  g_compilesSent.fetch_add(1, std::memory_order_relaxed);
  const bool taken = compiler_thread().submit([job] {
    const auto start = std::chrono::steady_clock::now();
    if (compile_gxp(job->vertexSource, SHARK_VERTEX_SHADER, job->vertexGxp, job->error)) {
      compile_gxp(job->fragmentSource, SHARK_FRAGMENT_SHADER, job->fragmentGxp, job->error);
    }
    job->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g_compilesFinished.fetch_add(1, std::memory_order_release);
    job->done.store(true, std::memory_order_release);
  });
  if (!taken) {
    job->error = "no thread for the compiler";
    g_compilesFinished.fetch_add(1, std::memory_order_release);
    job->done.store(true, std::memory_order_release);
  }
}

// Programs compiled on the PC with the Vita's own compiler (tools/vita_precompile): the card's
// vita_programs/precompiled.bin, its index read at the start (key, then each GXP's place). A program
// there is linked from its GXPs, read on the card thread, rather than compiled. Read-only after
// initialize: the render worker and the FIFO thread (program_known) both look keys up.
struct PackEntry {
  uint32_t vertexOffset = 0;
  uint32_t vertexSize = 0;
  uint32_t fragmentOffset = 0;
  uint32_t fragmentSize = 0;
};
static_assert(sizeof(PackEntry) == 16);
std::string g_packPath;
std::unordered_map<uint64_t, PackEntry> g_pack;

void read_pack_index() noexcept {
  std::FILE* file = std::fopen(g_packPath.c_str(), "rb");
  if (file == nullptr) {
    return;
  }
  char magic[4] = {};
  uint32_t count = 0;
  if (std::fread(magic, sizeof(magic), 1, file) == 1 && std::memcmp(magic, "VPK1", sizeof(magic)) == 0 &&
      std::fread(&count, sizeof(count), 1, file) == 1 && count < (1u << 20)) {
    std::vector<uint8_t> table(static_cast<size_t>(count) * 24);
    if (std::fread(table.data(), table.size(), 1, file) == 1) {
      g_pack.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        PackEntry entry;
        std::memcpy(&key, table.data() + i * 24, sizeof(key));
        std::memcpy(&entry, table.data() + i * 24 + sizeof(key), sizeof(entry));
        g_pack.emplace(key, entry);
      }
    }
  }
  std::fclose(file);
}

// Card thread: a precompiled program's two GXPs from the pack, false when it does not give them back.
bool read_packed(const PackEntry& entry, CompileJob& job) noexcept {
  std::FILE* file = std::fopen(g_packPath.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  const auto read = [file](uint32_t offset, uint32_t size, std::vector<uint8_t>& gxp) {
    gxp.resize(size);
    return size >= 4 && size < (1u << 20) && std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
           std::fread(gxp.data(), size, 1, file) == 1 && std::memcmp(gxp.data(), "GXP", 4) == 0;
  };
  const bool ok = read(entry.vertexOffset, entry.vertexSize, job.vertexGxp) &&
                  read(entry.fragmentOffset, entry.fragmentSize, job.fragmentGxp);
  std::fclose(file);
  return ok;
}

// A program's sources compiled on the compiler thread, or with `packed` (the pack has it) its GXPs
// read from the pack on the card thread, compiled after all when the pack does not give them back.
std::shared_ptr<CompileJob> submit_compile(std::string vertexSource, std::string fragmentSource,
                                           const PackEntry* packed) noexcept {
  auto job = std::make_shared<CompileJob>();
  job->vertexSource = std::move(vertexSource);
  job->fragmentSource = std::move(fragmentSource);
  if (packed != nullptr) {
    job->packed = true;
    const bool taken = card_thread().submit([job, entry = *packed] {
      if (read_packed(entry, *job)) {
        job->done.store(true, std::memory_order_release);
        return;
      }
      job->packed = false;
      job->vertexGxp.clear();
      job->fragmentGxp.clear();
      send_to_compiler(job);
    });
    if (taken) {
      return job;
    }
    job->packed = false;
  }
  send_to_compiler(job);
  return job;
}

// A shader made from a GXP. vitaGL's glShaderBinary takes its own form of one (unserialize_shader):
// the count of the program's matrix uniforms and their indices (these programs have none) ahead of
// the GXP.
GLuint shader_from_gxp(GLenum type, const std::vector<uint8_t>& gxp) noexcept {
  std::vector<uint8_t> serialized(sizeof(uint32_t) + gxp.size(), 0);
  std::memcpy(serialized.data() + sizeof(uint32_t), gxp.data(), gxp.size());
  const GLuint shader = glCreateShader(type);
  glShaderBinary(1, &shader, 0, serialized.data(), static_cast<GLsizei>(serialized.size()));
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

// The keys of the programs on the card, from its folder read once at the start (index_card); a
// program's first draw asks this, not the card (render worker). And those being compiled, by a
// draw's program or the warm-up's, so that neither starts a second compile of one. The FIFO thread
// asks both too (program_known): under g_keysMutex.
std::unordered_set<uint64_t> g_cardKeys;
std::unordered_set<uint64_t> g_compilingKeys;
std::mutex g_keysMutex;

// The warm-up's material records on the card (gx/vita_tev.cpp): read whole at the start; new ones
// gathered and appended by the card thread, a batch a job (their materials come in bursts, a room's
// at its first draws).
std::string g_materialsPath;
std::vector<uint8_t> g_savedMaterials;
std::mutex g_materialsMutex;
std::vector<uint8_t> g_materialsPending;
bool g_materialsQueued = false;

void read_materials() noexcept {
  std::FILE* file = std::fopen(g_materialsPath.c_str(), "rb");
  if (file == nullptr) {
    return;
  }
  if (std::fseek(file, 0, SEEK_END) == 0) {
    const long size = std::ftell(file);
    if (size > 0 && size < (32l << 20) && std::fseek(file, 0, SEEK_SET) == 0) {
      g_savedMaterials.resize(static_cast<size_t>(size));
      if (std::fread(g_savedMaterials.data(), g_savedMaterials.size(), 1, file) != 1) {
        g_savedMaterials.clear();
      }
    }
  }
  std::fclose(file);
}

// Card thread: the records gathered since the last batch, appended.
void write_materials() noexcept {
  std::vector<uint8_t> batch;
  {
    std::lock_guard lock{g_materialsMutex};
    batch.swap(g_materialsPending);
    g_materialsQueued = false;
  }
  if (std::FILE* file = std::fopen(g_materialsPath.c_str(), "ab"); file != nullptr) {
    std::fwrite(batch.data(), batch.size(), 1, file);
    std::fclose(file);
  }
}

void index_card() noexcept {
  std::error_code ec;
  std::filesystem::directory_iterator it{g_programCacheDir, ec};
  for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
    // <16 hex digits>.gxp
    const std::string name = it->path().filename().string();
    uint64_t key = 0;
    if (name.size() == 20 && name.ends_with(".gxp") &&
        std::from_chars(name.data(), name.data() + 16, key, 16).ec == std::errc{}) {
      g_cardKeys.insert(key);
    }
  }
}

// The build that wrote the card's program binaries (vita_programs/build), and this one. The user
// found the map's GPU crash came from the shaders an earlier build had left in vita_programs after
// a new build was installed, and went away with them deleted (VITA_PHASE2.md, "The map's GPU crash:
// stale shaders"). How an old binary differs from a new compile is not known: a binary's key names
// both its sources, and the card's binaries were byte-identical to fresh compiles on Nov 2. So a
// new build starts without them, as the user's fix did: they are never indexed (nothing reads
// one), and the card thread deletes them first, before it writes any new one. The material records
// stay, and the warm-up compiles their programs again from the start, one batch.
std::string program_binaries_build() noexcept {
  std::string build;
  if (std::FILE* file = std::fopen((g_programCacheDir + "/build").c_str(), "rb"); file != nullptr) {
    char text[64] = {};
    build.assign(text, std::fread(text, 1, sizeof(text) - 1, file));
    std::fclose(file);
  }
  return build;
}

std::string this_build() noexcept { return kMpBuildRevision != nullptr ? std::string{kMpBuildRevision} : std::string{}; }

// Card thread: the program binaries another build left deleted, then this build named as theirs.
void delete_program_binaries(const std::string& previous, const std::string& current) noexcept {
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  std::filesystem::directory_iterator it{g_programCacheDir, ec};
  for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
    // <16 hex digits>.gxp, and one cut short while written aside (.gxp.tmp)
    const std::string name = it->path().filename().string();
    uint64_t key = 0;
    if (name.ends_with(".gxp.tmp") || (name.size() == 20 && name.ends_with(".gxp") &&
                                       std::from_chars(name.data(), name.data() + 16, key, 16).ec == std::errc{})) {
      files.push_back(it->path());
    }
  }
  uint32_t removed = 0;
  for (const auto& path : files) {
    std::error_code gone;
    removed += std::filesystem::remove(path, gone) ? 1 : 0;
  }
  if (std::FILE* file = std::fopen((g_programCacheDir + "/build").c_str(), "wb"); file != nullptr) {
    std::fwrite(current.data(), current.size(), 1, file);
    std::fclose(file);
  }
  Log.info("program binaries on the card from {}: {} of {} deleted in {:.0f} ms; build {} writes its own",
           previous.empty() ? "a build that named none" : fmt::format("build {}", previous), removed, files.size(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), current);
}

// vitaGL's own cache of the shaders behind its fixed-function calls: the overlay's, and those of
// every draw whose generated program is not ready (after a build that changes the generated
// programs, the map's 1,700 draws a frame until theirs compile). vitaGL keeps it in
// ux0:data/shader_cache/v<N>/v and /f, a folder every vitaGL app shares, whatever
// vglSetShaderCachePath says (that one is glCompileShader's, which nothing here calls). It writes a
// shader there in place as soon as it is compiled, without checking that the compile worked, and
// hands the shader patcher whatever bytes a file holds: a file cut short (a crash or a quit during
// the write) or a failed compile's is a broken shader in every session after, until it is deleted.
// The user found the map's GPU crash went away with the card's old shaders deleted
// (VITA_PHASE2.md, "The map's GPU crash: stale shaders").
constexpr const char* kFixedFunctionCache = "ux0:data/shader_cache";

// Whether the file at `path`, `size` bytes, is one whole GXP: its magic and version, and its
// header's size (at byte 8: every byte of the program, unpadded) the file's own.
bool whole_gxp(const std::filesystem::path& path, uintmax_t size) noexcept {
  if (size < 16 || size > kMaxBinary) {
    return false;
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return true; // vitaGL cannot read it either, and compiles the shader again
  }
  uint8_t head[12] = {};
  const bool read = std::fread(head, sizeof(head), 1, file) == 1;
  std::fclose(file);
  uint32_t field = 0;
  std::memcpy(&field, head + 8, sizeof(field));
  return read && std::memcmp(head, "GXP", 4) == 0 && head[4] == 1 && field == size;
}

// Before vitaGL reads any, the fixed-function cache's files that are not whole GXPs deleted
// (vitaGL compiles a shader again when a draw needs it), and the count logged.
void check_fixed_function_cache() noexcept {
  const auto start = std::chrono::steady_clock::now();
  uint32_t checked = 0;
  std::vector<std::pair<std::filesystem::path, uintmax_t>> broken;
  std::error_code ec;
  std::filesystem::directory_iterator versions{kFixedFunctionCache, ec};
  for (; !ec && versions != std::filesystem::directory_iterator{}; versions.increment(ec)) {
    for (const char* stage : {"v", "f"}) {
      std::error_code listed;
      std::filesystem::directory_iterator it{versions->path() / stage, listed};
      for (; !listed && it != std::filesystem::directory_iterator{}; it.increment(listed)) {
        std::error_code sized;
        const uintmax_t size = it->file_size(sized);
        if (sized || !it->path().filename().string().ends_with(".gxp")) {
          continue;
        }
        ++checked;
        if (!whole_gxp(it->path(), size)) {
          broken.emplace_back(it->path(), size);
        }
      }
    }
  }
  // Deleted once listed: not while the folder is read.
  std::string names;
  uint32_t removed = 0;
  for (const auto& [path, size] : broken) {
    std::error_code gone;
    if (!std::filesystem::remove(path, gone)) {
      continue;
    }
    if (++removed <= 8) {
      const std::filesystem::path& folder = path.parent_path();
      names += fmt::format("{}{}/{}/{} ({} bytes)", names.empty() ? "" : ", ",
                           folder.parent_path().filename().string(), folder.filename().string(),
                           path.filename().string(), size);
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (removed == 0) {
    Log.info("vitaGL's fixed-function shaders ({}): {} on the card, all whole ({:.0f} ms)", kFixedFunctionCache,
             checked, ms);
  } else {
    Log.warn("vitaGL's fixed-function shaders ({}): {} on the card, {} not whole GXPs deleted ({:.0f} ms): {}{}",
             kFixedFunctionCache, checked, removed, ms, names, removed > 8 ? ", ..." : "");
  }
}

// Card thread: a program's binary read back, ok when its header matches the sources' lengths.
void read_cached(LoadJob& job) noexcept {
  const auto start = std::chrono::steady_clock::now();
  if (std::FILE* file = std::fopen(cache_path(job.key).c_str(), "rb"); file != nullptr) {
    uint32_t header[5] = {};
    job.ok = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == kCacheMagic &&
             header[1] == job.vertexLength && header[2] == job.fragmentLength && header[4] != 0 &&
             header[4] < kMaxBinary;
    if (job.ok) {
      job.format = header[3];
      job.binary.resize(header[4]);
      job.ok = std::fread(job.binary.data(), job.binary.size(), 1, file) == 1;
    }
    std::fclose(file);
  }
  job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Card thread: a binary written aside and renamed, so a file cut short (the power, a crash) is
// never read.
void write_cached(uint64_t key, uint32_t vertexLength, uint32_t fragmentLength, GLenum format,
                  const std::vector<uint8_t>& binary) noexcept {
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

// Render worker: a linked program's binary in vitaGL's format, for the card. vitaGL's
// glGetProgramBinary writes the whole binary whatever the buffer's size, so the buffer is the
// length vitaGL gives first, with room to spare.
bool program_binary(GLuint program, GLenum& format, std::vector<uint8_t>& binary) noexcept {
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
} // namespace

class Program : public Kept {
public:
  Program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept
  : m_vertexSource(std::move(vertexSource)), m_fragmentSource(std::move(fragmentSource)), m_key(key) {}
  ~Program() {
    if (m_id != 0) {
      const GLuint id = m_id;
      gfx::render_worker::enqueue_work([id] { glDeleteProgram(id); });
    }
  }

  // Render worker: true once linked. A program not tried yet is read back from the card (the card's
  // index has it) or compiled, both on threads of their own (one the warm-up is compiling waits for
  // it, then is read back); done, it is linked while the frame's budget allows (a few milliseconds
  // each). Its draws are fixed-function till then.
  bool ready(int& buildBudget) noexcept {
    if (m_state == State::Pending) {
      bool onCard = false;
      bool compiling = false;
      {
        std::lock_guard lock{g_keysMutex};
        onCard = g_cardKeys.contains(m_key);
        compiling = g_compilingKeys.contains(m_key);
      }
      if (onCard) {
        load();
      } else if (!compiling) {
        compile();
      }
    }
    if (m_state == State::Loading && buildBudget > 0 && m_load->done.load(std::memory_order_acquire)) {
      --buildBudget;
      on_compile_stack(&Program::link_loaded);
    }
    if (m_state == State::Compiling && buildBudget > 0 && m_job->done.load(std::memory_order_acquire)) {
      --buildBudget;
      on_compile_stack(&Program::link_compiled);
    }
    return m_state == State::Ready;
  }

  GLuint id() const noexcept { return m_id; }
  // Linked, or failed: nothing more to build.
  bool settled() const noexcept { return m_state == State::Ready || m_state == State::Failed; }
  GLint attr(int index) const noexcept { return m_attrs[index]; }
  GLint samplers[kSamplers] = {-1, -1, -1, -1, -1, -1, -1, -1};

  // Render worker, this program in use: sets its constants unless they are the rows it was sent
  // last. vitaGL keeps each program's uniforms, and sends a side's uniform buffer at a draw only
  // after a glUniform call (or glUseProgram) marked that side, so an unchanged side costs nothing.
  // The FIFO thread points a draw that repeats its program's last constants in a frame at the same
  // rows; a program that becomes ready within a frame sends its first draw's rows, as none were
  // sent before, and every change after in order.
  void set_constants(const ProgramInputs& inputs, const Frame& frame) noexcept {
    upload(m_vertexLocation, m_vertexSent, frame, inputs.vertexAt, inputs.vertexCount, g_uniformStats.vertex);
    upload(m_constantsLocation, m_constantsSent, frame, inputs.constantsAt, inputs.constantCount,
           g_uniformStats.fragment);
  }

  // Render worker, this program in use: its fragment constants from `rows`, sent now (the copy
  // program's colour matrix, at each copy; not rows of a frame's).
  void set_rows(const float (*rows)[4], int count) noexcept {
    if (m_constantsLocation != kNoUniform) {
      glUniform4fv(m_constantsLocation, count, rows[0]);
      m_constantsSent = {};
    }
  }

private:
  enum class State : uint8_t { Pending, Loading, Compiling, Ready, Failed };

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

  // Render worker: a step that links (vitaGL's link, and a load's own) on a thread with the
  // compiler's stack, as every build ran before the compiles left the render worker, while the
  // render worker waits for it: the GL calls stay one at a time, and vitaGL keeps no per-thread
  // context.
  void on_compile_stack(void (Program::*step)()) noexcept {
    struct Call {
      Program* self;
      void (Program::*step)();
    } call{this, step};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kCompileStack);
    pthread_t thread;
    const auto run = [](void* arg) -> void* {
      const Call& c = *static_cast<Call*>(arg);
      (c.self->*c.step)();
      return nullptr;
    };
    if (pthread_create(&thread, &attr, run, &call) == 0) {
      pthread_join(thread, nullptr);
    } else {
      Log.error("no thread for a program's link: it stays fixed-function");
      m_state = State::Failed;
      ++g_programStats.failed;
    }
    pthread_attr_destroy(&attr);
  }

  // Its sources to the compiler thread, or its GXPs from the precompiled pack when it has them; the
  // draws are fixed-function till its GXPs are linked.
  void compile() noexcept {
    {
      std::lock_guard lock{g_keysMutex};
      g_compilingKeys.insert(m_key);
    }
    const auto packed = g_pack.find(m_key);
    const bool inPack = packed != g_pack.end();
    m_job = submit_compile(std::move(m_vertexSource), std::move(m_fragmentSource), inPack ? &packed->second : nullptr);
    ++(inPack ? g_programStats.precompiled : g_programStats.queued);
    m_state = State::Compiling;
  }

  // A program compiled in an earlier session: its binary read back on the card thread (without
  // one, compiled).
  void load() noexcept {
    auto job = std::make_shared<LoadJob>();
    job->key = m_key;
    job->vertexLength = m_vertexLength;
    job->fragmentLength = m_fragmentLength;
    if (!card_thread().submit([job] {
          read_cached(*job);
          job->done.store(true, std::memory_order_release);
        })) {
      compile();
      return;
    }
    m_load = std::move(job);
    m_state = State::Loading;
  }

  // A binary from the card: glProgramBinary rebuilds both shaders from their GXPs and links them.
  // One the card did not give back, or that no longer links (another vitaGL, a damaged card), goes
  // from the card and is compiled now.
  void link_loaded() noexcept {
    const auto start = std::chrono::steady_clock::now();
    LoadJob& job = *m_load;
    if (job.ok) {
      const GLuint program = glCreateProgram();
      glProgramBinary(program, job.format, job.binary.data(), static_cast<GLsizei>(job.binary.size()));
      GLint ok = GL_FALSE;
      glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (ok == GL_TRUE) {
        m_id = program;
      } else {
        glDeleteProgram(program);
      }
    }
    g_programStats.loadMs +=
        job.ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ++g_programStats.loaded;
    m_load.reset();
    if (m_id == 0) {
      ++g_programStats.reloaded;
      {
        std::lock_guard lock{g_keysMutex};
        g_cardKeys.erase(m_key);
      }
      card_thread().submit([key = m_key] {
        std::error_code ec;
        std::filesystem::remove(cache_path(key), ec);
      });
      compile();
      return;
    }
    finish({}, m_vertexSource, m_fragmentSource);
  }

  // The compiler thread's GXPs as shaders, linked; its binary to the card thread, for the next
  // session.
  void link_compiled() noexcept {
    CompileJob& job = *m_job;
    std::string error = job.error;
    if (!job.vertexGxp.empty() && !job.fragmentGxp.empty()) {
      // No glBindAttribLocation: glLinkProgram looks every bound name up in the compiled program
      // and passes the result on unchecked, so a name the compiler dropped would crash it. The
      // locations are asked for after the link instead.
      const GLuint vs = shader_from_gxp(GL_CG_VERTEX_SHADER_EXT, job.vertexGxp);
      const GLuint fs = shader_from_gxp(GL_CG_FRAGMENT_SHADER_EXT, job.fragmentGxp);
      const GLuint program = glCreateProgram();
      glAttachShader(program, vs);
      glAttachShader(program, fs);
      glLinkProgram(program);
      GLint ok = GL_FALSE;
      glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (ok == GL_TRUE) {
        m_id = program;
        GLenum format = 0;
        std::vector<uint8_t> binary;
        if (program_binary(m_id, format, binary)) {
          {
            std::lock_guard lock{g_keysMutex};
            g_cardKeys.insert(m_key);
          }
          card_thread().submit([key = m_key, vertexLength = m_vertexLength, fragmentLength = m_fragmentLength,
                                format, binary = std::move(binary)] {
            write_cached(key, vertexLength, fragmentLength, format, binary);
          });
        }
      } else {
        error = "the link failed";
        glDeleteProgram(program);
      }
      glDeleteShader(vs);
      glDeleteShader(fs);
    }
    if (m_id == 0 && job.packed) {
      // The pack's GXPs did not link: compiled here after all (it stays among those compiling).
      ++g_programStats.reloaded;
      std::string vertexSource = std::move(job.vertexSource);
      std::string fragmentSource = std::move(job.fragmentSource);
      m_job = submit_compile(std::move(vertexSource), std::move(fragmentSource), nullptr);
      ++g_programStats.queued;
      return;
    }
    g_programStats.compileMs += job.ms;
    {
      std::lock_guard lock{g_keysMutex};
      g_compilingKeys.erase(m_key);
    }
    finish(error, job.vertexSource, job.fragmentSource);
    m_job.reset();
  }

  // After a load or a link: the program's attribute and uniform locations and its samplers' units,
  // or the failure (the first few logged with their sources).
  void finish(const std::string& error, const std::string& vertexSource, const std::string& fragmentSource) noexcept {
    if (m_id == 0) {
      m_state = State::Failed;
      ++g_programStats.failed;
      if (g_programStats.failuresLogged++ < 3) {
        Log.error("a generated program did not build: {}\n-- vertex --\n{}\n-- fragment --\n{}", error,
                  vertexSource, fragmentSource);
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
  // The sources' lengths, which a binary on the card must match (its header).
  uint32_t m_vertexLength = static_cast<uint32_t>(m_vertexSource.size());
  uint32_t m_fragmentLength = static_cast<uint32_t>(m_fragmentSource.size());
  std::shared_ptr<LoadJob> m_load;
  std::shared_ptr<CompileJob> m_job;
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

const std::vector<uint8_t>& saved_materials() noexcept { return g_savedMaterials; }

void save_material(const void* record, size_t size) noexcept {
  const auto length = static_cast<uint32_t>(size);
  const auto* lengthBytes = reinterpret_cast<const uint8_t*>(&length);
  const auto* bytes = static_cast<const uint8_t*>(record);
  std::lock_guard lock{g_materialsMutex};
  g_materialsPending.insert(g_materialsPending.end(), lengthBytes, lengthBytes + sizeof(length));
  g_materialsPending.insert(g_materialsPending.end(), bytes, bytes + size);
  if (!g_materialsQueued) {
    g_materialsQueued = card_thread().submit(write_materials);
  }
}

namespace {
// The warm-up's programs (gx/vita_tev.cpp): the FIFO thread's, waiting (kWarmUpWaiting at most:
// warm_up_room; their sources about 9 KiB each), and the one the render worker builds
// (warm_up_step), dropped once its binary is on the card, from where the draw that needs it reads it
// back. The render worker sends them to the compiler thread one at a time, so a draw's own compile
// waits behind one at most however many wait here.
struct WarmUpJob {
  uint64_t key = 0;
  std::string vertexSource;
  std::string fragmentSource;
};
constexpr size_t kWarmUpWaiting = 64;
std::mutex g_warmUpMutex;
std::deque<WarmUpJob> g_warmUpQueue;
std::shared_ptr<Program> g_warmingUp;
// For the toast (program_compiles): the warm-up's programs waiting their turn (queued, not yet sent
// to the compiler thread nor found built), and those found built by their turn.
std::atomic<uint32_t> g_warmUpAhead{0};
std::atomic<uint32_t> g_warmUpFoundBuilt{0};
// For the log: the warm-up's programs built, failed, and found built or being built by their turn.
struct WarmUpStats {
  uint32_t built = 0;
  uint32_t failed = 0;
  uint32_t skipped = 0;
};
WarmUpStats g_warmUpStats;

// Render worker, each present: the warm-up's program a step further (linked once compiled, its
// binary to the card, then dropped), or the next one waiting started.
void warm_up_step() noexcept {
  if (g_warmingUp != nullptr) {
    int budget = 1;
    g_warmingUp->ready(budget);
    if (!g_warmingUp->settled()) {
      return;
    }
    ++(g_warmingUp->id() != 0 ? g_warmUpStats.built : g_warmUpStats.failed);
    g_warmingUp.reset();
    // Its link made it the program in use (to set its samplers): the overlay draws fixed-function.
    glUseProgram(0);
  }
  for (;;) {
    WarmUpJob job;
    {
      std::lock_guard lock{g_warmUpMutex};
      if (g_warmUpQueue.empty()) {
        return;
      }
      job = std::move(g_warmUpQueue.front());
      g_warmUpQueue.pop_front();
    }
    bool known = false;
    {
      std::lock_guard lock{g_keysMutex};
      known = g_cardKeys.contains(job.key) || g_compilingKeys.contains(job.key);
    }
    if (known) {
      ++g_warmUpStats.skipped;
      g_warmUpFoundBuilt.fetch_add(1, std::memory_order_release);
      g_warmUpAhead.fetch_sub(1, std::memory_order_relaxed);
      continue;
    }
    g_warmingUp = std::make_shared<Program>(std::move(job.vertexSource), std::move(job.fragmentSource), job.key);
    int budget = 0;
    g_warmingUp->ready(budget); // its sources to the compiler thread
    g_warmUpAhead.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
}
} // namespace

bool program_known(uint64_t key) noexcept {
  // One in the precompiled pack is linked at its first draw, in a frame or two: nothing to warm.
  if (g_pack.contains(key)) {
    return true;
  }
  {
    std::lock_guard lock{g_keysMutex};
    if (g_cardKeys.contains(key) || g_compilingKeys.contains(key)) {
      return true;
    }
  }
  std::lock_guard lock{g_warmUpMutex};
  return std::any_of(g_warmUpQueue.begin(), g_warmUpQueue.end(), [key](const WarmUpJob& job) { return job.key == key; });
}

bool warm_up_room() noexcept {
  std::lock_guard lock{g_warmUpMutex};
  return g_warmUpQueue.size() < kWarmUpWaiting;
}

void warm_up(uint64_t key, std::string vertexSource, std::string fragmentSource) noexcept {
  std::lock_guard lock{g_warmUpMutex};
  g_warmUpQueue.push_back({key, std::move(vertexSource), std::move(fragmentSource)});
  g_warmUpAhead.fetch_add(1, std::memory_order_relaxed);
}

namespace {
bool same_unit(const DrawUnit& a, const DrawUnit& b) noexcept {
  return a.texture == b.texture && std::memcmp(a.matrix, b.matrix, sizeof(a.matrix)) == 0 && a.sampler == b.sampler &&
         a.envMode == b.envMode && a.coord == b.coord;
}

// A program's rows as two draws point at them: the same rows, or the same values recorded twice.
bool same_rows(uint32_t a, uint32_t b, uint8_t count) noexcept {
  return count == 0 || a == b ||
         std::memcmp(g_recording.constants[a].data(), g_recording.constants[b].data(), sizeof(float[4]) * count) == 0;
}

// `draw` joins the frame's last draw where both draw the frame's own vertices, the second's right
// after the first's in the frame's arrays, as lists (GL's lines or triangles), and nothing else
// about them differs: matrices, units, state, program, its textures and rows, viewport, scissor,
// width. Drawn as one, GXM rasterises their primitives in the same order. The map draws each room's
// outlines and faces a few at a time: Chozo Ruins' was some 5,700 draws a frame, recorded at 1 KiB
// each, and a frame's draws grew past what the heap had in one piece (0.42.0 test 3,
// std::bad_alloc on the FIFO thread), at 9 FPS.
bool merge_into_last(const Draw& draw) noexcept {
  const FrameMesh& b = draw.frameMesh;
  if (draw.mesh != nullptr || b.vertexCount == 0 || b.primitive == MeshPrimitive::Points || draw.clear ||
      draw.copyTo != nullptr || g_recording.draws.empty()) {
    return false;
  }
  Draw& last = g_recording.draws.back();
  FrameMesh& a = last.frameMesh;
  if (last.mesh != nullptr || a.vertexCount == 0 || last.clear || last.copyTo != nullptr || a.kind != b.kind ||
      a.primitive != b.primitive || a.vertexAt + a.vertexCount != b.vertexAt ||
      (a.indexCount != 0) != (b.indexCount != 0) || last.width != draw.width) {
    return false;
  }
  // Indices are the draw's own vertices' (from 0): the second's move past the first's, in 16 bits.
  if (b.indexCount != 0 && (a.indexAt + a.indexCount != b.indexAt || a.vertexCount + b.vertexCount > 0x10000)) {
    return false;
  }
  if (std::memcmp(last.modelView, draw.modelView, sizeof(draw.modelView)) != 0 ||
      std::memcmp(last.projection, draw.projection, sizeof(draw.projection)) != 0 || !(last.state == draw.state) ||
      !(last.viewport == draw.viewport) || !(last.scissor == draw.scissor)) {
    return false;
  }
  for (int unit = 0; unit < kTexUnits; ++unit) {
    if (!same_unit(last.units[unit], draw.units[unit])) {
      return false;
    }
  }
  const ProgramInputs& p = last.program;
  const ProgramInputs& q = draw.program;
  if (p.program != q.program || !(p.state == q.state) || p.vertexCount != q.vertexCount ||
      p.constantCount != q.constantCount) {
    return false;
  }
  for (int m = 0; m < kSamplers; ++m) {
    if (p.textures[m] != q.textures[m] || !(p.samplers[m] == q.samplers[m])) {
      return false;
    }
  }
  if (!same_rows(p.vertexAt, q.vertexAt, p.vertexCount) || !same_rows(p.constantsAt, q.constantsAt, p.constantCount)) {
    return false;
  }
  if (b.indexCount != 0) {
    uint16_t* indices = g_recording.indices.at(b.indexAt);
    for (uint32_t i = 0; i < b.indexCount; ++i) {
      indices[i] = static_cast<uint16_t>(indices[i] + a.vertexCount);
    }
    a.indexCount += b.indexCount;
  }
  a.vertexCount += b.vertexCount;
  return true;
}
} // namespace

void record(Draw&& draw) noexcept {
  if (merge_into_last(draw)) {
    ++g_recording.merged;
    return;
  }
  g_recording.copies += draw.copyTo != nullptr ? 1 : 0;
  g_recording.draws.push_back(draw);
}

namespace {
template <typename T>
T* keep_in_frame(const std::shared_ptr<T>& object) noexcept {
  T* const raw = object.get();
  if (raw != nullptr && raw->keptIn != g_recording.serial) {
    raw->keptIn = g_recording.serial;
    g_recording.kept.push_back(object);
  }
  return raw;
}
} // namespace

Mesh* keep(const std::shared_ptr<Mesh>& object) noexcept { return keep_in_frame(object); }
Texture* keep(const std::shared_ptr<Texture>& object) noexcept { return keep_in_frame(object); }
Program* keep(const std::shared_ptr<Program>& object) noexcept { return keep_in_frame(object); }

uint32_t record_constants(const float (*rows)[4], int count) noexcept {
  // As one block: a lit program's rows are 30 or more a draw.
  std::vector<std::array<float, 4>>& constants = g_recording.constants;
  const auto at = static_cast<uint32_t>(constants.size());
  static_assert(sizeof(std::array<float, 4>) == sizeof(float[4]));
  constants.resize(at + static_cast<size_t>(count));
  std::memcpy(constants[at].data(), rows, sizeof(float[4]) * static_cast<size_t>(count));
  return at;
}

const float* recorded_constants(uint32_t at) noexcept { return g_recording.constants[at].data(); }

uint32_t recording_serial() noexcept { return g_recording.serial; }

uint32_t recorded_draw_count() noexcept { return static_cast<uint32_t>(g_recording.draws.size()); }

Vertex* frame_vertices(uint32_t count, uint32_t& at) noexcept { return g_recording.vertices.grow(count, at); }

void trim_frame_vertices(uint32_t at) noexcept { g_recording.vertices.trim(at); }

uint32_t frame_indices(const uint16_t* indices, uint32_t count) noexcept {
  uint32_t at = 0;
  std::memcpy(g_recording.indices.grow(count, at), indices, sizeof(uint16_t) * count);
  return at;
}

void end_frame_draws() noexcept {
  const size_t drawn = g_recording.draws.size();
  const size_t rows = g_recording.constants.size();
  const size_t kept = g_recording.kept.size();
  const uint32_t vertices = g_recording.vertices.size();
  const uint32_t indices = g_recording.indices.size();
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
  g_recording.kept.reserve(kept);
  g_recording.vertices.reserve(vertices);
  g_recording.indices.reserve(indices);
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
// The reaper runs above the program compiler: at the same priority (the lowest, as it was), a
// compile (a second or so) kept it off its core, the frames it had not destroyed held their
// arrays, none came back to the spares, and each new frame took megabytes more from the heap
// until it ran out (tallon14: 31 MiB in a second, then std::bad_alloc on the FIFO thread). And a
// frame it is two behind on is destroyed by the caller instead, so their memory stays bounded.
class FrameReaper {
public:
  void reap(Frame frame) noexcept {
    {
      std::lock_guard lock{m_mutex};
      if (!m_started) {
        m_started = true;
        std::thread([this] { run(); }).detach();
      }
      if (m_frames.size() < kBehind) {
        m_frames.push_back(std::move(frame));
        m_ready.notify_one();
        return;
      }
      ++m_destroyedHere;
    }
    // `frame` is destroyed here, on the caller's thread.
  }

  // For the log: frames destroyed by reap's caller since the last call.
  uint32_t take_destroyed_here() noexcept {
    std::lock_guard lock{m_mutex};
    return std::exchange(m_destroyedHere, 0);
  }

private:
  static constexpr size_t kBehind = 2;

  void run() noexcept {
    // On the render worker's core, just below it (and above the compiler): at the FIFO processor's
    // priority it took the FIFO's core 6% of the time (0.42.0 test 7), each frame's destruction run
    // through while the FIFO waited.
    thread::set_current({.name = "frame reaper", .priority = thread::Priority::Normal, .vitaCore = 0,
                         .vitaPriority = 170});
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
  uint32_t m_destroyedHere = 0;
};
FrameReaper g_reaper;

// For the log: presents and draws since the last report (render worker).
struct PresentStats {
  uint32_t presents = 0;
  uint32_t framesWithDraws = 0;
  uint64_t draws = 0;
  uint64_t merged = 0; // draws recorded into the one before them (merge_into_last)
  // Draws with their generated program, and draws whose program was not ready (fixed-function).
  uint64_t programDraws = 0;
  uint64_t programPending = 0;
  // EFB copies made on the GPU, the pixels they wrote, the render worker's time on them (the GPU's
  // is in the frame's), and copies left unmade while the copy program was being built.
  uint32_t copies = 0;
  uint64_t copyPixels = 0;
  uint64_t copyUs = 0;
  uint32_t copiesWaiting = 0;
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
  if (force || state.blendSrc != current.blendSrc || state.blendDst != current.blendDst ||
      state.alphaBlendOwn != current.alphaBlendOwn || state.alphaBlendSrc != current.alphaBlendSrc ||
      state.alphaBlendDst != current.alphaBlendDst) {
    if (state.alphaBlendOwn) {
      glBlendFuncSeparate(state.blendSrc, state.blendDst, state.alphaBlendSrc, state.alphaBlendDst);
    } else {
      glBlendFunc(state.blendSrc, state.blendDst);
    }
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

// Generated programs linked a frame at most, from the card thread's binaries or the compiler
// thread's GXPs: a few milliseconds each.
constexpr int kBuildsPerFrame = 4;

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

// Binds a Mesh (render worker), counting for the log what its path and kind upload.
void bind_mesh(Mesh& mesh, DrawPath path, Section section) noexcept {
  const bool fresh = !mesh.uploaded();
  const auto count = static_cast<uint32_t>(mesh.bind());
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
  if (fresh && path == kPathFixed && count > g_clock.largestFixedVertices) {
    g_clock.largestFixedVertices = count;
    g_clock.largestFixedKind = mesh.kind();
  }
}

// Binds a draw's mesh (render worker) and gives what its arrays' offsets are from: its Mesh's
// (bind_mesh), or for a mesh of the frame's own no buffer, its vertices in the frame, counted for
// the log as a Mesh in client memory is (vitaGL copies them at each draw).
const uint8_t* bind_draw_mesh(const Draw& draw, const Frame& frame, DrawPath path, Section section) noexcept {
  if (draw.mesh != nullptr) {
    bind_mesh(*draw.mesh, path, section);
    return draw.mesh->base();
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  const FrameMesh& mesh = draw.frameMesh;
  const uint64_t us = g_clock.mark(section);
  if (g_clock.on) {
    SectionClock::Uploads& uploads = g_clock.uploads[path][static_cast<int>(mesh.kind)];
    ++uploads.draws;
    uploads.us += us;
    ++uploads.made;
    uploads.bytes += static_cast<uint64_t>(mesh.vertexCount) * sizeof(Vertex);
  }
  if (path == kPathFixed && mesh.vertexCount > g_clock.largestFixedVertices) {
    g_clock.largestFixedVertices = mesh.vertexCount;
    g_clock.largestFixedKind = mesh.kind;
  }
  return reinterpret_cast<const uint8_t*>(frame.vertices.at(mesh.vertexAt));
}

// Draws a draw's mesh, its arrays set (render worker): a mesh of the frame's own by its indices
// among the frame's, or its vertices in order.
void draw_mesh(const Draw& draw, const Frame& frame) noexcept {
  if (draw.mesh != nullptr) {
    draw.mesh->draw(draw.mesh->count());
    return;
  }
  const FrameMesh& mesh = draw.frameMesh;
  const GLenum mode = gl_mode(mesh.primitive);
  if (mesh.indexCount != 0) {
    glDrawRangeElements(mode, 0, mesh.vertexCount - 1, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_SHORT,
                        frame.indices.at(mesh.indexAt));
  } else {
    glDrawArrays(mode, 0, static_cast<GLsizei>(mesh.vertexCount));
  }
}

// Makes `program` the one in use (render worker). Leaving the fixed-function path turns its arrays
// off; a program switch turns on the generic arrays the new one reads.
void use_program(Program& program, GLuint& usedProgram, uint32_t& attribsOn) noexcept {
  if (usedProgram == program.id()) {
    return;
  }
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

// The EFB copies' program (copy_on_gpu): the screen's texel through a colour matrix, its four rows
// and then an offset in uConst (copy_rows). Built as the generated programs are (Program: compiled
// on the compiler thread, then read back from the card), from the first frame on (draw_world), so
// it is ready by the first copy that matters (the pause screen copies the world once, at the pause).
constexpr std::string_view kCopyVertex =
    "void main(float2 aPos, float2 aUv0, float4 out vPos : POSITION, float2 out vUv0 : TEXCOORD0) {\n"
    "  vPos = float4(aPos, 0.0, 1.0);\n"
    "  vUv0 = aUv0;\n"
    "}\n";
constexpr std::string_view kCopyFragment =
    "float4 main(float2 vUv0 : TEXCOORD0, uniform sampler2D uTex0 : TEXUNIT0, uniform float4 uConst[5]) : COLOR {\n"
    "  float4 c = tex2D(uTex0, vUv0);\n"
    "  return float4(dot(uConst[0], c), dot(uConst[1], c), dot(uConst[2], c), dot(uConst[3], c)) + uConst[4];\n"
    "}\n";
constexpr uint64_t fnv1a(std::string_view text, uint64_t hash = 0xCBF29CE484222325ull) noexcept {
  for (const char c : text) {
    hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001B3ull;
  }
  return hash;
}
// Its name on the card (Program), from its sources as a generated program's is.
constexpr uint64_t kCopyProgramKey = fnv1a(kCopyFragment, fnv1a(kCopyVertex));
std::shared_ptr<Program> g_copyProgram;

// A copy read as a palette texture, converted (palette_on_gpu): the copy's texel (nearest) as an
// index, uConst[0].x times its 0-1 value plus .y, floored, and that palette entry (the palette one
// row, .z its width's reciprocal). Built as the copy program is.
constexpr std::string_view kPaletteFragment =
    "float4 main(float2 vUv0 : TEXCOORD0, uniform sampler2D uTex0 : TEXUNIT0, uniform sampler2D uTex1 : TEXUNIT1,\n"
    "            uniform float4 uConst[1]) : COLOR {\n"
    "  float index = floor(tex2D(uTex0, vUv0).r * uConst[0].x + uConst[0].y);\n"
    "  return tex2D(uTex1, float2((index + 0.5) * uConst[0].z, 0.5));\n"
    "}\n";
constexpr uint64_t kPaletteProgramKey = fnv1a(kPaletteFragment, fnv1a(kCopyVertex));
std::shared_ptr<Program> g_paletteProgram;

// The copy program's colour matrix for a copy (render worker): the output's r, g, b and a, each the
// dot product of a row with the screen's texel, then the fifth row added. GX's copies to intensity
// formats take the YUV luma, 16 for black to 235 for white. Where the EFB has no alpha (`alpha`
// false: the game's but for the Phazon Suit's and the thermal visor's frames), GX reads it as one,
// so a row's alpha weight goes into its offset.
void copy_rows(CopyKind kind, bool alpha, float rows[5][4]) noexcept {
  constexpr float kRed[4] = {1.f, 0.f, 0.f, 0.f};
  constexpr float kGreen[4] = {0.f, 1.f, 0.f, 0.f};
  constexpr float kBlue[4] = {0.f, 0.f, 1.f, 0.f};
  constexpr float kAlpha[4] = {0.f, 0.f, 0.f, 1.f};
  constexpr float kLuma[4] = {66.f / 256.f, 129.f / 256.f, 25.f / 256.f, 0.f};
  constexpr float kLumaOffset = 16.f / 255.f;
  const float* row[4];
  float offset[4] = {};
  switch (kind) {
  case CopyKind::Rgba:
    row[0] = kRed, row[1] = kGreen, row[2] = kBlue, row[3] = kAlpha;
    break;
  case CopyKind::Intensity:
    row[0] = row[1] = row[2] = row[3] = kLuma;
    offset[0] = offset[1] = offset[2] = offset[3] = kLumaOffset;
    break;
  case CopyKind::IntensityAlpha:
    row[0] = row[1] = row[2] = kLuma, row[3] = kAlpha;
    offset[0] = offset[1] = offset[2] = kLumaOffset;
    break;
  case CopyKind::Red:
    row[0] = row[1] = row[2] = row[3] = kRed;
    break;
  case CopyKind::RedAlpha:
    row[0] = row[1] = row[2] = kRed, row[3] = kAlpha;
    break;
  case CopyKind::Green:
    row[0] = row[1] = row[2] = row[3] = kGreen;
    break;
  case CopyKind::Blue:
    row[0] = row[1] = row[2] = row[3] = kBlue;
    break;
  case CopyKind::Alpha:
    row[0] = row[1] = row[2] = row[3] = kAlpha;
    break;
  case CopyKind::Rgb:
  default:
    // A format without alpha: opaque.
    row[0] = kRed, row[1] = kGreen, row[2] = kBlue, row[3] = nullptr;
    offset[3] = 1.f;
    break;
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      rows[r][c] = row[r] != nullptr ? row[r][c] : 0.f;
    }
    if (!alpha) {
      offset[r] += rows[r][3];
      rows[r][3] = 0.f;
    }
    rows[4][r] = offset[r];
  }
}

// A GL texture that is the display's back buffer as the frame's draws so far left it (render
// worker): the EFB copies' source, sampled linear for a halved copy. vitaGL binds a texture's GXM
// texture as it holds it (no sampler object is ever bound here), so its GXM texture is set to that
// buffer's own, the one GXM loads the buffer with, at each copy (the back buffer changes with each
// swap). Made once, never deleted (vitaGL would free the 1x1 level it was made with, not the display).
GLuint g_screenTexture = 0;

void bind_screen_texture(bool linear) noexcept {
  if (g_screenTexture == 0) {
    glGenTextures(1, &g_screenTexture);
    glBindTexture(GL_TEXTURE_2D, g_screenTexture);
    constexpr uint8_t kBlack[4] = {};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kBlack);
  } else {
    glBindTexture(GL_TEXTURE_2D, g_screenTexture);
  }
  SceGxmTexture screen = gxm_color_surfaces[gxm_back_buffer_index].backgroundTex;
  const SceGxmTextureFilter filter = linear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
  sceGxmTextureSetMinFilter(&screen, filter);
  sceGxmTextureSetMagFilter(&screen, filter);
  sceGxmTextureSetUAddrMode(&screen, SCE_GXM_TEXTURE_ADDR_CLAMP);
  sceGxmTextureSetVAddrMode(&screen, SCE_GXM_TEXTURE_ADDR_CLAMP);
  *vglGetGxmTexture(GL_TEXTURE_2D) = screen;
}

// The display's depth and stencil across a frame's scenes (render worker, between two scenes: GXM
// takes a surface's modes at a scene's start). Each EFB copy ends the display's scene: the copy is
// drawn into its texture's framebuffer, and the next draw starts the display's next scene. GXM
// starts a scene with the depth at its background value unless told to load it, and leaves it
// unstored unless told to store it, and vitaGL tells it neither (a STORE_DEPTH_STENCIL build would,
// for every scene): the draws after a copy (the particles behind a wall, the fog volumes) would
// test against nothing drawn before it. So a frame with copies stores the depth at the end of
// each scene a copy ends, and the scene after loads it. The mask bit, which vitaGL's scissor keeps
// in the depth surface, is loaded with it: this vitaGL clears it again at the scene's first scissor
// update (later ones skip that at a scene's start, assuming a fresh surface: kept, it would leak).
void keep_depth(bool load, bool store) noexcept {
  sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface, load ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
                                                                             : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
  sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface,
                                             store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                   : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
}

// An EFB copy, made on the GPU (render worker): the display's back buffer, as the draws so far left
// it, drawn through the copy program into the destination's texture (its framebuffer), with
// nothing waiting for the GPU. Drawing there ends the display's scene; the next draw on the display
// starts another (keep_depth: loading the depth this one stored, storing it again where another
// copy follows). The caller sets the display's viewport and scissor again, and binds every unit's
// texture again (unit 0 took the screen's, and the destination may now be another of its sizes).
// False when there was nothing to copy.
bool copy_on_gpu(const Draw& draw, Program& program, GLuint& usedProgram, uint32_t& attribsOn, RenderState& current,
                 bool moreCopies) noexcept {
  const ScreenRect& src = draw.scissor;
  const int step = draw.copyHalf && src.width > 1 && src.height > 1 ? 2 : 1;
  const int width = src.width / step;
  const int height = src.height / step;
  if (width <= 0 || height <= 0 || program.attr(kAttrPos) < 0 || program.attr(kAttrUv0) < 0) {
    return false;
  }
  use_program(program, usedProgram, attribsOn);
  apply_state(RenderState{.depthTest = false, .depthWrite = false}, current, false);
  glDisable(GL_SCISSOR_TEST);
  draw.copyTo->target(width, height);
  glViewport(0, 0, width, height);
  glActiveTexture(GL_TEXTURE0);
  bind_screen_texture(step == 2);
  float rows[5][4];
  copy_rows(draw.copyKind, draw.copyAlpha, rows);
  program.set_rows(rows, 5);
  // The rectangle's texels in the display's texture, whose rows run down from the screen's top
  // (GL's rectangle up from its bottom). vitaGL keeps a framebuffer's texture the right way up for
  // sampling: its first row is the draw's bottom (y -1), and takes the rectangle's top, as a GX copy
  // starts with it. Halved, each pixel samples its 2x2 block's middle, linear: GX's box filter.
  const float u0 = static_cast<float>(src.x) / kWidth;
  const float u1 = static_cast<float>(src.x + width * step) / kWidth;
  const float v0 = static_cast<float>(kHeight - (src.y + src.height)) / kHeight;
  const float v1 = v0 + static_cast<float>(height * step) / kHeight;
  const float quad[4][4] = {{-1.f, -1.f, u0, v0}, {1.f, -1.f, u1, v0}, {-1.f, 1.f, u0, v1}, {1.f, 1.f, u1, v1}};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(program.attr(kAttrPos), 2, GL_FLOAT, GL_FALSE, sizeof(quad[0]), &quad[0][0]);
  glVertexAttribPointer(program.attr(kAttrUv0), 2, GL_FLOAT, GL_FALSE, sizeof(quad[0]), &quad[0][2]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  keep_depth(true, moreCopies);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glEnable(GL_SCISSOR_TEST);
  g_presentStats.copyPixels += static_cast<uint64_t>(width) * height;
  return true;
}

// A copy read as a palette texture (render worker), converted on the GPU as Aurora's WebGPU path
// converts it (texture.cpp, resolve_dynamic_palette_texture): each texel of the copy (units[0]) as
// an index into its palette (units[1]), drawn through the palette program into `copyTo` at the
// copy's size; then sampled as any texture, filtered after the palette as GX filters. A
// framebuffer's draw as a copy's is: the display's scene ends, and the caller sets its state
// again. False when the copy was never made (nothing to convert).
bool palette_on_gpu(const Draw& draw, Program& program, GLuint& usedProgram, uint32_t& attribsOn,
                    RenderState& current, bool moreCopies) noexcept {
  Texture& source = *draw.units[0].texture;
  Texture& palette = *draw.units[1].texture;
  const auto width = static_cast<int>(source.width());
  const auto height = static_cast<int>(source.height());
  if (!source.copied() || width <= 0 || height <= 0 || palette.width() == 0 || program.attr(kAttrPos) < 0 ||
      program.attr(kAttrUv0) < 0) {
    return false;
  }
  use_program(program, usedProgram, attribsOn);
  apply_state(RenderState{.depthTest = false, .depthWrite = false}, current, false);
  glDisable(GL_SCISSOR_TEST);
  draw.copyTo->target(width, height);
  glViewport(0, 0, width, height);
  constexpr Sampler kNearest{GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST};
  glActiveTexture(GL_TEXTURE0);
  source.bind(kNearest);
  glActiveTexture(GL_TEXTURE1);
  palette.bind(kNearest);
  glActiveTexture(GL_TEXTURE0);
  // C4: the 8-bit value's top four bits (half a step up keeps an exact value off the edge below);
  // C8: the value, rounded.
  const bool four = draw.copyKind == CopyKind::Palette4;
  const float rows[1][4] = {{four ? 255.f / 16.f : 255.f, four ? 0.5f / 16.f : 0.5f,
                             1.f / static_cast<float>(palette.width()), 0.f}};
  program.set_rows(rows, 1);
  // The copy's first row to the framebuffer's (its texture's first, at the draw's bottom).
  constexpr float kQuad[4][4] = {{-1.f, -1.f, 0.f, 0.f}, {1.f, -1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f, 1.f}, {1.f, 1.f, 1.f, 1.f}};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(program.attr(kAttrPos), 2, GL_FLOAT, GL_FALSE, sizeof(kQuad[0]), &kQuad[0][0]);
  glVertexAttribPointer(program.attr(kAttrUv0), 2, GL_FLOAT, GL_FALSE, sizeof(kQuad[0]), &kQuad[0][2]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  keep_depth(true, moreCopies);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glEnable(GL_SCISSOR_TEST);
  g_presentStats.copyPixels += static_cast<uint64_t>(width) * height;
  return true;
}

// A draw with its generated program (render worker).
void draw_with_program(const Draw& draw, const Frame& frame, Program& program, GLuint& usedProgram,
                       uint32_t& attribsOn, RenderState& current, BoundTextures& bound) {
  use_program(program, usedProgram, attribsOn);
  g_clock.mark(kProgramSwitch);
  const auto base = reinterpret_cast<uintptr_t>(bind_draw_mesh(draw, frame, kPathProgram, kProgramUpload));
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
  draw_mesh(draw, frame);
  g_clock.mark(kProgramDraw);
}

void draw_world(const Frame& frame) {
  const std::vector<Draw>& draws = frame.draws;
  if (draws.empty()) {
    return;
  }
  ++g_presentStats.framesWithDraws;
  g_presentStats.draws += draws.size();
  g_presentStats.merged += frame.merged;
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
  // arrays it has on. A frame builds a few new ones at most; their draws go fixed-function till then.
  int buildBudget = kBuildsPerFrame;
  GLuint usedProgram = 0;
  uint32_t attribsOn = 0;
  // The copy and palette programs, built ahead of the copies that need them (a program just linked
  // is left in use).
  for (Program* copier : {g_copyProgram.get(), g_paletteProgram.get()}) {
    if (!copier->settled()) {
      const int budgetBefore = buildBudget;
      copier->ready(buildBudget);
      if (buildBudget != budgetBefore) {
        glUseProgram(0);
      }
    }
  }
  // The frame's EFB copies still to come (copy_on_gpu: each stores the depth for the one after).
  int copiesLeft = static_cast<int>(frame.copies);
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
  // The line and point width, set where a line or point draw changes it (GXM's context keeps it,
  // for the programs and the fixed-function path alike).
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
  for (const Draw& draw : draws) {
    if (draw.clear) {
      // An EFB copy's clear: its rectangle (the whole screen for the viewport, so the clear is
      // the scissor's alone), with the masks GX's updates give, then the draws' masks again.
      // Colour and alpha both or neither: vitaGL's glClear writes all four channels whatever
      // glColorMask says, so a clear of one of them comes as a draw (record_masked_clear).
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
      // An EFB copy, made on the GPU once the copy program is built (till then its destination
      // keeps what it held: black before its first copy). It ends the display's scene, and the
      // draws after start another: the viewport and scissor are set again, each unit's texture is
      // bound again at its next draw, and the matrices and the line width are loaded again.
      const uint64_t start = sceKernelGetProcessTimeWide();
      --copiesLeft;
      // Or a copy's conversion through its palette (palette_on_gpu), alike but for its program.
      const bool palette = draw.copyKind == CopyKind::Palette4 || draw.copyKind == CopyKind::Palette8;
      Program& copier = palette ? *g_paletteProgram : *g_copyProgram;
      const int budgetBefore = buildBudget;
      const bool ready = copier.ready(buildBudget);
      if (buildBudget != budgetBefore) {
        glUseProgram(usedProgram);
      }
      if (!ready) {
        ++g_presentStats.copiesWaiting;
      } else if (palette ? palette_on_gpu(draw, copier, usedProgram, attribsOn, current, copiesLeft > 0)
                         : copy_on_gpu(draw, copier, usedProgram, attribsOn, current, copiesLeft > 0)) {
        glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        glScissor(scissor.x, scissor.y, scissor.width, scissor.height);
        bound = {};
        haveProjection = false;
        for (bool& have : haveTexMatrix) {
          have = false;
        }
        width = 0;
        ++g_presentStats.copies;
      }
      g_presentStats.copyUs += sceKernelGetProcessTimeWide() - start;
      g_clock.mark(kCopy);
      continue;
    }
    set_rects(draw.viewport, draw.scissor);
    const MeshPrimitive primitive = draw.mesh != nullptr ? draw.mesh->primitive() : draw.frameMesh.primitive;
    if (primitive != MeshPrimitive::Triangles && draw.width != width) {
      glLineWidth(static_cast<GLfloat>(draw.width));
      glPointSize(static_cast<GLfloat>(draw.width));
      width = draw.width;
    }
    Program* program = draw.program.program;
    if (program != nullptr) {
      const int budgetBefore = buildBudget;
      const bool ready = program->ready(buildBudget);
      if (buildBudget != budgetBefore) {
        // A program built just now was made current to set its samplers: back to the one in use.
        glUseProgram(usedProgram);
      }
      if (!ready) {
        program = nullptr;
      }
      g_clock.mark(kBuild);
    }
    if (program != nullptr) {
      ++g_presentStats.programDraws;
      g_clock.programDraws += g_clock.on ? 1 : 0;
      draw_with_program(draw, frame, *program, usedProgram, attribsOn, current, bound);
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
    const auto base = reinterpret_cast<uintptr_t>(bind_draw_mesh(draw, frame, kPathFixed, kFixedUpload));
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
      const DrawUnit& texUnit = draw.units[unit];
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
    draw_mesh(draw, frame);
    g_clock.mark(kFixedDraw);
  }
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
  glViewport(0, 0, kWidth, kHeight);
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

} // namespace

bool initialize() noexcept {
  // vitaGL's cache for what glCompileShader compiles, in the game's folder. Nothing here compiles
  // through it: the generated programs compile on the compiler thread, and vitaGL keeps its
  // fixed-function shaders in a folder of its own (check_fixed_function_cache).
  static const std::string cachePath = std::string{g_config.userPath} + "/vitagl_shader_cache";
  std::error_code ec;
  std::filesystem::create_directories(cachePath, ec);
  vglSetShaderCachePath(cachePath.c_str());
  check_fixed_function_cache();
  // The generated programs' own cache (Program, read_cached), and what it holds: nothing another
  // build wrote (program_binaries_build).
  g_programCacheDir = std::string{g_config.userPath} + "/vita_programs";
  std::filesystem::create_directories(g_programCacheDir, ec);
  const std::string build = this_build();
  const std::string binariesBuild = program_binaries_build();
  if (build.empty() || binariesBuild == build) {
    index_card();
  } else if (!card_thread().submit([binariesBuild, build] { delete_program_binaries(binariesBuild, build); })) {
    Log.warn("no card thread: the program binaries another build left stay on the card, unread");
  }
  // The warm-up's material records (gx/vita_tev.cpp), read here before the FIFO thread asks, and the
  // programs compiled on the PC (tools/vita_precompile).
  g_materialsPath = g_programCacheDir + "/materials.bin";
  read_materials();
  g_packPath = g_programCacheDir + "/precompiled.bin";
  read_pack_index();
  Log.info("programs on the card: {}; precompiled: {}; material records: {} KiB", g_cardKeys.size(), g_pack.size(),
           g_savedMaterials.size() / 1024);
  // GXM's ring buffers hold a scene's draws until the GPU draws the scene (the fragment ring each
  // draw's textures and fragment state, the VDM ring its commands). The defaults (512, 128 and 16
  // KiB) were made for scenes of a few hundred plain draws; the water's generated programs take four
  // to seven textures each, and 1100 to 4300 draws in one scene ended a run in Tallon Overworld in a
  // GPU hang before (VITA_PHASE2.md). Four times each; the vertex ring (2 MiB) twice.
  vglSetFragmentBufferSize(4 * SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE);
  vglSetVDMBufferSize(4 * SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE);
  vglSetVertexBufferSize(2 * SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE);
  vglSetUSSEBufferSize(4 * SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE);
  // The scenes a frame draws to each render target: each EFB copy ends the display's scene and
  // draws one into its texture's framebuffer (copy_on_gpu), and GXM sizes a target's own memory
  // by the scenes it expects a frame (one by default, past which a scene waits for the GPU to be
  // done with one before it). The display takes the copies' (record_copy makes 7 a frame at most)
  // and one; a copy's framebuffer, a destination copied a few times a frame (the particles').
  vglSetupRenderTargetScenesNum(8, 4);
  // The shader patcher's heaps, larger (kPatcherFragmentUsse's comment).
  vglSetupShaderPatcher(kPatcherBuffer, kPatcherVertexUsse, kPatcherFragmentUsse);
  // Not a success flag: this vitaGL returns true when the requested size was beyond the display's
  // and it started at the display's own size instead, false when it started as asked. Either way
  // GXM is now vitaGL's, so nothing else (SDL's renderer) may initialise it after this.
  const bool clamped = vglInitExtended(kLegacyPool, kWidth, kHeight, kRamThreshold, SCE_GXM_MULTISAMPLE_NONE);
  use_compiler_heap();
  g_copyProgram = make_program(std::string(kCopyVertex), std::string(kCopyFragment), kCopyProgramKey);
  g_paletteProgram = make_program(std::string(kCopyVertex), std::string(kPaletteFragment), kPaletteProgramKey);
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

namespace {
// The system keyboard (open_keyboard): asked for on the game thread; opened, drawn and closed on
// the render worker, the thread that swaps. A common dialog draws at the swap
// (vglSwapBuffers(GL_TRUE)), and SceImeDialog's calls all stay on that one thread.
struct Keyboard {
  enum class State : uint8_t { Closed, Asked, Open, Confirmed, Cancelled };
  State state = State::Closed;
  uint32_t maxLength = 0;
  SceWChar16 initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = {};
  SceWChar16 text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = {};
};
std::mutex g_keyboardMutex;
Keyboard g_keyboard;

// Render worker, before each swap: opens the keyboard asked for, and takes a finished one's
// outcome. True while one is up.
bool update_keyboard() noexcept {
  std::lock_guard lock{g_keyboardMutex};
  Keyboard& k = g_keyboard;
  if (k.state == Keyboard::State::Asked) {
    static const SceWChar16 kTitle[] = {0};
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.type = SCE_IME_TYPE_DEFAULT;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = kTitle;
    param.maxTextLength = k.maxLength;
    param.initialText = k.initial;
    param.inputTextBuffer = k.text;
    if (const int result = sceImeDialogInit(&param); result < 0) {
      Log.warn("the system keyboard did not open (error {:#010x})", static_cast<uint32_t>(result));
      k.state = Keyboard::State::Cancelled;
      return false;
    }
    k.state = Keyboard::State::Open;
  }
  if (k.state != Keyboard::State::Open) {
    return false;
  }
  if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
    return true;
  }
  SceImeDialogResult result{};
  sceImeDialogGetResult(&result);
  sceImeDialogTerm();
  k.state = result.button == SCE_IME_DIALOG_BUTTON_ENTER ? Keyboard::State::Confirmed : Keyboard::State::Cancelled;
  return false;
}
} // namespace

bool open_keyboard(const char* text, uint32_t maxLength) noexcept {
  std::lock_guard lock{g_keyboardMutex};
  Keyboard& k = g_keyboard;
  if (k.state != Keyboard::State::Closed) {
    return false;
  }
  k.maxLength = std::clamp<uint32_t>(maxLength, 1, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
  k.initial[0] = 0;
  k.text[0] = 0;
  if (char* wide = SDL_iconv_string("UTF-16LE", "UTF-8", text, SDL_strlen(text) + 1)) {
    const auto* units = reinterpret_cast<const SceWChar16*>(wide);
    uint32_t n = 0;
    for (; n < k.maxLength && units[n] != 0; ++n) {
      k.initial[n] = units[n];
    }
    k.initial[n] = 0;
    SDL_free(wide);
  }
  k.state = Keyboard::State::Asked;
  return true;
}

int keyboard_result(std::string& text) noexcept {
  std::lock_guard lock{g_keyboardMutex};
  Keyboard& k = g_keyboard;
  switch (k.state) {
  case Keyboard::State::Closed:
    return -2;
  case Keyboard::State::Asked:
  case Keyboard::State::Open:
    return 0;
  case Keyboard::State::Confirmed: {
    size_t units = 0;
    while (units < SCE_IME_DIALOG_MAX_TEXT_LENGTH && k.text[units] != 0) {
      ++units;
    }
    text.clear();
    if (char* utf8 = SDL_iconv_string("UTF-8", "UTF-16LE", reinterpret_cast<const char*>(k.text),
                                      units * sizeof(SceWChar16))) {
      text = utf8;
      SDL_free(utf8);
    }
    k.state = Keyboard::State::Closed;
    return 1;
  }
  case Keyboard::State::Cancelled:
    k.state = Keyboard::State::Closed;
    return -1;
  }
  return -2;
}

bool keyboard_open() noexcept {
  std::lock_guard lock{g_keyboardMutex};
  return g_keyboard.state == Keyboard::State::Asked || g_keyboard.state == Keyboard::State::Open;
}

void program_compiles(uint32_t& sent, uint32_t& finished) noexcept {
  // Finished first: read the other way, a compile sent and finished in between could count as
  // finished and not sent. The warm-up's programs waiting their turn count as sent, and those found
  // built by their turn as sent and finished, so the toast counts the warm-up's batch whole from its
  // start rather than one program at a time.
  const uint32_t foundBuilt = g_warmUpFoundBuilt.load(std::memory_order_acquire);
  finished = g_compilesFinished.load(std::memory_order_acquire) + foundBuilt;
  sent = g_compilesSent.load(std::memory_order_relaxed) + g_warmUpAhead.load(std::memory_order_relaxed) + foundBuilt;
}

void present(ImDrawData* overlay) noexcept {
  if (g_clock.callUs < 0.0) {
    g_clock.callUs = timer_call_us();
  }
  g_clock.on = ++g_clock.presents % kTimedEvery == 0;
  g_clock.start();
  // The font texture is made here, on the render worker, from the atlas the game thread built;
  // the atlas only changes when the fonts do (fonts_changed).
  if (g_fontsChanged.exchange(false, std::memory_order_acq_rel)) {
    ImGui_ImplOpenGL2_DestroyFontsTexture();
  }
  ImGui_ImplOpenGL2_NewFrame();
  glViewport(0, 0, kWidth, kHeight);
  glDisable(GL_SCISSOR_TEST);
  // Black, as the GameCube's frame starts (a dark blue told this path's screen apart at first), and
  // alpha 0, the game's clear colour's (CCubeRenderer::BeginScene), which an EFB with alpha starts
  // from (the display shows no alpha, and an EFB without it reads one: dst_alpha_mode).
  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClearDepth(1.0);
  // The clear follows the masks; a draw's state may have left them off.
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  Frame frame = take_frame();
  // The clear starts the frame's first scene: it loads no depth, and stores it for a copy to come.
  keep_depth(false, frame.copies != 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  draw_world(frame);
  warm_up_step();
  g_clock.mark(kBuild);
  g_reaper.reap(std::move(frame));
  g_clock.mark(kFinish);
  if (++g_presentStats.presents == 300) {
    Log.info("presents: {}, {} with game draws, {:.1f} draws a frame ({:.1f} more joined to the draw before them), {} "
             "destroyed here with the reaper two behind; "
             "generated programs: {} draws with theirs, {} "
             "fixed-function waiting for theirs; {} built ({} failed, {} sent to the compiler thread, {:.0f} ms "
             "compiling there, at most {} KiB of its heap in use, {} allocations past it; {} from the precompiled "
             "pack; {} loaded from the card, {} of them compiled again, {:.0f} ms) in all, the warm-up's {} ({} "
             "failed, {} built or being built by their turn); uniforms sent/kept: vertex {}/{}, fragment {}/{}; "
             "textures bound/kept: {}/{}; EFB copies made on the GPU: {} ({:.0f} kpixels written, {:.1f} ms on "
             "the render worker, {:.2f} ms each), {} before the copy program was built",
             g_presentStats.presents, g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.merged) / g_presentStats.framesWithDraws : 0.0,
             g_reaper.take_destroyed_here(),
             g_presentStats.programDraws, g_presentStats.programPending, g_programStats.compiled,
             g_programStats.failed, g_programStats.queued, g_programStats.compileMs,
             g_compilerPeak.load(std::memory_order_relaxed) / 1024, g_compilerOverflows.load(std::memory_order_relaxed),
             g_programStats.precompiled, g_programStats.loaded, g_programStats.reloaded, g_programStats.loadMs,
             g_warmUpStats.built,
             g_warmUpStats.failed, g_warmUpStats.skipped,
             g_uniformStats.vertex.uploaded, g_uniformStats.vertex.skipped, g_uniformStats.fragment.uploaded,
             g_uniformStats.fragment.skipped, g_uniformStats.textureBinds, g_uniformStats.texturesKept,
             g_presentStats.copies, g_presentStats.copyPixels / 1000.0, g_presentStats.copyUs / 1000.0,
             g_presentStats.copies != 0 ? g_presentStats.copyUs / 1000.0 / g_presentStats.copies : 0.0,
             g_presentStats.copiesWaiting);
    g_presentStats = {};
    g_uniformStats = {};
    log_memory("presents");
    thread::log_cpu();
  }
  g_clock.mark(kOther);
  if (overlay != nullptr && overlay->CmdListsCount > 0) {
    ImGui_ImplOpenGL2_RenderDrawData(overlay);
  }
  g_clock.mark(kOverlay);
  // A system keyboard that is up (open_keyboard) draws over the frame at the swap.
  vglSwapBuffers(update_keyboard() ? GL_TRUE : GL_FALSE);
  g_clock.mark(kSwap);
  if (g_clock.on) {
    ++g_clock.frames;
  }
  g_clock.on = false;
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
    "idle",   "state", "copies",      "dispatch", "material", "lookup",   "skinned", "skinned copy", "immediate",
    "mesh",   "record", "rows",       "other draws", "aurora", "vita",    "textures", "finish",      "frame"};
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

// vitaGL's splash screen: its logo, which vglInit has a thread of its own draw on a GXM context of
// its own until the first frame's scene. Not started (the linker wraps its start, CMakeLists): its
// flag stays off, so that scene and the swaps take their usual paths, as in vitaGL's system app
// mode, which skips it too.
extern "C" void __wrap_invoke_splashscreen() {}

// vitashark keeps a compile's input, options and output in statics of its own, so two compiles
// must not overlap: the compiler thread's and vitaGL's (a fixed-function shader its cache lacks, on
// the render worker). The linker wraps both entry points (CMakeLists): a compile takes the lock,
// and clearing its output, which follows every compile on its thread (vitaGL's and the compiler
// thread's alike), gives it back.
//
// vitaGL compiles inside a draw call (reload_ffp_shaders, whose own frame is some 17 KiB), and on
// the render worker's 32 KiB SceShaccCg ran past the stack's end: twice at the title screen in a
// run of tallon5 (873832b). A compile called with less than half the compiler thread's stack left
// runs on a thread with that stack, its caller waiting.
namespace {
std::mutex& shark_lock() noexcept {
  static std::mutex& lock = *new std::mutex;
  return lock;
}

// The calling thread's stack left below here (0 when the kernel does not say).
size_t stack_left() noexcept {
  SceKernelThreadInfo info{};
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) {
    return 0;
  }
  return reinterpret_cast<uintptr_t>(&info) - reinterpret_cast<uintptr_t>(info.stack);
}
} // namespace

extern "C" {
SceGxmProgram* __real_shark_compile_shader_extended(const char* src, uint32_t* size, shark_type type, shark_opt opt,
                                                    int32_t useFastmath, int32_t useFastprecision, int32_t useFastint);
void __real_shark_clear_output();

SceGxmProgram* __wrap_shark_compile_shader_extended(const char* src, uint32_t* size, shark_type type, shark_opt opt,
                                                    int32_t useFastmath, int32_t useFastprecision, int32_t useFastint) {
  using aurora::vita_gl::kCompileStack;
  shark_lock().lock();
  if (stack_left() >= kCompileStack / 2) {
    return __real_shark_compile_shader_extended(src, size, type, opt, useFastmath, useFastprecision, useFastint);
  }
  struct Call {
    const char* src;
    uint32_t* size;
    shark_type type;
    shark_opt opt;
    int32_t useFastmath;
    int32_t useFastprecision;
    int32_t useFastint;
    SceGxmProgram* program;
  } call{src, size, type, opt, useFastmath, useFastprecision, useFastint, nullptr};
  const auto run = [](void* arg) -> void* {
    Call& c = *static_cast<Call*>(arg);
    c.program = __real_shark_compile_shader_extended(c.src, c.size, c.type, c.opt, c.useFastmath, c.useFastprecision,
                                                     c.useFastint);
    return nullptr;
  };
  const uint64_t start = sceKernelGetProcessTimeWide();
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kCompileStack);
  pthread_t thread;
  const bool own = pthread_create(&thread, &attr, run, &call) == 0;
  if (own) {
    pthread_join(thread, nullptr);
  } else {
    run(&call); // no thread for it: on the caller's stack, as before
  }
  pthread_attr_destroy(&attr);
  aurora::vita_gl::Log.info("vitaGL compiled a {} shader of its own in {} ms{}{}",
                            type == SHARK_VERTEX_SHADER ? "vertex" : "fragment",
                            (sceKernelGetProcessTimeWide() - start) / 1000,
                            own ? "" : " on its caller's stack (no thread for it)",
                            call.program == nullptr ? ": it failed" : "");
  return call.program;
}

void __wrap_shark_clear_output() {
  __real_shark_clear_output();
  shark_lock().unlock();
}
}
