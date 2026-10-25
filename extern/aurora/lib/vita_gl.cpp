#include "vita_gl.hpp"
#include "vita_fifo_clock.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>

#include "backends/imgui_impl_opengl2.h"

#include <cstdio>

#include <fmt/format.h>

#include <pthread.h>

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

// vitaGL's memory by pool, free of total (MiB). The game's heap is in the [heap] lines.
void log_memory(const char* when) {
  constexpr double kMiB = 1024.0 * 1024.0;
  const auto pool = [](vglMemType type) {
    return fmt::format("{:.1f}/{:.1f}", vglMemFree(type) / kMiB, vglMemTotal(type) / kMiB);
  };
  Log.info("{}: vitaGL memory free of total (MiB): VRAM {}, RAM {}, phycont {}, budget {}; "
           "textures on the GPU: {} RGBA8 ({:.1f} MiB), {} BC1 ({:.1f} MiB)",
           when, pool(VGL_MEM_VRAM), pool(VGL_MEM_RAM), pool(VGL_MEM_PHYCONT), pool(VGL_MEM_BUDGET),
           g_textureCount[0].load(), g_textureBytes[0].load() / kMiB, g_textureCount[1].load(),
           g_textureBytes[1].load() / kMiB);
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
    m_mode(primitive == MeshPrimitive::Lines ? GL_LINES : GL_TRIANGLES) {}
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

  // Render worker: true when binding it again with `sampler` would change nothing.
  bool holds(const Sampler& sampler) const noexcept { return m_id != 0 && m_samplerSet && sampler == m_sampler; }

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

  // Render worker: fills it from the screen's `src` as the draws so far left it, on the active
  // unit (an EFB copy's texture). vitaGL's glReadPixels ends the scene and waits for the GPU, and
  // gives the rows bottom first; the texture's first row is the rectangle's top, as GX copies it.
  void copy_screen(const ScreenRect& src, CopyKind kind, bool half, std::vector<uint8_t>& readback) noexcept {
    const int width = src.width;
    const int height = src.height;
    if (width <= 0 || height <= 0) {
      return;
    }
    readback.resize(static_cast<size_t>(width) * height * 4);
    glReadPixels(src.x, src.y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
    const int step = half && width > 1 && height > 1 ? 2 : 1;
    const int outWidth = width / step;
    const int outHeight = height / step;
    m_cpu.resize(static_cast<size_t>(outWidth) * outHeight * 4);
    const size_t stride = static_cast<size_t>(width) * 4;
    for (int y = 0; y < outHeight; ++y) {
      const uint8_t* row = readback.data() + static_cast<size_t>(height - 1 - y * step) * stride;
      uint8_t* out = m_cpu.data() + static_cast<size_t>(y) * outWidth * 4;
      for (int x = 0; x < outWidth; ++x, out += 4) {
        int rgba[4];
        const uint8_t* p = row + static_cast<size_t>(x) * step * 4;
        if (step == 1) {
          for (int c = 0; c < 4; ++c) {
            rgba[c] = p[c];
          }
        } else {
          // GX's box filter: the 2x2 block's mean (the row before in memory is the next one down the screen).
          const uint8_t* q = p - stride;
          for (int c = 0; c < 4; ++c) {
            rgba[c] = (p[c] + p[c + 4] + q[c] + q[c + 4] + 2) >> 2;
          }
        }
        const auto luma = [&rgba] {
          return static_cast<uint8_t>(std::min(255, (66 * rgba[0] + 129 * rgba[1] + 25 * rgba[2] + 128) / 256 + 16));
        };
        uint8_t value;
        switch (kind) {
        case CopyKind::Rgba:
          out[0] = rgba[0], out[1] = rgba[1], out[2] = rgba[2], out[3] = rgba[3];
          continue;
        case CopyKind::IntensityAlpha:
          value = luma();
          out[0] = out[1] = out[2] = value, out[3] = rgba[3];
          continue;
        case CopyKind::RedAlpha:
          out[0] = out[1] = out[2] = rgba[0], out[3] = rgba[3];
          continue;
        case CopyKind::Intensity:
          value = luma();
          break;
        case CopyKind::Red:
          value = rgba[0];
          break;
        case CopyKind::Green:
          value = rgba[1];
          break;
        case CopyKind::Blue:
          value = rgba[2];
          break;
        case CopyKind::Alpha:
          value = rgba[3];
          break;
        default:
          out[0] = rgba[0], out[1] = rgba[1], out[2] = rgba[2], out[3] = 255;
          continue;
        }
        out[0] = out[1] = out[2] = out[3] = value;
      }
    }
    const bool fresh = m_id == 0;
    if (fresh) {
      glGenTextures(1, &m_id);
      g_textureCount[static_cast<int>(m_format)].fetch_add(1, std::memory_order_relaxed);
    }
    glBindTexture(GL_TEXTURE_2D, m_id);
    if (fresh || static_cast<uint32_t>(outWidth) != m_width || static_cast<uint32_t>(outHeight) != m_height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, outWidth, outHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_cpu.data());
      m_width = static_cast<uint32_t>(outWidth);
      m_height = static_cast<uint32_t>(outHeight);
      const auto bytes = static_cast<int64_t>(m_cpu.size());
      g_textureBytes[static_cast<int>(m_format)].fetch_add(bytes - m_bytes, std::memory_order_relaxed);
      m_bytes = bytes;
      m_samplerSet = false;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, outWidth, outHeight, GL_RGBA, GL_UNSIGNED_BYTE, m_cpu.data());
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

std::shared_ptr<Texture> make_copy_texture() noexcept {
  return std::make_shared<Texture>(0, 0, TexelFormat::RGBA8, std::vector<uint8_t>());
}

namespace {
// For the log: generated programs compiled, failed, and the time spent compiling (render worker).
struct ProgramStats {
  uint32_t compiled = 0; // built, from the compiler or the card
  uint32_t failed = 0;
  double compileMs = 0.0;
  uint32_t loaded = 0; // of them, loaded from the card
  double loadMs = 0.0;
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

void save_cached(uint64_t key, uint32_t vertexLength, uint32_t fragmentLength, GLuint program) {
  // vitaGL's glGetProgramBinary writes the whole binary whatever the buffer's size, so the buffer
  // is the length vitaGL gives first, with room to spare.
  GLint length = 0;
  glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
  if (length <= 0 || static_cast<uint32_t>(length) >= kMaxBinary) {
    return;
  }
  std::vector<uint8_t> binary(static_cast<size_t>(length) + 4096);
  GLsizei written = 0;
  GLenum format = 0;
  glGetProgramBinary(program, static_cast<GLsizei>(binary.size()), &written, &format, binary.data());
  if (written <= 0 || static_cast<size_t>(written) > binary.size()) {
    return;
  }
  // Written aside and renamed, so a file cut short (the power, a crash) is never read.
  const std::string path = cache_path(key);
  const std::string temp = path + ".tmp";
  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (file == nullptr) {
    return;
  }
  const uint32_t header[5] = {kCacheMagic, vertexLength, fragmentLength, format, static_cast<uint32_t>(written)};
  const bool ok = std::fwrite(header, sizeof(header), 1, file) == 1 &&
                  std::fwrite(binary.data(), static_cast<size_t>(written), 1, file) == 1;
  std::fclose(file);
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(temp, path, ec);
  } else {
    std::filesystem::remove(temp, ec);
  }
}
} // namespace

class Program {
public:
  Program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept
  : m_vertexSource(std::move(vertexSource)), m_fragmentSource(std::move(fragmentSource)), m_key(key) {}
  ~Program() {
    if (m_id != 0) {
      const GLuint id = m_id;
      gfx::render_worker::enqueue_work([id] { glDeleteProgram(id); });
    }
  }

  // Render worker: true once linked. A program not tried yet is built while the frame's budget
  // allows: compiles (hundreds of milliseconds) and loads from the card (a few) count apart.
  bool ready(int& compileBudget, int& loadBudget) noexcept {
    if (m_state == State::Pending) {
      if (!m_cacheChecked) {
        std::error_code ec;
        m_inCache = std::filesystem::exists(cache_path(m_key), ec);
        m_cacheChecked = true;
      }
      int& budget = m_inCache ? loadBudget : compileBudget;
      if (budget > 0) {
        --budget;
        build_on_compile_stack();
      }
    }
    return m_state == State::Ready;
  }

  GLuint id() const noexcept { return m_id; }
  GLint attr(int index) const noexcept { return m_attrs[index]; }
  GLint samplers[kSamplers] = {-1, -1, -1, -1};

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

private:
  enum class State : uint8_t { Pending, Ready, Failed };

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

  void build() noexcept {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const auto vertexLength = static_cast<uint32_t>(m_vertexSource.size());
    const auto fragmentLength = static_cast<uint32_t>(m_fragmentSource.size());
    GLenum format = 0;
    std::vector<uint8_t> binary;
    const bool cached = load_cached(m_key, vertexLength, fragmentLength, format, binary);
    if (cached) {
      // glProgramBinary rebuilds both shaders from their GXPs and links them.
      const GLuint program = glCreateProgram();
      glProgramBinary(program, format, binary.data(), static_cast<GLsizei>(binary.size()));
      GLint ok = GL_FALSE;
      glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (ok == GL_TRUE) {
        m_id = program;
      } else {
        glDeleteProgram(program);
      }
    }
    const GLuint vs = cached ? 0 : compile_cg(GL_CG_VERTEX_SHADER_EXT, m_vertexSource, error);
    const GLuint fs = vs != 0 ? compile_cg(GL_CG_FRAGMENT_SHADER_EXT, m_fragmentSource, error) : 0;
    if (vs != 0 && fs != 0) {
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
    if (vs != 0) {
      glDeleteShader(vs);
    }
    if (fs != 0) {
      glDeleteShader(fs);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (cached) {
      g_programStats.loadMs += ms;
      ++g_programStats.loaded;
      if (m_id == 0) {
        // A binary that no longer links (another vitaGL, a damaged card) goes, for a compile next time.
        std::error_code ec;
        std::filesystem::remove(cache_path(m_key), ec);
        error = "its binary from the card did not link (removed)";
      }
    } else {
      if (m_id != 0) {
        save_cached(m_key, vertexLength, fragmentLength, m_id);
      }
      g_programStats.compileMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
      static constexpr const char* kSamplerNames[kSamplers] = {"uTex0", "uTex1", "uTex2", "uTex3"};
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
  bool m_cacheChecked = false;
  bool m_inCache = false;
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
  // EFB copies made from the screen, the screen pixels they read, and their time (the GPU's wait in it).
  uint32_t copies = 0;
  uint64_t copyPixels = 0;
  uint64_t copyUs = 0;
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

// Binds a draw's mesh (render worker), counting for the log what its path and kind upload.
GLsizei bind_mesh(Mesh& mesh, DrawPath path, Section section) noexcept {
  const bool fresh = !mesh.uploaded();
  const GLsizei count = mesh.bind();
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
      // An EFB copy: the read ends vitaGL's scene, so the program in use, its uniforms and the
      // matrices are all set again at the next draw. The copy's texture is left on unit 0.
      static std::vector<uint8_t> sReadback;
      const uint64_t start = sceKernelGetProcessTimeWide();
      enter_fixed_function();
      glActiveTexture(GL_TEXTURE0);
      draw.copyTo->copy_screen(draw.scissor, draw.copyKind, draw.copyHalf, sReadback);
      bound.units[0] = nullptr;
      haveProjection = false;
      for (bool& have : haveTexMatrix) {
        have = false;
      }
      ++g_presentStats.copies;
      g_presentStats.copyPixels += static_cast<uint64_t>(draw.scissor.width) * draw.scissor.height;
      g_presentStats.copyUs += sceKernelGetProcessTimeWide() - start;
      g_clock.mark(kCopy);
      continue;
    }
    set_rects(draw.viewport, draw.scissor);
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
    glDrawArrays(draw.mesh->mode(), 0, count);
    g_clock.mark(kFixedDraw);
  }
  enter_fixed_function();
  // Back to GL's defaults for the overlay (ImGui's backend sets blending, depth and culling
  // itself, but not the alpha test, the masks or the winding).
  apply_state(RenderState{.depthTest = false}, current, true);
  glFrontFace(GL_CCW);
  glDisable(GL_SCISSOR_TEST);
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
  // vitaGL compiles the shaders behind its fixed-function calls (the overlay's) with the Vita's
  // own compiler, which takes a while; it keeps them here, in the game's folder.
  static const std::string cachePath = std::string{g_config.userPath} + "/vitagl_shader_cache";
  std::error_code ec;
  std::filesystem::create_directories(cachePath, ec);
  vglSetShaderCachePath(cachePath.c_str());
  // The generated programs' own cache (Program, load_cached).
  g_programCacheDir = std::string{g_config.userPath} + "/vita_programs";
  std::filesystem::create_directories(g_programCacheDir, ec);
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
  // The font texture is made here, on the render worker, from the atlas the game thread built;
  // the atlas only changes when the fonts do (fonts_changed).
  if (g_fontsChanged.exchange(false, std::memory_order_acq_rel)) {
    ImGui_ImplOpenGL2_DestroyFontsTexture();
  }
  ImGui_ImplOpenGL2_NewFrame();
  glViewport(0, 0, kWidth, kHeight);
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
             "EFB copies: {} ({:.0f} kpixels, {:.1f} ms, {:.2f} ms each)",
             g_presentStats.presents, g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0,
             g_presentStats.programDraws, g_presentStats.programPending, g_programStats.compiled,
             g_programStats.failed, g_programStats.compileMs, g_programStats.loaded, g_programStats.loadMs,
             g_uniformStats.vertex.uploaded, g_uniformStats.vertex.skipped, g_uniformStats.fragment.uploaded,
             g_uniformStats.fragment.skipped, g_uniformStats.textureBinds, g_uniformStats.texturesKept,
             g_presentStats.copies, g_presentStats.copyPixels / 1000.0, g_presentStats.copyUs / 1000.0,
             g_presentStats.copies != 0 ? g_presentStats.copyUs / 1000.0 / g_presentStats.copies : 0.0);
    g_presentStats = {};
    g_uniformStats = {};
    log_memory("presents");
  }
  g_clock.mark(kOther);
  if (overlay != nullptr && overlay->CmdListsCount > 0) {
    ImGui_ImplOpenGL2_RenderDrawData(overlay);
  }
  g_clock.mark(kOverlay);
  vglSwapBuffers(GL_FALSE);
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
