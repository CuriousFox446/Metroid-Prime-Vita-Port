#include "vita_gl.hpp"
#include "vita_fifo_clock.hpp"
#include "vita_side_jobs.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"
#include "thread.hpp"

#include <vitaGL.h>
#include <vitashark.h>
#include <dolphin/os.h>
#include <SDL3/SDL_stdinc.h>
#include <psp2/common_dialog.h>
#include <psp2/display.h>
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
// And its shader patcher, whose heaps the log reports (log_memory). The fence each scene ends with
// (its scene_end: `value` the number of the last scene sent, `*address` the last one the GPU has
// finished, written as its fragments are done), which times the GPU (GpuClock); and the
// framebuffer object bound for drawing (its `framebuffer *`), whose depth surface a frame drawn at
// another scale keeps across its copies (WorldTarget).
extern "C" {
extern SceGxmColorSurface gxm_color_surfaces[];
extern unsigned int gxm_back_buffer_index;
extern SceGxmDepthStencilSurface gxm_depth_stencil_surface;
extern SceGxmShaderPatcher* gxm_shader_patcher;
extern SceGxmNotification query_fence;
extern void* active_write_fb;
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
#include <shared_mutex>
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
std::atomic<int64_t> g_textureCount[kTexelFormats];
std::atomic<int64_t> g_textureBytes[kTexelFormats];
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
           "textures on the GPU: {} RGBA8 ({:.1f} MiB), {} BC1 ({:.1f} MiB), {} I8 ({:.1f} MiB), {} IA8 ({:.1f} "
           "MiB); not yet uploaded: {} ({:.1f} MiB){}",
           when, pool(VGL_MEM_VRAM), pool(VGL_MEM_RAM), pool(VGL_MEM_PHYCONT), pool(VGL_MEM_BUDGET),
           g_textureCount[0].load(), g_textureBytes[0].load() / kMiB, g_textureCount[1].load(),
           g_textureBytes[1].load() / kMiB, g_textureCount[2].load(), g_textureBytes[2].load() / kMiB,
           g_textureCount[3].load(), g_textureBytes[3].load() / kMiB, g_pendingCount.load(),
           g_pendingBytes.load() / kMiB, patcher);
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
  // How many more grow takes without moving what it holds.
  uint32_t room() const { return m_capacity - m_size; }

private:
  std::unique_ptr<T[]> m_data;
  uint32_t m_size = 0;
  uint32_t m_capacity = 0;
};

// 0.46.0 test 9. A frame's vertices ran to some 50,000 in Phendrana (the snow alone ~28,000): ~2.5
// MiB in one piece, doubled to ~5 MiB when a frame outgrew the spares. With the heap near its
// ceiling (191 of 229 MiB in use, the 33 MiB free in pieces) test 8 died there: std::bad_alloc for
// 4,956,896 bytes (88,516 vertices) on the FIFO thread. They come in chunks of one size now, kept
// for the frames after: no piece larger than a chunk, and the same pieces again and again.
template <typename T>
class ChunkPool {
public:
  static constexpr uint32_t kChunk = 8192; // elements a chunk (448 KiB of vertices)
  static ChunkPool& get() noexcept {
    static ChunkPool& pool = *new ChunkPool;
    return pool;
  }
  std::unique_ptr<T[]> take() {
    {
      std::lock_guard lock{m_mutex};
      if (!m_free.empty()) {
        std::unique_ptr<T[]> chunk = std::move(m_free.back());
        m_free.pop_back();
        return chunk;
      }
    }
    return std::unique_ptr<T[]>(new T[kChunk]);
  }
  void put(std::unique_ptr<T[]>&& chunk) noexcept {
    if (!chunk) {
      return;
    }
    std::unique_ptr<T[]> dropped; // freed after the lock
    std::lock_guard lock{m_mutex};
    if (m_free.size() < kKept) {
      m_free.push_back(std::move(chunk));
    } else {
      dropped = std::move(chunk);
    }
  }

private:
  static constexpr size_t kKept = 16;
  std::mutex m_mutex;
  std::vector<std::unique_ptr<T[]>> m_free;
};

// A frame's own vertices (FrameMesh), in chunks (ChunkPool), written by the FIFO thread as it
// records. A run that does not fit in the last chunk starts the next (one larger than a chunk gets
// one of its own size); what was handed out never moves, so the core 0 helper's writes need no wait
// when the frame grows (room). A place is the chunk's number above kOffsetBits and the place in it
// below: a mesh's vertices are together in one chunk, and two meshes are next to each other (place
// plus count, merge_into_last) only in the same chunk.
template <typename T>
class FrameChunks {
public:
  static constexpr uint32_t kOffsetBits = 20;
  static constexpr uint32_t kOffsetMask = (1u << kOffsetBits) - 1;

  FrameChunks() = default;
  FrameChunks(FrameChunks&& other) noexcept
  : m_chunks(std::move(other.m_chunks)), m_used(std::exchange(other.m_used, 0)),
    m_size(std::exchange(other.m_size, 0)) {}
  FrameChunks& operator=(FrameChunks&& other) noexcept {
    if (this != &other) {
      release();
      m_chunks = std::move(other.m_chunks);
      m_used = std::exchange(other.m_used, 0);
      m_size = std::exchange(other.m_size, 0);
    }
    return *this;
  }
  ~FrameChunks() { release(); }

  // Room for `count` more, `at` set to their place; the pointer holds for the frame.
  T* grow(uint32_t count, uint32_t& at) {
    if (m_chunks.empty() || m_used + count > m_chunks.back().capacity) {
      add_chunk(count);
    }
    Chunk& chunk = m_chunks.back();
    at = (static_cast<uint32_t>(m_chunks.size() - 1) << kOffsetBits) | m_used;
    T* const out = chunk.data.get() + m_used;
    m_used += count;
    m_size += count;
    return out;
  }
  // Back to `at`, the place the last grow gave (in the last chunk).
  void trim(uint32_t at) {
    const uint32_t chunk = at >> kOffsetBits;
    const uint32_t offset = at & kOffsetMask;
    if (!m_chunks.empty() && chunk == m_chunks.size() - 1 && offset < m_used) {
      m_size -= m_used - offset;
      m_used = offset;
    }
  }
  const T* at(uint32_t place) const { return m_chunks[place >> kOffsetBits].data.get() + (place & kOffsetMask); }
  T* at(uint32_t place) { return m_chunks[place >> kOffsetBits].data.get() + (place & kOffsetMask); }
  uint32_t size() const { return m_size; }
  // Growing never moves what it holds.
  uint32_t room() const { return UINT32_MAX; }

private:
  struct Chunk {
    std::unique_ptr<T[]> data;
    uint32_t capacity = 0;
  };
  void add_chunk(uint32_t count) {
    Chunk chunk;
    if (count <= ChunkPool<T>::kChunk) {
      chunk.data = ChunkPool<T>::get().take();
      chunk.capacity = ChunkPool<T>::kChunk;
    } else {
      chunk.data.reset(new T[count]);
      chunk.capacity = count;
    }
    m_chunks.push_back(std::move(chunk));
    m_used = 0;
  }
  void release() noexcept {
    for (Chunk& chunk : m_chunks) {
      if (chunk.capacity == ChunkPool<T>::kChunk) {
        ChunkPool<T>::get().put(std::move(chunk.data));
      }
    }
    m_chunks.clear();
    m_used = 0;
    m_size = 0;
  }
  std::vector<Chunk> m_chunks;
  uint32_t m_used = 0; // in the last chunk
  uint32_t m_size = 0;
};

// A vector whose storage, when its frame goes, is kept for the frames after (0.46.0 test 9): a
// frame's draws are ~1 KiB each, a heavy frame's a megabyte, and were allocated anew each frame
// and freed a frame or two later. The two largest are kept; a frame takes the smallest that fits.
template <typename T>
class VectorPool {
public:
  static VectorPool& get() noexcept {
    static VectorPool& pool = *new VectorPool;
    return pool;
  }
  void put(std::vector<T>&& v) noexcept {
    if (v.capacity() == 0) {
      return;
    }
    v.clear();
    std::vector<T> dropped; // freed after the lock
    std::lock_guard lock{m_mutex};
    m_spares.push_back(std::move(v));
    if (m_spares.size() > kKept) {
      const auto smallest = std::min_element(m_spares.begin(), m_spares.end(), [](const auto& a, const auto& b) {
        return a.capacity() < b.capacity();
      });
      dropped = std::move(*smallest);
      m_spares.erase(smallest);
    }
  }
  // An empty vector with room for `capacity`: the smallest spare that has it, else a new one.
  std::vector<T> take(size_t capacity) {
    {
      std::lock_guard lock{m_mutex};
      auto best = m_spares.end();
      for (auto it = m_spares.begin(); it != m_spares.end(); ++it) {
        if (it->capacity() >= capacity && (best == m_spares.end() || it->capacity() < best->capacity())) {
          best = it;
        }
      }
      if (best != m_spares.end()) {
        std::vector<T> v = std::move(*best);
        m_spares.erase(best);
        return v;
      }
    }
    std::vector<T> v;
    v.reserve(capacity);
    return v;
  }

private:
  static constexpr size_t kKept = 2;
  std::mutex m_mutex;
  std::vector<std::vector<T>> m_spares;
};

template <typename T>
struct RecycledVector {
  std::vector<T> v;
  RecycledVector() = default;
  RecycledVector(RecycledVector&& other) noexcept : v(std::move(other.v)) {}
  RecycledVector& operator=(RecycledVector&& other) noexcept {
    if (this != &other) {
      VectorPool<T>::get().put(std::move(v));
      v = std::move(other.v);
    }
    return *this;
  }
  ~RecycledVector() { VectorPool<T>::get().put(std::move(v)); }
};

// A frame's draws, their programs' constant rows (ProgramInputs) and the vertices and indices of
// its own meshes (FrameMesh), and its serial, a new one each frame (from 1), by which the render
// worker tells one frame's rows from another's; and its EFB copies among the draws, counted.
struct Frame {
  RecycledVector<Draw> draws;
  std::vector<std::array<float, 4>> constants;
  FrameChunks<Vertex> vertices;
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

// Copy surfaces (a copy texture's texture at one size and the framebuffer drawing into it) no copy
// texture holds any more: kept kSurfaceKeptFrames presents before they are deleted, and taken again
// meanwhile by a copy at their size. vitaGL frees a texture's memory at once, not a few frames
// later, when no draw has sampled it for a few frames or ever since it was made (its last_frame
// OBJ_NOT_USED), and a draw into its framebuffer does not count: a surface made again at another
// size (glTexImage2D) or deleted soon after a copy into it, before any draw sampled it, let that
// copy's scene, still to run on the GPU, write into a freed block of vitaGL's heap, whose
// bookkeeping lives in its free blocks (0.46.0 test 11: a data abort in SceLibKernel's heap code on
// the render worker, in glBufferData, after the Thardus fight with the copies heavy). The GPU runs
// a frame or two behind, and vitaGL's own deferred frees wait 4 (FRAME_PURGE_FREQ).
struct KeptSurface {
  GLuint texture = 0;
  GLuint framebuffer = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t frame = 0; // g_surfaceFrame when it was let go
};
constexpr uint32_t kSurfaceKeptFrames = 8;
std::vector<KeptSurface> g_keptSurfaces; // render worker
uint32_t g_surfaceFrame = 0;             // render worker: presents
uint64_t g_keptSurfaceBytes = 0;
struct SurfaceStats {
  uint32_t made = 0;
  uint32_t reused = 0;
  uint32_t letGo = 0;
  uint32_t deleted = 0;
};
SurfaceStats g_surfaceStats;

// Render worker: a surface no copy texture holds now.
void keep_surface(GLuint texture, GLuint framebuffer, uint16_t width, uint16_t height) noexcept {
  g_keptSurfaces.push_back({texture, framebuffer, width, height, g_surfaceFrame});
  g_keptSurfaceBytes += uint64_t{width} * height * 4;
  ++g_surfaceStats.letGo;
}

// Render worker: a kept surface at `width` x `height`, taken (its framebuffer bound), or false. A
// copy into it runs after the scenes that used it before, as a surface copied into frame after frame
// does.
bool take_kept_surface(int width, int height, GLuint& texture, GLuint& framebuffer) noexcept {
  for (auto it = g_keptSurfaces.begin(); it != g_keptSurfaces.end(); ++it) {
    if (it->width == width && it->height == height) {
      texture = it->texture;
      framebuffer = it->framebuffer;
      g_keptSurfaceBytes -= uint64_t{it->width} * it->height * 4;
      g_keptSurfaces.erase(it);
      ++g_surfaceStats.reused;
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      return true;
    }
  }
  return false;
}

// Render worker, after each present: deletes the surfaces kept kSurfaceKeptFrames presents, which
// the GPU is done with (vitaGL may free them at once).
void delete_kept_surfaces() noexcept {
  ++g_surfaceFrame;
  if (g_keptSurfaces.empty() || g_surfaceFrame - g_keptSurfaces.front().frame < kSurfaceKeptFrames) {
    return;
  }
  std::vector<GLuint> textures;
  std::vector<GLuint> framebuffers;
  size_t done = 0;
  for (; done < g_keptSurfaces.size(); ++done) {
    const KeptSurface& s = g_keptSurfaces[done];
    if (g_surfaceFrame - s.frame < kSurfaceKeptFrames) {
      break;
    }
    textures.push_back(s.texture);
    framebuffers.push_back(s.framebuffer);
    g_keptSurfaceBytes -= uint64_t{s.width} * s.height * 4;
  }
  g_keptSurfaces.erase(g_keptSurfaces.begin(), g_keptSurfaces.begin() + static_cast<ptrdiff_t>(done));
  // The framebuffers first: a texture still attached to one is only marked for later.
  glDeleteFramebuffers(static_cast<GLsizei>(framebuffers.size()), framebuffers.data());
  glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
  g_surfaceStats.deleted += static_cast<uint32_t>(done);
}

class Texture : public Kept {
public:
  Texture(uint32_t width, uint32_t height, TexelFormat format, std::vector<uint8_t> texels, uint32_t levels) noexcept
  : m_cpu(std::move(texels)), m_width(width), m_height(height), m_format(format),
    m_levels(std::max(levels, 1u)) {
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
      // A copy texture: its texture and framebuffer for each size it was copied at, kept a few
      // frames for the copies' scenes still to run (keep_surface), and the stand-in it was bound as
      // before its first copy, if that is still about (only ever sampled: deleted now).
      const GLuint standIn = m_id != 0 && !is_surface(m_id) ? m_id : 0;
      std::array<CopySurface, kCopySurfaces> surfaces = *m_surfaces;
      int64_t count = standIn != 0 ? 1 : 0;
      for (const CopySurface& surface : surfaces) {
        count += surface.texture != 0 ? 1 : 0;
      }
      g_textureCount[kind].fetch_sub(count, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_sub(m_bytes, std::memory_order_relaxed);
      gfx::render_worker::enqueue_work([standIn, surfaces] {
        if (standIn != 0) {
          glDeleteTextures(1, &standIn);
        }
        for (const CopySurface& surface : surfaces) {
          if (surface.texture != 0) {
            keep_surface(surface.texture, surface.framebuffer, surface.width, surface.height);
          }
        }
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
    uint64_t total = 0;
    for (uint32_t level = 0; level < m_levels; ++level) {
      total += m_format == TexelFormat::BC1 ? bc1_level_bytes(level) : plain_level_bytes(level);
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
        // 0.46.0 test 14: its levels too (gfx/texture.cpp, make_vita_texture), each after the
        // transfers before it as a BC1 chain's. Test 15: an intensity texture in a byte a texel
        // (GL_INTENSITY, sampled (I, I, I, I)), one with alpha in two (GL_LUMINANCE_ALPHA, (I, I, I, A)).
        GLint internalFormat = GL_RGBA;
        GLenum dataFormat = GL_RGBA;
        if (m_format == TexelFormat::I8) {
          // 0.46.0 test 22: given as GL_RED, which vitaGL copies in as it is (its fast_store), and
          // turned to (I, I, I, I) below, once every level is in (U8_R and U8_RRRR are one layout,
          // two swizzles). As GL_INTENSITY vitaGL put each texel through its read and write
          // callbacks: a movie frame's three planes (640x480 and two 320x240) took the render worker
          // 20 to 40 ms, and the title and file select menus ran at 24 FPS in test 21.
          internalFormat = GL_RED;
          dataFormat = GL_RED;
        } else if (m_format == TexelFormat::IA8) {
          internalFormat = GL_LUMINANCE_ALPHA;
          dataFormat = GL_LUMINANCE_ALPHA;
        }
        size_t offset = 0;
        uint32_t uploaded = 0;
        for (uint32_t level = 0; level < m_levels; ++level) {
          const auto size = static_cast<size_t>(plain_level_bytes(level));
          if (offset + size > m_cpu.size()) {
            break;
          }
          if (level != 0) {
            sceGxmTransferFinish();
          }
          glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), internalFormat, std::max(width >> level, 1),
                       std::max(height >> level, 1), 0, dataFormat, GL_UNSIGNED_BYTE, m_cpu.data() + offset);
          offset += size;
          ++uploaded;
        }
        if (m_format == TexelFormat::I8) {
          // After the last level: vitaGL sets the texture up again (its stored U8_R) as it adds each.
          if (SceGxmTexture* gxm = vglGetGxmTexture(GL_TEXTURE_2D)) {
            sceGxmTextureSetFormat(gxm, SCE_GXM_TEXTURE_FORMAT_U8_RRRR);
          }
        }
        m_levels = std::max(uploaded, 1u);
        static int sChainsLogged[kTexelFormats] = {};
        static constexpr const char* kNames[kTexelFormats] = {"RGBA8", "BC1", "I8", "IA8"};
        const int kind = static_cast<int>(m_format);
        if ((uploaded > 1 || m_format != TexelFormat::RGBA8) && sChainsLogged[kind] < 3) {
          ++sChainsLogged[kind];
          Log.info("{} texture {}x{} uploaded with {} levels ({} bytes)", kNames[kind], m_width, m_height, uploaded,
                   offset);
        }
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
      if (surface->texture != 0) {
        // Never made again in place (keep_surface says why): let go for a few frames, and another
        // of the new size taken or made.
        const int64_t old = static_cast<int64_t>(surface->width) * surface->height * 4;
        g_textureCount[kind].fetch_sub(1, std::memory_order_relaxed);
        g_textureBytes[kind].fetch_sub(old, std::memory_order_relaxed);
        m_bytes -= old;
        keep_surface(surface->texture, surface->framebuffer, surface->width, surface->height);
        *surface = {};
      }
      if (!take_kept_surface(width, height, surface->texture, surface->framebuffer)) {
        glGenTextures(1, &surface->texture);
        glBindTexture(GL_TEXTURE_2D, surface->texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &surface->framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, surface->framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface->texture, 0);
        ++g_surfaceStats.made;
      }
      const int64_t bytes = static_cast<int64_t>(width) * height * 4;
      g_textureCount[kind].fetch_add(1, std::memory_order_relaxed);
      g_textureBytes[kind].fetch_add(bytes, std::memory_order_relaxed);
      m_bytes += bytes;
      surface->width = static_cast<uint16_t>(width);
      surface->height = static_cast<uint16_t>(height);
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

  // An RGBA8, I8 or IA8 level's bytes: four, one or two a texel.
  uint64_t plain_level_bytes(uint32_t level) const noexcept {
    const uint64_t texel = m_format == TexelFormat::I8 ? 1 : m_format == TexelFormat::IA8 ? 2 : 4;
    return uint64_t{std::max(m_width >> level, 1u)} * std::max(m_height >> level, 1u) * texel;
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
  uint32_t m_levels; // its levels in m_cpu (and on the GPU), back to back; 1 for a copy texture
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
  // (thread::Options::vitaCore, vitaPriority; -1 and 0: any core, `priority`'s); `fallback`, the
  // core it takes when `core` is the system's and the game may not use it (vitaCoreFallback).
  JobThread(const char* name, thread::Priority priority, size_t stack, int core = -1, int vitaPriority = 0,
            int fallback = -1) noexcept
  : m_name(name), m_priority(priority), m_stack(stack), m_core(core), m_vitaPriority(vitaPriority)
  , m_fallback(fallback) {}

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
    thread::set_current({.name = self.m_name,
                         .priority = self.m_priority,
                         .vitaCore = self.m_core,
                         .vitaPriority = self.m_vitaPriority,
                         .vitaCoreFallback = self.m_fallback});
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
  int m_fallback;
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
// 0.42.0 test 18: on the fourth core where CapUnlocker opens it. Free to float, it ran on whichever of
// the game's and the FIFO processor's cores was idle (test 17's thread log: core 1 or 2), at the
// lowest priority, so a new build's warm-up compiled only in those cores' spare time.
JobThread& compiler_thread() noexcept {
  static JobThread& jobs =
      *new JobThread("program compiler", thread::Priority::Low, kCompileStack, thread::kVitaSystemCore);
  return jobs;
}

// The memory card's side of the programs: reading a binary back, writing one, removing one. The
// render worker did these itself, a file's existence asked at each program's first draw too, and
// with the game streaming a room from the disc image on the same card each waited behind its
// reads: 250 to 850 ms stalls at room changes (a run through Tallon Overworld, 7dfaad5).
JobThread& card_thread() noexcept {
  // Was on the render worker's core, just below it as the frame reaper is (170): at its priority a
  // card read held the core for as long as it ran (test 8: 16% busy while the room's programs
  // loaded). But since 0.46.0 the first core is full in the fights (the render worker, the core 0
  // helper, side worker 2 and the DVD worker: 90% and more), and there it starved. In 0.46.0 test 19's
  // Sheegoth fight a program took 1.4 to 6.2 s to load: its draws went fixed-function meanwhile (the
  // Sheegoths black when hit: their hit flash's program), 28 skinned draws a frame at 0.45 ms each,
  // which kept the render worker at 99% and the loads starved, and the music's refills from the disc
  // image on the same card came more than 2 s late (the music repeating and overlapping) while its
  // reads waited behind a starved card thread's. Since test 20 it is on the fourth core, the least
  // busy (the mixer 35%, the side worker 18%), below its three threads (128 to 130); without that
  // core, where it was.
  static JobThread& jobs = thread::vita_core3_available()
                               ? *new JobThread("program cache", thread::Priority::Normal, 128 * 1024,
                                                thread::kVitaSystemCore, 131)
                               : *new JobThread("program cache", thread::Priority::Normal, 128 * 1024, 0, 170);
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

// What vita_programs/build says of the card's program binaries. The user found the map's GPU crash
// came from the shaders an earlier build had left in vita_programs after a new build was installed,
// and went away with them deleted (VITA_PHASE2.md, "The map's GPU crash: stale shaders"); how an old
// binary differs from a new compile was never found (a binary's key names both its sources, and the
// card's binaries were byte-identical to fresh compiles on Nov 2). So from tallon25 to 0.42.0 test 19
// the file named the build that wrote them, and a new build deleted them all: every build began with
// a warm-up of some 900 programs, ~23 minutes (test 18). The user's choice since test 20: keep them.
// The file names their format ("binaries 3", kProgramBinaryVersion), and only binaries of another
// format, or of a card no file vouches for (from before tallon25), are deleted. A build whose
// generated sources differ asks for other keys and leaves the old binaries unread.
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

// What vita_programs/build holds for binaries of this format.
std::string binaries_marker() noexcept { return fmt::format("binaries {}", kProgramBinaryVersion); }

void write_binaries_marker(const std::string& marker) noexcept {
  if (std::FILE* file = std::fopen((g_programCacheDir + "/build").c_str(), "wb"); file != nullptr) {
    std::fwrite(marker.data(), marker.size(), 1, file);
    std::fclose(file);
  }
}

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
  write_binaries_marker(current);
  Log.info("program binaries on the card ({}): {} of {} deleted in {:.0f} ms; this build ({}) writes its own, as \"{}\"",
           previous.empty() ? "no file says what they are" : fmt::format("\"{}\"", previous), removed, files.size(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), this_build(),
           current);
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

// Why the file at `path`, `size` bytes, is not one whole GXP (empty when it is): its magic and
// version, and its header's size (at byte 8: every byte of the program, unpadded) within the file.
// 0.46.0 test 12: a file longer than its header says is kept (`padded`). The check wanted the two
// equal, and the same 8 of 31 files failed it at every start (292 to 480 bytes, vertex and fragment
// shaders alike): vitaGL compiled those 8 again in each session (~145 ms each, two of them in the
// save station's warm-up) and wrote them back, for the next start to delete. A file cut short is
// shorter than its header says, which still fails.
std::string gxp_flaw(const std::filesystem::path& path, uintmax_t size, bool& padded) noexcept {
  padded = false;
  if (size < 16 || size > kMaxBinary) {
    return fmt::format("{} bytes", size);
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {}; // vitaGL cannot read it either, and compiles the shader again
  }
  uint8_t head[12] = {};
  const bool read = std::fread(head, sizeof(head), 1, file) == 1;
  std::fclose(file);
  if (!read) {
    return "its header unread";
  }
  if (std::memcmp(head, "GXP", 4) != 0) {
    return "no GXP magic";
  }
  if (head[4] != 1) {
    return fmt::format("version {}.{}", head[4], head[5]);
  }
  uint32_t field = 0;
  std::memcpy(&field, head + 8, sizeof(field));
  if (field < 16 || field > size) {
    return fmt::format("its header says {} bytes", field);
  }
  padded = field < size;
  return {};
}

// Before vitaGL reads any, the fixed-function cache's files that are not whole GXPs deleted
// (vitaGL compiles a shader again when a draw needs it), and the count logged.
void check_fixed_function_cache() noexcept {
  const auto start = std::chrono::steady_clock::now();
  uint32_t checked = 0;
  uint32_t padded = 0;
  struct Broken {
    std::filesystem::path path;
    uintmax_t size;
    std::string flaw;
  };
  std::vector<Broken> broken;
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
        bool longer = false;
        std::string flaw = gxp_flaw(it->path(), size, longer);
        if (!flaw.empty()) {
          broken.push_back({it->path(), size, std::move(flaw)});
        }
        padded += longer ? 1 : 0;
      }
    }
  }
  // Deleted once listed: not while the folder is read.
  std::string names;
  uint32_t removed = 0;
  for (const Broken& b : broken) {
    std::error_code gone;
    if (!std::filesystem::remove(b.path, gone)) {
      continue;
    }
    if (++removed <= 8) {
      const std::filesystem::path& folder = b.path.parent_path();
      names += fmt::format("{}{}/{}/{} ({} bytes: {})", names.empty() ? "" : ", ",
                           folder.parent_path().filename().string(), folder.filename().string(),
                           b.path.filename().string(), b.size, b.flaw);
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (removed == 0) {
    Log.info("vitaGL's fixed-function shaders ({}): {} on the card, all whole, {} of them longer than their header "
             "says ({:.0f} ms)",
             kFixedFunctionCache, checked, padded, ms);
  } else {
    Log.warn("vitaGL's fixed-function shaders ({}): {} on the card ({} longer than their header says, kept), {} not "
             "whole GXPs deleted ({:.0f} ms): {}{}",
             kFixedFunctionCache, checked, padded, removed, ms, names, removed > 8 ? ", ..." : "");
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
      draw.copyTo != nullptr || g_recording.draws.v.empty()) {
    return false;
  }
  Draw& last = g_recording.draws.v.back();
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
  g_recording.draws.v.push_back(draw);
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

uint32_t recorded_draw_count() noexcept { return static_cast<uint32_t>(g_recording.draws.v.size()); }

Vertex* frame_vertices(uint32_t count, uint32_t& at) noexcept { return g_recording.vertices.grow(count, at); }

uint32_t frame_vertex_room() noexcept { return g_recording.vertices.room(); }

void trim_frame_vertices(uint32_t at) noexcept { g_recording.vertices.trim(at); }

uint32_t frame_indices(const uint16_t* indices, uint32_t count) noexcept {
  uint32_t at = 0;
  std::memcpy(g_recording.indices.grow(count, at), indices, sizeof(uint16_t) * count);
  return at;
}

void end_frame_draws() noexcept {
  const size_t drawn = g_recording.draws.v.size();
  const size_t rows = g_recording.constants.size();
  const size_t kept = g_recording.kept.size();
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
  // The draws' storage from the spares (VectorPool); the vertices come a chunk at a time as they
  // are recorded (FrameChunks).
  g_recording.draws.v = VectorPool<Draw>::get().take(drawn);
  g_recording.constants.reserve(rows);
  g_recording.kept.reserve(kept);
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
  // Draws whose program was not ready, left out rather than drawn fixed-function without any of
  // their textures (left_out_while_pending).
  uint64_t pendingLeftOut = 0;
  // EFB copies made on the GPU, the pixels they wrote, the render worker's time on them (the GPU's
  // is in the frame's), and copies left unmade while the copy program was being built.
  uint32_t copies = 0;
  uint64_t copyPixels = 0;
  uint64_t copyUs = 0;
  uint32_t copiesWaiting = 0;
};
PresentStats g_presentStats;

// 0.46.0 test 6. A draw whose program is still on its way (loaded from the card or compiled, one a
// frame) is drawn fixed-function meanwhile: its units' textures modulated by its vertex colour.
// One with no unit (no stage's colour reads its texture) whose program samples textures has none
// of them so: a flat shape in its vertex colour, at full alpha, where the program would draw what
// they give. A fog volume's last pass (CCubeRenderer::ReallyRenderFogVolume) is one: its colour a
// konst, its alpha the ramp read through two depth copies, its vertices without a colour (white
// for a textured format), blended adding, over the volume's rectangle on screen without the depth
// test. While its program loaded, it was a white rectangle over the lava's fog (0.46.0 tests 4 and
// 5, "the white box"; with the program it adds nothing, the depth copies being black). Such a draw
// that writes no depth is left out until its program is ready; one whose program failed is drawn
// fixed-function as before.
bool left_out_while_pending(const Draw& draw) noexcept {
  if (draw.units[0].texture != nullptr || draw.state.depthWrite) {
    return false;
  }
  return std::any_of(std::begin(draw.program.textures), std::end(draw.program.textures),
                     [](const Texture* texture) { return texture != nullptr; });
}


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

// A frame drawn from fewer pixels than the screen's, sharpened as it is drawn up
// (draw_world_to_display): AMD's contrast adaptive sharpening (CAS) on its cross of texels, less
// where they already differ (no halos), more where they are flat and soft. uConst[0]: a texel's
// size (x, y) and the weight's peak (z: -1 / lerp(8, 5, sharpness)). Built as the copy program is.
constexpr std::string_view kSharpenFragment =
    "float4 main(float2 vUv0 : TEXCOORD0, uniform sampler2D uTex0 : TEXUNIT0, uniform float4 uConst[1]) : COLOR {\n"
    "  float2 d = uConst[0].xy;\n"
    "  float3 c = tex2D(uTex0, vUv0).rgb;\n"
    "  float3 n = tex2D(uTex0, vUv0 + float2(0.0, d.y)).rgb;\n"
    "  float3 s = tex2D(uTex0, vUv0 - float2(0.0, d.y)).rgb;\n"
    "  float3 e = tex2D(uTex0, vUv0 + float2(d.x, 0.0)).rgb;\n"
    "  float3 w = tex2D(uTex0, vUv0 - float2(d.x, 0.0)).rgb;\n"
    "  float3 lo = min(c, min(min(n, s), min(e, w)));\n"
    "  float3 hi = max(c, max(max(n, s), max(e, w)));\n"
    "  float3 amp = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, float3(0.0001))));\n"
    "  float3 weight = amp * uConst[0].z;\n"
    "  return float4((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight), 1.0);\n"
    "}\n";
constexpr uint64_t kSharpenProgramKey = fnv1a(kSharpenFragment, fnv1a(kCopyVertex));
std::shared_ptr<Program> g_sharpenProgram;

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

// vitaGL's framebuffer object (its shared.h's `framebuffer`, as VitaSDK's library is built: 104 bytes,
// no HAVE_ACCURATE_MSAA), for the depth surface it gives a framebuffer at its first scene.
struct VglFramebuffer {
  uint8_t active;
  uint8_t isFloat;
  uint8_t depthbufferState;
  SceGxmRenderTarget* target;
  SceGxmColorSurface colorbuffer;
  SceGxmDepthStencilSurface depthbuffer;
  SceGxmDepthStencilSurface* depthbufferPtr;
  SceGxmTextureFormat format;
  int width;
  int height;
  int stride;
  void* data;
  void* tex;
};
static_assert(sizeof(VglFramebuffer) == 104 && offsetof(VglFramebuffer, depthbuffer) == 56);

// A coordinate rounded to the nearest pixel, halves away from zero (std::lround is a library call).
int round_px(float v) noexcept { return v >= 0.f ? static_cast<int>(v + 0.5f) : -static_cast<int>(0.5f - v); }

// Where a frame's draws go (render worker, set at each present by set_world_target): the display,
// or at another resolution scale (set_render_scale) a framebuffer of that size of its own, drawn to
// the display at the frame's end (draw_world_to_display). Draws carry their rectangles in the
// screen's pixels (ScreenRect), scaled to its pixels here (rect). vitaGL draws a framebuffer upside
// down, so that its texture's first row is the frame's bottom row (copy_on_gpu reads it so); its
// viewport, scissor and culling it turns itself. Its depth surface is made at its first scene.
struct WorldTarget {
  bool scaled = false;
  int width = kScreenWidth;
  int height = kScreenHeight;
  float sx = 1.f; // its pixels a screen pixel
  float sy = 1.f;
  GLuint texture = 0;
  GLuint framebuffer = 0; // 0: the display
  VglFramebuffer* vgl = nullptr;

  ScreenRect rect(const ScreenRect& r) const noexcept {
    if (!scaled) {
      return r;
    }
    const int x0 = round_px(static_cast<float>(r.x) * sx);
    const int x1 = round_px(static_cast<float>(r.x + r.width) * sx);
    const int y0 = round_px(static_cast<float>(r.y) * sy);
    const int y1 = round_px(static_cast<float>(r.y + r.height) * sy);
    return {static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0),
            static_cast<int16_t>(y1 - y0)};
  }
  // The depth surface its draws test against: the display's, or its framebuffer's.
  SceGxmDepthStencilSurface* depth() const noexcept {
    return scaled && vgl != nullptr ? &vgl->depthbuffer : &gxm_depth_stencil_surface;
  }
};
WorldTarget g_world;

// The frame size asked for (set_render_scale: width << 16 | height), and the last one that could not
// be made (not tried again till another is asked for).
std::atomic<uint32_t> g_renderSize{static_cast<uint32_t>(kScreenWidth) << 16 | kScreenHeight};
uint32_t g_renderSizeFailed = 0;
std::atomic<float> g_sharpness{0.f};

// 0.46.0 test 12, frame pacing: vitaGL's swap interval asked for (set_swap_interval) and the one
// given it (render worker), and how many vblanks apart the presents came since the last report (1
// to 4, then 5 or more; sceDisplayGetVcount after each swap).
std::atomic<uint32_t> g_swapInterval{1};
uint32_t g_swapApplied = 1;
uint32_t g_lastVcount = 0;
std::array<uint32_t, 5> g_pacing{};

// The scenes vitaGL sizes a render target for (vglSetupRenderTargetScenesNum): the display's, which
// takes a frame's copies (record_copy makes 7 a frame at most) and one, and a copy's framebuffer's.
// A frame drawn at another scale takes the display's for its framebuffer, made with it.
constexpr uint8_t kDisplayScenes = 8;
constexpr uint8_t kCopyScenes = 4;

void release_world_target() noexcept {
  WorldTarget& t = g_world;
  if (t.framebuffer != 0 || t.texture != 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &t.framebuffer);
    glDeleteTextures(1, &t.texture);
  }
  t = {};
}

// The frame's framebuffer at `width` x `height` (render worker), its texture sampled linear (the
// copies set their own filter: bind_world_texture). False when its texture could not be had.
bool make_world_target(int width, int height) noexcept {
  const uint64_t start = sceKernelGetProcessTimeWide();
  WorldTarget& t = g_world;
  glGenTextures(1, &t.texture);
  glBindTexture(GL_TEXTURE_2D, t.texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  if (vglGetTexDataPointer(GL_TEXTURE_2D) == nullptr) {
    glDeleteTextures(1, &t.texture);
    t = {};
    return false;
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &t.framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
  t.vgl = static_cast<VglFramebuffer*>(active_write_fb);
  t.scaled = true;
  t.width = width;
  t.height = height;
  t.sx = static_cast<float>(width) / static_cast<float>(kScreenWidth);
  t.sy = static_cast<float>(height) / static_cast<float>(kScreenHeight);
  Log.info("frames drawn at {}x{} ({:.2f}x the screen), drawn to it at the end: their framebuffer made in {} us", width,
           height, t.sx, sceKernelGetProcessTimeWide() - start);
  return true;
}

// The frame's target for this present (render worker), bound: the display, or the framebuffer at the
// size asked for, made (again) here for a new size. True when the framebuffer is new: its first
// scene (the frame's clear) makes its render target and depth surface.
bool set_world_target() noexcept {
  const uint32_t size = g_renderSize.load(std::memory_order_relaxed);
  const int width = static_cast<int>(size >> 16);
  const int height = static_cast<int>(size & 0xFFFF);
  WorldTarget& t = g_world;
  if ((width == kScreenWidth && height == kScreenHeight) || size == g_renderSizeFailed) {
    if (t.scaled) {
      release_world_target();
      Log.info("frames drawn at the screen's {}x{} again", kScreenWidth, kScreenHeight);
      log_memory("screen size");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return false;
  }
  bool fresh = false;
  if (!t.scaled || t.width != width || t.height != height) {
    release_world_target();
    if (!make_world_target(width, height)) {
      g_renderSizeFailed = size;
      Log.warn("frames not drawn at {}x{}: no room for its framebuffer; at the screen's size instead", width, height);
      log_memory("no frame buffer");
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      return false;
    }
    g_renderSizeFailed = 0;
    log_memory("frame buffer");
    fresh = true;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
  return fresh;
}

// The frame's target bound again for drawing (after a copy into another framebuffer).
void bind_world() noexcept { glBindFramebuffer(GL_FRAMEBUFFER, g_world.framebuffer); }

// The GPU's busy time (render worker), for dynamic resolution and the log. GXM gives no timestamps
// here, but each scene vitaGL sends ends with its fence (query_fence), so the GPU is busy from a
// scene sent while it had none till the last one sent is done. The render worker looks every few
// draws, at each copy and around the swap (two loads), and times the moments it changes. A frame's
// last scene done while the render worker waited for the next frame is timed by the display
// callback (vglSetDisplayCallback: GXM's display thread, once the frame is drawn, before its flip).
// A scene's vertex work while it is being recorded is not seen: what it times is mostly pixels.
struct GpuClock {
  static constexpr uint32_t kRing = 8;
  bool busy = false;
  uint64_t since = 0;
  uint64_t busyUs = 0; // this window's
  uint32_t frames = 0;
  uint64_t windowStart = 0;
  uint32_t swaps = 0;
  uint32_t swapScene[kRing] = {}; // each swap's last scene (the display's)
  bool callbackOff = false;       // the display callbacks ran ahead of the swaps: not used
  // Since the last report (present's log line).
  uint64_t reportBusyUs = 0;
  uint64_t reportUs = 0;
  uint32_t reportFrames = 0;
  // Every busy stretch ended so far, and the open one's part counted into a window (the GPU cost
  // test reads it: gpu_busy_total).
  uint64_t totalBusyUs = 0;
};
GpuClock g_gpu;
std::atomic<uint32_t> g_displayed{0};
std::atomic<uint64_t> g_displayedAt[GpuClock::kRing];
std::atomic<float> g_gpuMs{0.f};

// GXM's display thread: each frame's callback, once it is drawn, in the swaps' order.
void on_display(void*) {
  const uint32_t n = g_displayed.load(std::memory_order_relaxed);
  g_displayedAt[n % GpuClock::kRing].store(sceKernelGetProcessTimeWide(), std::memory_order_relaxed);
  g_displayed.store(n + 1, std::memory_order_release);
}

void gpu_changed(bool busy, uint32_t sent) noexcept {
  uint64_t now = sceKernelGetProcessTimeWide();
  GpuClock& g = g_gpu;
  if (busy) {
    g.busy = true;
    g.since = now;
    return;
  }
  // Done: when the display callback says, if the last scene sent was a swap's (the render worker may
  // have been waiting for the next frame since).
  const uint32_t swaps = g.swaps;
  if (swaps != 0 && !g.callbackOff && g.swapScene[(swaps - 1) % GpuClock::kRing] == sent) {
    const uint32_t displayed = g_displayed.load(std::memory_order_acquire);
    if (displayed > swaps) {
      g.callbackOff = true;
      Log.warn("GPU clock: {} display callbacks for {} swaps; timing at the render worker's looks alone", displayed,
               swaps);
    } else if (displayed == swaps) {
      const uint64_t at = g_displayedAt[(swaps - 1) % GpuClock::kRing].load(std::memory_order_relaxed);
      if (at >= g.since && at < now) {
        now = at;
      }
    }
  }
  g.busyUs += now - g.since;
  g.totalBusyUs += now - g.since;
  g.busy = false;
}

// The GPU's busy time so far, the stretch still open counted to `now` (render worker).
uint64_t gpu_busy_total(uint64_t now) noexcept {
  return g_gpu.totalBusyUs + (g_gpu.busy && now > g_gpu.since ? now - g_gpu.since : 0);
}

// A look at the fence (render worker).
inline void gpu_watch() noexcept {
  const uint32_t sent = query_fence.value;
  const bool busy = *query_fence.address != sent;
  if (busy != g_gpu.busy) {
    gpu_changed(busy, sent);
  }
}

// After a swap: the frame counted, its display scene noted; once half a second has gone the window's
// busy time a frame published (an open busy stretch counted to now, and on into the next window).
void gpu_swapped(uint64_t swapStart) noexcept {
  GpuClock& g = g_gpu;
  g.swapScene[g.swaps % GpuClock::kRing] = query_fence.value;
  ++g.swaps;
  // The swap sent the display's scene: a GPU idle till then is busy from the swap's start.
  if (!g.busy && *query_fence.address != query_fence.value) {
    g.busy = true;
    g.since = swapStart;
  }
  gpu_watch();
  ++g.frames;
  const uint64_t now = sceKernelGetProcessTimeWide();
  if (g.windowStart == 0) {
    g.windowStart = now;
    g.busyUs = 0;
    g.frames = 0;
    return;
  }
  if (now - g.windowStart < 500000 || g.frames < 4) {
    return;
  }
  uint64_t busy = g.busyUs;
  if (g.busy) {
    busy += now - g.since;
    g.totalBusyUs += now - g.since;
    g.since = now;
  }
  g_gpuMs.store(static_cast<float>(static_cast<double>(busy) / 1000.0 / g.frames), std::memory_order_relaxed);
  g.reportBusyUs += busy;
  g.reportUs += now - g.windowStart;
  g.reportFrames += g.frames;
  g.busyUs = 0;
  g.frames = 0;
  g.windowStart = now;
}

// 0.46.0 test 14: the GPU cost test (start_gpu_cost_test, from the Port menu's Performance tab).
// Test 12's heavy scenes kept the GPU busy 40 to 52 ms a frame, more than anything else, and the
// clock above times frames whole. With the player standing still where the frame rate is low, the
// test leaves one kind of draw out at a time, each for 1.2 s after 0.4 s for the GPU to catch up (it
// runs a frame or two behind), and logs the GPU's busy time a frame and the frame time without it:
// what each kind costs the GPU. The picture is wrong while it runs (about 15 s).
namespace gpu_test {
enum : uint32_t {
  kCopies = 1u << 0,      // EFB copies and their palette conversions
  kImmediate = 1u << 1,   // immediate and indexed draws (particles, snow, beams, the HUD's quads)
  kBlended = 1u << 2,     // every draw that blends
  kAlphaTested = 1u << 3, // every draw with an alpha test
  kSkinned = 1u << 4,     // skinned calls
  kLists = 1u << 5,       // display lists (the rooms, most models)
  kDraws = 1u << 6,       // every draw (the clears and the copies stay)
};
// Test 15 drew a kind of draw into one pixel (a 1x1 scissor) to leave its pixels out, its vertices
// in; but the scissor changed at each draw, and those changes cost the GPU more than the pixels
// saved (+7 to +17 ms in the Triclops room). Test 16 keeps one such phase, every draw, its scissor
// set once a frame. And an "everything" phase comes after every two others: a phase's change is
// against the mean of the two around it, so the scene's own drift (an enemy coming closer) is
// mostly taken out.
enum : uint32_t {
  kTinyAll = 1u << 0,
};
struct Phase {
  const char* name;
  uint32_t skip;
  uint32_t tiny;
};
constexpr Phase kPhases[] = {
    {"everything", 0, 0},
    {"no EFB copies", kCopies, 0},
    {"no immediate draws", kImmediate, 0},
    {"everything", 0, 0},
    {"no blended draws", kBlended, 0},
    {"no alpha-tested draws", kAlphaTested, 0},
    {"everything", 0, 0},
    {"no skinned draws", kSkinned, 0},
    {"no display lists", kLists, 0},
    {"everything", 0, 0},
    {"every draw into one pixel (its vertices kept, its pixels not)", 0, kTinyAll},
    {"no draws (the clears and copies alone)", kDraws, 0},
    {"everything", 0, 0},
};
constexpr size_t kPhaseCount = std::size(kPhases);
constexpr uint64_t kSettleUs = 400000;
constexpr uint64_t kMeasureUs = 1200000;
std::atomic<bool> g_start{false};
uint32_t g_skip = 0;     // render worker: what the draws leave out now
uint32_t g_tiny = 0;     // render worker: what the draws draw into one pixel now
bool g_counting = false; // render worker: the first phase counts its draws by kind
struct Counts {
  uint64_t frames = 0;
  uint64_t copies = 0;
  uint64_t immediate = 0;
  uint64_t blended = 0;
  uint64_t alphaTested = 0;
  uint64_t skinned = 0;
  uint64_t lists = 0;
  uint64_t draws = 0;
};
Counts g_counts;
struct Result {
  double gpuMs = 0.0;
  double frameMs = 0.0;
  uint32_t frames = 0;
};
struct State {
  int phase = -1; // -1: not running
  uint64_t phaseStart = 0;
  bool measuring = false;
  uint64_t measureStart = 0;
  uint64_t busyStart = 0;
  uint32_t swapsStart = 0;
  Result results[kPhaseCount];
};
State g_state;

MeshKind kind_of(const Draw& draw) noexcept { return draw.mesh != nullptr ? draw.mesh->kind() : draw.frameMesh.kind; }

// Whether the test leaves this draw out now (render worker).
bool skips(const Draw& draw) noexcept {
  const uint32_t s = g_skip;
  if ((s & kDraws) != 0) {
    return true;
  }
  const MeshKind kind = kind_of(draw);
  return ((s & kImmediate) != 0 && (kind == MeshKind::Immediate || kind == MeshKind::Indexed)) ||
         ((s & kBlended) != 0 && draw.state.blend) ||
         ((s & kAlphaTested) != 0 && draw.state.alphaFunc != GL_ALWAYS) ||
         ((s & kSkinned) != 0 && kind == MeshKind::Skinned) || ((s & kLists) != 0 && kind == MeshKind::List);
}

void count(const Draw& draw) noexcept {
  const MeshKind kind = kind_of(draw);
  Counts& c = g_counts;
  ++c.draws;
  c.immediate += kind == MeshKind::Immediate || kind == MeshKind::Indexed ? 1 : 0;
  c.blended += draw.state.blend ? 1 : 0;
  c.alphaTested += draw.state.alphaFunc != GL_ALWAYS ? 1 : 0;
  c.skinned += kind == MeshKind::Skinned ? 1 : 0;
  c.lists += kind == MeshKind::List ? 1 : 0;
}

void log_results() noexcept {
  const State& st = g_state;
  const double base = st.results[0].gpuMs;
  std::string text;
  for (size_t i = 0; i < kPhaseCount; ++i) {
    const Result& r = st.results[i];
    const bool everything = kPhases[i].skip == 0 && kPhases[i].tiny == 0;
    std::string delta;
    if (!everything) {
      // Against the mean of the "everything" phases just before and just after it.
      size_t before = i;
      while (before > 0 && !(kPhases[before].skip == 0 && kPhases[before].tiny == 0)) {
        --before;
      }
      size_t after = i;
      while (after + 1 < kPhaseCount && !(kPhases[after].skip == 0 && kPhases[after].tiny == 0)) {
        ++after;
      }
      const double ref = (st.results[before].gpuMs + st.results[after].gpuMs) / 2.0;
      delta = fmt::format(" ({:+.1f} against the {:.1f} around it)", r.gpuMs - ref, ref);
    }
    text += fmt::format("{}{}: GPU {:.1f} ms a frame{}, frame {:.1f} ms ({} frames)", i != 0 ? "; " : "",
                        kPhases[i].name, r.gpuMs, delta, r.frameMs, r.frames);
  }
  const double f = static_cast<double>(std::max<uint64_t>(g_counts.frames, 1));
  // The two "everything" phases apart by more than a sixth: the scene changed while it ran.
  const double last = st.results[kPhaseCount - 1].gpuMs;
  const double drift = base > 0.0 ? std::abs(last - base) / base : 0.0;
  const std::string changed =
      drift > 1.0 / 6.0 ? fmt::format(" THE SCENE CHANGED while it ran (its two \"everything\" phases {:.0f}% apart): "
                                      "read it with care.",
                                      100.0 * drift)
                        : std::string{};
  Log.info("GPU cost test: {}. The first phase's draws a frame: {:.1f} EFB copies; {:.0f} draws, {:.0f} of them "
           "immediate, {:.0f} blended, {:.0f} alpha-tested, {:.0f} skinned, {:.0f} display lists.{}",
           text, g_counts.copies / f, g_counts.draws / f, g_counts.immediate / f, g_counts.blended / f,
           g_counts.alphaTested / f, g_counts.skinned / f, g_counts.lists / f, changed);
}

// Render worker, after each swap: starts the test when asked, and moves it on.
void step() noexcept {
  const uint64_t now = sceKernelGetProcessTimeWide();
  State& st = g_state;
  if (g_start.exchange(false, std::memory_order_acq_rel)) {
    st = {};
    st.phase = 0;
    st.phaseStart = now;
    g_skip = kPhases[0].skip;
    g_tiny = kPhases[0].tiny;
    g_counts = {};
    Log.info("GPU cost test: started, {} phases of {:.1f} s; parts of the picture are left out till it ends",
             kPhaseCount, static_cast<double>(kSettleUs + kMeasureUs) / 1e6);
    return;
  }
  if (st.phase < 0) {
    return;
  }
  if (!st.measuring) {
    if (now - st.phaseStart >= kSettleUs) {
      st.measuring = true;
      st.measureStart = now;
      st.busyStart = gpu_busy_total(now);
      st.swapsStart = g_gpu.swaps;
      g_counting = st.phase == 0;
    }
    return;
  }
  if (now - st.measureStart < kMeasureUs) {
    return;
  }
  Result& r = st.results[st.phase];
  r.frames = g_gpu.swaps - st.swapsStart;
  if (r.frames != 0) {
    r.gpuMs = static_cast<double>(gpu_busy_total(now) - st.busyStart) / 1000.0 / r.frames;
    r.frameMs = static_cast<double>(now - st.measureStart) / 1000.0 / r.frames;
  }
  g_counting = false;
  ++st.phase;
  if (st.phase == static_cast<int>(kPhaseCount)) {
    log_results();
    g_skip = 0;
    g_tiny = 0;
    st.phase = -1;
    return;
  }
  g_skip = kPhases[st.phase].skip;
  g_tiny = kPhases[st.phase].tiny;
  st.phaseStart = now;
  st.measuring = false;
}
} // namespace gpu_test

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

// The frame as its draws so far left it, bound (render worker): the display's back buffer, or a
// scaled frame's texture (its framebuffer's), sampled linear for a halved copy.
void bind_world_texture(bool linear) noexcept {
  if (!g_world.scaled) {
    bind_screen_texture(linear);
    return;
  }
  glBindTexture(GL_TEXTURE_2D, g_world.texture);
  const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
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
// A frame drawn at another scale keeps its framebuffer's depth so (WorldTarget::depth); vitaGL makes
// that surface at the framebuffer's first scene, as it starts, so a new one's first frame keeps none.
void keep_depth_of(SceGxmDepthStencilSurface* surface, bool load, bool store) noexcept {
  sceGxmDepthStencilSurfaceSetForceLoadMode(surface, load ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
                                                          : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
  sceGxmDepthStencilSurfaceSetForceStoreMode(surface, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                            : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
}

void keep_depth(bool load, bool store) noexcept { keep_depth_of(g_world.depth(), load, store); }

// An EFB copy, made on the GPU (render worker): the display's back buffer, as the draws so far left
// it, drawn through the copy program into the destination's texture (its framebuffer), with
// nothing waiting for the GPU. Drawing there ends the display's scene; the next draw on the display
// starts another (keep_depth: loading the depth this one stored, storing it again where another
// copy follows). The caller sets the display's viewport and scissor again, and binds every unit's
// texture again (unit 0 took the screen's, and the destination may now be another of its sizes).
// False when there was nothing to copy.
bool copy_on_gpu(const Draw& draw, Program& program, GLuint& usedProgram, uint32_t& attribsOn, RenderState& current,
                 bool moreCopies) noexcept {
  // In the frame's pixels: a frame drawn at another scale copies at that scale too (its destination
  // as many texels smaller or larger; GX's coordinates sample it as a whole all the same).
  const ScreenRect src = g_world.rect(draw.scissor);
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
  bind_world_texture(step == 2);
  float rows[5][4];
  copy_rows(draw.copyKind, draw.copyAlpha, rows);
  program.set_rows(rows, 5);
  // The rectangle's texels in the frame's texture: the display's, whose rows run down from the
  // screen's top (GL's rectangle up from its bottom), or a scaled frame's framebuffer's, whose rows
  // run up from its bottom. vitaGL keeps a framebuffer's texture the right way up for sampling: its
  // first row is the draw's bottom (y -1), and takes the rectangle's top, as a GX copy starts with
  // it. Halved, each pixel samples its 2x2 block's middle, linear: GX's box filter.
  const auto frameWidth = static_cast<float>(g_world.width);
  const auto frameHeight = static_cast<float>(g_world.height);
  const float u0 = static_cast<float>(src.x) / frameWidth;
  const float u1 = static_cast<float>(src.x + width * step) / frameWidth;
  float v0 = 0.f;
  float v1 = 0.f;
  if (g_world.scaled) {
    v0 = static_cast<float>(src.y + src.height) / frameHeight;
    v1 = v0 - static_cast<float>(height * step) / frameHeight;
  } else {
    v0 = static_cast<float>(g_world.height - (src.y + src.height)) / frameHeight;
    v1 = v0 + static_cast<float>(height * step) / frameHeight;
  }
  const float quad[4][4] = {{-1.f, -1.f, u0, v0}, {1.f, -1.f, u1, v0}, {-1.f, 1.f, u0, v1}, {1.f, 1.f, u1, v1}};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(program.attr(kAttrPos), 2, GL_FLOAT, GL_FALSE, sizeof(quad[0]), &quad[0][0]);
  glVertexAttribPointer(program.attr(kAttrUv0), 2, GL_FLOAT, GL_FALSE, sizeof(quad[0]), &quad[0][2]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  keep_depth(true, moreCopies);
  bind_world();
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
  bind_world();
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
  const std::vector<Draw>& draws = frame.draws.v;
  if (draws.empty()) {
    return;
  }
  ++g_presentStats.framesWithDraws;
  g_presentStats.draws += draws.size();
  g_presentStats.merged += frame.merged;
  if (gpu_test::g_counting) {
    ++gpu_test::g_counts.frames;
  }
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
  // is left in use); and the sharpening one, for a frame drawn at a lower scale.
  for (Program* copier : {g_copyProgram.get(), g_paletteProgram.get(), g_sharpenProgram.get()}) {
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
  // GX's viewport and scissor as the draws carry them (the screen's pixels), set where they change,
  // in the frame's own pixels (WorldTarget::rect); the scissor test stays on through the draws
  // (present's clear and the overlay run without it).
  ScreenRect viewport;
  ScreenRect scissor;
  // The GPU cost test's "every draw into one pixel": the scissor 1x1 for the whole frame, set once.
  const bool tinyAll = (gpu_test::g_tiny & gpu_test::kTinyAll) != 0;
  const auto apply_rects = [&viewport, &scissor, tinyAll]() {
    const ScreenRect v = g_world.rect(viewport);
    const ScreenRect s = g_world.rect(scissor);
    glViewport(v.x, v.y, v.width, v.height);
    if (tinyAll) {
      glScissor(0, 0, 1, 1);
    } else {
      glScissor(s.x, s.y, s.width, s.height);
    }
  };
  apply_rects();
  glEnable(GL_SCISSOR_TEST);
  // The line and point width, set where a line or point draw changes it (GXM's context keeps it,
  // for the programs and the fixed-function path alike). Points in the frame's pixels; lines stay a
  // pixel wide (line_width in gx/vita_draw.cpp: GXM's wide lines hang the GPU).
  uint8_t width = 1;
  glLineWidth(1.f);
  glPointSize(1.f);
  const auto set_rects = [&viewport, &scissor, tinyAll](const ScreenRect& v, const ScreenRect& s) {
    if (!(v == viewport)) {
      const ScreenRect r = g_world.rect(v);
      glViewport(r.x, r.y, r.width, r.height);
      viewport = v;
    }
    if (!(s == scissor)) {
      if (!tinyAll) {
        const ScreenRect r = g_world.rect(s);
        glScissor(r.x, r.y, r.width, r.height);
      }
      scissor = s;
    }
  };
  uint32_t drawIndex = 0;
  g_clock.mark(kSetup);
  if (g_clock.on) {
    g_clock.draws += draws.size();
  }
  for (const Draw& draw : draws) {
    // The GPU's clock: whether the scenes sent so far are done, every few draws (GpuClock).
    if ((++drawIndex & 3) == 0) {
      gpu_watch();
    }
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
      gpu_test::g_counts.copies += gpu_test::g_counting ? 1 : 0;
      if ((gpu_test::g_skip & gpu_test::kCopies) != 0) {
        continue; // the GPU cost test's: the destination keeps what it held
      }
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
        // The copy's draw sent the frame's scene to the GPU.
        gpu_watch();
        apply_rects();
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
    if (gpu_test::g_counting) {
      gpu_test::count(draw);
    }
    if (gpu_test::g_skip != 0 && gpu_test::skips(draw)) {
      continue; // the GPU cost test leaves this kind out now
    }
    set_rects(draw.viewport, draw.scissor);
    const MeshPrimitive primitive = draw.mesh != nullptr ? draw.mesh->primitive() : draw.frameMesh.primitive;
    if (primitive != MeshPrimitive::Triangles && draw.width != width) {
      glLineWidth(static_cast<GLfloat>(draw.width));
      const int point = g_world.scaled ? std::clamp(round_px(static_cast<float>(draw.width) * g_world.sx), 1, 16)
                                       : draw.width;
      glPointSize(static_cast<GLfloat>(point));
      width = draw.width;
    }
    Program* program = draw.program.program;
    bool leaveOut = false;
    if (program != nullptr) {
      const int budgetBefore = buildBudget;
      const bool ready = program->ready(buildBudget);
      if (buildBudget != budgetBefore) {
        // A program built just now was made current to set its samplers: back to the one in use.
        glUseProgram(usedProgram);
      }
      if (!ready) {
        leaveOut = !program->settled() && left_out_while_pending(draw);
        program = nullptr;
      }
      g_clock.mark(kBuild);
    }
    if (leaveOut) {
      ++g_presentStats.pendingLeftOut;
      continue;
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

// A frame drawn at another scale, drawn to the display (render worker, after draw_world): its
// texture over the whole screen, linear (a frame twice the screen's size so takes each 2x2 block's
// mean), through the copy program as an opaque copy, or sharpened where it is drawn up from fewer
// pixels and sharpening is on (aurora_set_present_sharpness). This starts the display's scene,
// which keeps no depth. Till the copy program is built the display shows what its scene loads.
void draw_world_to_display() noexcept {
  keep_depth_of(&gxm_depth_stencil_surface, false, false);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, kWidth, kHeight);
  glDisable(GL_SCISSOR_TEST);
  int budget = 1;
  const float sharpness = g_world.width < kWidth ? g_sharpness.load(std::memory_order_relaxed) : 0.f;
  Program* program = nullptr;
  if (sharpness > 0.f && g_sharpenProgram->ready(budget)) {
    program = g_sharpenProgram.get();
  } else if (g_copyProgram->ready(budget)) {
    program = g_copyProgram.get();
  }
  if (program == nullptr || program->attr(kAttrPos) < 0 || program->attr(kAttrUv0) < 0) {
    return;
  }
  GLuint usedProgram = 0;
  uint32_t attribsOn = 0;
  use_program(*program, usedProgram, attribsOn);
  RenderState current;
  apply_state(RenderState{.depthTest = false, .depthWrite = false}, current, true);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_world.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (program == g_sharpenProgram.get()) {
    const float peak = -1.f / (8.f + (5.f - 8.f) * std::clamp(sharpness, 0.f, 1.f));
    const float rows[1][4] = {{1.f / static_cast<float>(g_world.width), 1.f / static_cast<float>(g_world.height), peak,
                               0.f}};
    program->set_rows(rows, 1);
  } else {
    float rows[5][4];
    copy_rows(CopyKind::Rgb, true, rows);
    program->set_rows(rows, 5);
  }
  // The framebuffer's texture is the right way up (its first row the frame's bottom), as the
  // display's bottom (y -1) takes it.
  constexpr float kQuad[4][4] = {{-1.f, -1.f, 0.f, 0.f}, {1.f, -1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f, 1.f}, {1.f, 1.f, 1.f, 1.f}};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(program->attr(kAttrPos), 2, GL_FLOAT, GL_FALSE, sizeof(kQuad[0]), &kQuad[0][0]);
  glVertexAttribPointer(program->attr(kAttrUv0), 2, GL_FLOAT, GL_FALSE, sizeof(kQuad[0]), &kQuad[0][2]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  // Back to the fixed-function path, as draw_world leaves it for the overlay.
  for (int i = 0; i < 32; ++i) {
    if ((attribsOn & (1u << i)) != 0) {
      glDisableVertexAttribArray(i);
    }
  }
  glUseProgram(0);
  // Unit 0 the client's too, as draw_world leaves it: use_program left the last one so (turning each
  // unit's coordinates off), and the overlay's backend (ImGui's GL2) gives its coordinates to the
  // client's unit and samples its font on unit 0. Each glyph then read one texel of the font's atlas,
  // the white corner: solid boxes for text at any scale but 1x (0.46.0 test 31).
  glActiveTexture(GL_TEXTURE0);
  glClientActiveTexture(GL_TEXTURE0);
  apply_state(RenderState{.depthTest = false}, current, true);
}

// A thread_local the clock costs' probe touches: emulated TLS here (__emutls_get_address).
thread_local int t_costProbe = 0;
__attribute__((noinline)) void touch_thread_local() noexcept { ++t_costProbe; }

// What the clocks and locks the game leans on cost here, once at the start, for the log: a frame
// reads OSGetTime thousands of times (every CStopwatch), and the game clock behind it took a
// std::shared_mutex read lock each time until 0.42.0 test 17 (vitasdk's pthread rwlock: kernel
// mutex calls). The shared_mutex and mutex rows are those locks alone, uncontended; the
// thread_local row one access to one (the particles' scopes took two an update until test 17).
void log_clock_costs() noexcept {
  constexpr int kCalls = 2000;
  const auto per_call = [](auto&& call) {
    const uint64_t start = sceKernelGetProcessTimeWide();
    for (int i = 0; i < kCalls; ++i) {
      call();
    }
    return static_cast<double>(sceKernelGetProcessTimeWide() - start) / kCalls;
  };
  volatile int64_t sink = 0;
  const double osTime = per_call([&sink] { sink = sink + OSGetTime(); });
  const double steady = per_call([&sink] { sink = sink + std::chrono::steady_clock::now().time_since_epoch().count(); });
  const double process = per_call([&sink] { sink = sink + static_cast<int64_t>(sceKernelGetProcessTimeWide()); });
  std::shared_mutex shared;
  const double sharedLock = per_call([&shared] {
    shared.lock_shared();
    shared.unlock_shared();
  });
  std::mutex mutex;
  const double mutexLock = per_call([&mutex] {
    mutex.lock();
    mutex.unlock();
  });
  const double threadLocal = per_call([] { touch_thread_local(); });
  Log.info("clock costs (us a call, over {}): OSGetTime {:.2f}, steady_clock::now {:.2f}, sceKernelGetProcessTimeWide "
           "{:.2f}; a std::shared_mutex read lock and unlock {:.2f}, a std::mutex lock and unlock {:.2f}, a "
           "thread_local access {:.2f}",
           kCalls, osTime, steady, process, sharedLock, mutexLock, threadLocal);
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
  // The generated programs' own cache (Program, read_cached), and what it holds: the binaries of
  // this format, whichever build wrote them (program_binaries_build).
  g_programCacheDir = std::string{g_config.userPath} + "/vita_programs";
  std::filesystem::create_directories(g_programCacheDir, ec);
  const std::string marker = binaries_marker();
  const std::string found = program_binaries_build();
  if (found == marker) {
    index_card();
  } else if (!found.empty() && !found.starts_with("binaries ")) {
    // A build's revision, as tallon25 to test 19 wrote: binaries of this format (the key's seed has
    // been 3 since before tallon25), kept.
    index_card();
    write_binaries_marker(marker);
    Log.info("program binaries on the card from build {}: {} kept (\"{}\"), as every build keeps them now", found,
             g_cardKeys.size(), marker);
  } else if (!card_thread().submit([found, marker] { delete_program_binaries(found, marker); })) {
    Log.warn("no card thread: the program binaries of another format stay on the card, unread");
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
  g_sharpenProgram = make_program(std::string(kCopyVertex), std::string(kSharpenFragment), kSharpenProgramKey);
  // The GPU's clock: each frame's end, as the display thread sees it drawn (GpuClock).
  vglSetDisplayCallback(on_display);
  g_active = true;
  if (clamped) {
    Log.warn("vitaGL started at the display's maximum size, not {}x{}", kWidth, kHeight);
  } else {
    Log.info("vitaGL started at {}x{}", kWidth, kHeight);
  }
  log_memory("started");
  log_clock_costs();
  return true;
}

bool active() noexcept { return g_active; }

void render_size_for_scale(float scale, int& width, int& height) noexcept {
  if (!(scale > 0.f)) {
    width = kScreenWidth;
    height = kScreenHeight;
    return;
  }
  scale = std::clamp(scale, 0.25f, kMaxRenderScale);
  width = std::max(8, static_cast<int>(static_cast<float>(kScreenWidth) * scale / 8.f + 0.5f) * 8);
  height = std::max(2, static_cast<int>(static_cast<float>(kScreenHeight) * scale / 2.f + 0.5f) * 2);
}

void set_render_scale(float scale) noexcept {
  int width = 0;
  int height = 0;
  render_size_for_scale(scale, width, height);
  g_renderSize.store(static_cast<uint32_t>(width) << 16 | static_cast<uint32_t>(height), std::memory_order_relaxed);
}

void set_present_sharpness(float sharpness) noexcept {
  g_sharpness.store(std::clamp(sharpness, 0.f, 1.f), std::memory_order_relaxed);
}

float gpu_busy_ms() noexcept { return g_gpuMs.load(std::memory_order_relaxed); }

void start_gpu_cost_test() noexcept { gpu_test::g_start.store(true, std::memory_order_release); }

void set_swap_interval(uint32_t vblanks) noexcept {
  g_swapInterval.store(std::clamp<uint32_t>(vblanks, 1, 4), std::memory_order_relaxed);
}

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
  // Its text stands however it was closed, with Enter or its close button, as SDL's own Vita
  // keyboard takes it (SDL_vitavideo.c): only a dialog that ended without one gives nothing.
  const bool withText = result.button == SCE_IME_DIALOG_BUTTON_ENTER || result.button == SCE_IME_DIALOG_BUTTON_CLOSE;
  Log.info("the system keyboard closed ({}, result {:#x})",
           result.button == SCE_IME_DIALOG_BUTTON_ENTER   ? "Enter"
           : result.button == SCE_IME_DIALOG_BUTTON_CLOSE ? "its close button"
                                                          : "no button",
           static_cast<uint32_t>(result.result));
  k.state = withText ? Keyboard::State::Confirmed : Keyboard::State::Cancelled;
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
  Frame frame = take_frame();
  // A scene the GPU finished while this waited for the frame (timed by the display callback).
  gpu_watch();
  // The display, or the framebuffer of a frame drawn at another scale (set_render_scale).
  const bool freshTarget = set_world_target();
  glViewport(0, 0, g_world.width, g_world.height);
  glDisable(GL_SCISSOR_TEST);
  // Black, as the GameCube's frame starts (a dark blue told this path's screen apart at first), and
  // alpha 0, the game's clear colour's (CCubeRenderer::BeginScene), which an EFB with alpha starts
  // from (the display shows no alpha, and an EFB without it reads one: dst_alpha_mode).
  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClearDepth(1.0);
  // The clear follows the masks; a draw's state may have left them off.
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  // The clear starts the frame's first scene: it loads no depth, and stores it for a copy to come. A
  // new framebuffer's render target is made at it, sized for the scenes a frame's copies make.
  keep_depth(false, frame.copies != 0);
  if (freshTarget) {
    vglSetupRenderTargetScenesNum(kDisplayScenes, kDisplayScenes);
  }
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (freshTarget) {
    vglSetupRenderTargetScenesNum(kDisplayScenes, kCopyScenes);
  }
  draw_world(frame);
  if (g_world.scaled) {
    draw_world_to_display();
  }
  warm_up_step();
  g_clock.mark(kBuild);
  g_reaper.reap(std::move(frame));
  g_clock.mark(kFinish);
  if (++g_presentStats.presents == 300) {
    Log.info("presents: {}, {} with game draws, {:.1f} draws a frame ({:.1f} more joined to the draw before them), {} "
             "destroyed here with the reaper two behind; "
             "generated programs: {} draws with theirs, {} "
             "fixed-function waiting for theirs, {} left out waiting for theirs (none of their textures without "
             "them); {} built ({} failed, {} sent to the compiler thread, {:.0f} ms "
             "compiling there, at most {} KiB of its heap in use, {} allocations past it; {} from the precompiled "
             "pack; {} loaded from the card, {} of them compiled again, {:.0f} ms) in all, the warm-up's {} ({} "
             "failed, {} built or being built by their turn); uniforms sent/kept: vertex {}/{}, fragment {}/{}; "
             "textures bound/kept: {}/{}; EFB copies made on the GPU: {} ({:.0f} kpixels written, {:.1f} ms on "
             "the render worker, {:.2f} ms each), {} before the copy program was built; copy surfaces: {} made, "
             "{} taken again from {} let go ({} deleted {} frames on; {} kept now, {:.1f} MiB); the GPU busy "
             "{:.1f} ms a frame ({:.0f}% of the time); frames drawn at {}x{}; frame pacing (presents this many "
             "vblanks apart): 1 or less {}, 2 {}, 3 {}, 4 {}, 5 or more {}, swap interval {}",
             g_presentStats.presents, g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.merged) / g_presentStats.framesWithDraws : 0.0,
             g_reaper.take_destroyed_here(),
             g_presentStats.programDraws, g_presentStats.programPending, g_presentStats.pendingLeftOut,
             g_programStats.compiled,
             g_programStats.failed, g_programStats.queued, g_programStats.compileMs,
             g_compilerPeak.load(std::memory_order_relaxed) / 1024, g_compilerOverflows.load(std::memory_order_relaxed),
             g_programStats.precompiled, g_programStats.loaded, g_programStats.reloaded, g_programStats.loadMs,
             g_warmUpStats.built,
             g_warmUpStats.failed, g_warmUpStats.skipped,
             g_uniformStats.vertex.uploaded, g_uniformStats.vertex.skipped, g_uniformStats.fragment.uploaded,
             g_uniformStats.fragment.skipped, g_uniformStats.textureBinds, g_uniformStats.texturesKept,
             g_presentStats.copies, g_presentStats.copyPixels / 1000.0, g_presentStats.copyUs / 1000.0,
             g_presentStats.copies != 0 ? g_presentStats.copyUs / 1000.0 / g_presentStats.copies : 0.0,
             g_presentStats.copiesWaiting, g_surfaceStats.made, g_surfaceStats.reused, g_surfaceStats.letGo,
             g_surfaceStats.deleted, kSurfaceKeptFrames, g_keptSurfaces.size(), g_keptSurfaceBytes / 1048576.0,
             g_gpu.reportFrames != 0 ? static_cast<double>(g_gpu.reportBusyUs) / 1000.0 / g_gpu.reportFrames : 0.0,
             g_gpu.reportUs != 0 ? 100.0 * static_cast<double>(g_gpu.reportBusyUs) / static_cast<double>(g_gpu.reportUs)
                                 : 0.0,
             g_world.width, g_world.height, g_pacing[0], g_pacing[1], g_pacing[2], g_pacing[3], g_pacing[4],
             g_swapApplied);
    g_pacing = {};
    g_gpu.reportBusyUs = 0;
    g_gpu.reportUs = 0;
    g_gpu.reportFrames = 0;
    g_presentStats = {};
    g_uniformStats = {};
    g_surfaceStats = {};
    log_memory("presents");
    thread::log_cpu();
    side_jobs::log_stats();
  }
  g_clock.mark(kOther);
  if (overlay != nullptr && overlay->CmdListsCount > 0) {
    ImGui_ImplOpenGL2_RenderDrawData(overlay);
  }
  g_clock.mark(kOverlay);
  // A system keyboard that is up (open_keyboard) draws over the frame at the swap.
  gpu_watch();
  const uint32_t interval = g_swapInterval.load(std::memory_order_relaxed);
  if (interval != g_swapApplied) {
    eglSwapInterval(nullptr, static_cast<EGLint>(interval));
    g_swapApplied = interval;
    Log.info("frame pacing: each frame up {} vblank{} at least", interval, interval == 1 ? "" : "s");
  }
  const uint64_t swapStart = sceKernelGetProcessTimeWide();
  vglSwapBuffers(update_keyboard() ? GL_TRUE : GL_FALSE);
  gpu_swapped(swapStart);
  gpu_test::step();
  {
    const uint32_t vcount = sceDisplayGetVcount();
    if (g_lastVcount != 0) {
      ++g_pacing[std::clamp<uint32_t>(vcount - g_lastVcount, 1, 5) - 1];
    }
    g_lastVcount = vcount;
  }
  delete_kept_surfaces();
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
