#include "vita_gl.hpp"

#include "gfx/render_worker.hpp"
#include "internal.hpp"

#include <vitaGL.h>

#include "backends/imgui_impl_opengl2.h"

#include <cstdio>

#include <fmt/format.h>

#include <pthread.h>

#include <atomic>
#include <chrono>
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

// vitaGL's uniform locations are its uniform records' addresses negated, so any value but -1
// (not found) is one; attribute locations are plain indices.
constexpr GLint kNoUniform = -1;

// The vertex attributes a generated program may declare (gx/vita_tev.cpp), by name.
enum ProgramAttr { kAttrPos, kAttrRaw, kAttrShade, kAttrUv0, kAttrCount = kAttrUv0 + kTexCoords };
constexpr const char* kAttrNames[kAttrCount] = {"aPos", "aRaw", "aShade", "aUv0", "aUv1", "aUv2", "aUv3"};

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
  GLint mv = -1;
  GLint proj = -1;
  GLint texMtx = -1;
  GLint constants = -1;
  GLint samplers[kSamplers] = {-1, -1, -1, -1};

private:
  enum class State : uint8_t { Pending, Ready, Failed };

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
      mv = glGetUniformLocation(m_id, "uMv");
      proj = glGetUniformLocation(m_id, "uProj");
      texMtx = glGetUniformLocation(m_id, "uTexMtx");
      constants = glGetUniformLocation(m_id, "uConst");
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
  GLint m_attrs[kAttrCount] = {-1, -1, -1, -1, -1, -1, -1};
  State m_state = State::Pending;
};

std::shared_ptr<Program> make_program(std::string vertexSource, std::string fragmentSource, uint64_t key) noexcept {
  return std::make_shared<Program>(std::move(vertexSource), std::move(fragmentSource), key);
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
  // Draws with their generated program, and draws whose program was not ready (fixed-function).
  uint64_t programDraws = 0;
  uint64_t programPending = 0;
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

// A draw with its generated program (render worker). Leaving the fixed-function path turns its
// arrays off; a program switch turns on the generic arrays the new one reads.
void draw_with_program(const Draw& draw, const Program& program, GLuint& usedProgram, uint32_t& attribsOn,
                       RenderState& current) {
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
  const GLsizei count = draw.mesh->bind();
  const auto pointer = [](size_t offset) { return reinterpret_cast<const void*>(offset); };
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
  for (int coord = 0; coord < kTexCoords; ++coord) {
    if (program.attr(kAttrUv0 + coord) >= 0) {
      glVertexAttribPointer(program.attr(kAttrUv0 + coord), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            pointer(offsetof(Vertex, uv) + coord * sizeof(float[2])));
    }
  }
  const ProgramInputs& inputs = draw.program;
  if (program.mv != kNoUniform) {
    glUniform4fv(program.mv, 3, draw.modelView);
  }
  if (program.proj != kNoUniform) {
    glUniform4fv(program.proj, 4, draw.projection);
  }
  if (program.texMtx != kNoUniform) {
    glUniform4fv(program.texMtx, 3 * kTexCoords, &inputs.texMatrices[0][0]);
  }
  if (program.constants != kNoUniform) {
    glUniform4fv(program.constants, kProgramConstants, &inputs.constants[0][0]);
  }
  for (int i = 0; i < kSamplers; ++i) {
    if (inputs.textures[i] != nullptr && program.samplers[i] != kNoUniform) {
      glActiveTexture(GL_TEXTURE0 + i);
      inputs.textures[i]->bind(inputs.samplers[i]);
    }
  }
  apply_state(inputs.state, current, false);
  glDrawArrays(GL_TRIANGLES, 0, count);
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
  for (const Draw& draw : draws) {
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
    }
    if (program != nullptr) {
      ++g_presentStats.programDraws;
      draw_with_program(draw, *program, usedProgram, attribsOn, current);
      continue;
    }
    if (draw.program.program != nullptr) {
      ++g_presentStats.programPending;
    }
    enter_fixed_function();
    const GLsizei count = draw.mesh->bind();
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, pos)));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
    // Both sets point into this mesh, a unit that is off too: no pointer is left into another buffer.
    for (int unit = 0; unit < kTexUnits; ++unit) {
      glClientActiveTexture(GL_TEXTURE0 + unit);
      glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex),
                        reinterpret_cast<const void*>(offsetof(Vertex, uv) + draw.units[unit].coord * sizeof(float[2])));
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
    apply_state(draw.state, current, false);
    glDrawArrays(GL_TRIANGLES, 0, count);
  }
  enter_fixed_function();
  // Back to GL's defaults for the overlay (ImGui's backend sets blending, depth and culling
  // itself, but not the alpha test, the masks or the winding).
  apply_state(RenderState{.depthTest = false}, current, true);
  glFrontFace(GL_CCW);
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

// An orange triangle in the screen's top-left corner, drawn every frame the same way with
// identity matrices: it shows that vitaGL's drawing reaches the screen whatever the game draws.
void draw_test_triangle() {
  static std::shared_ptr<Mesh> sMesh = [] {
    std::vector<Vertex> triangle(3);
    const float corners[3][2] = {{-0.97f, 0.97f}, {-0.80f, 0.97f}, {-0.97f, 0.70f}};
    for (int i = 0; i < 3; ++i) {
      triangle[i] = Vertex{{corners[i][0], corners[i][1], 0.f}, {}, {255, 140, 0, 255}, {255, 140, 0, 255}, 255};
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
    Log.info("presents: {}, {} with game draws, {:.1f} draws a frame; generated programs: {} draws with theirs, {} "
             "fixed-function waiting for theirs; {} built ({} failed, {:.0f} ms compiling; {} loaded from the card, "
             "{:.0f} ms) in all",
             g_presentStats.presents, g_presentStats.framesWithDraws,
             g_presentStats.framesWithDraws != 0 ? double(g_presentStats.draws) / g_presentStats.framesWithDraws : 0.0,
             g_presentStats.programDraws, g_presentStats.programPending, g_programStats.compiled,
             g_programStats.failed, g_programStats.compileMs, g_programStats.loaded, g_programStats.loadMs);
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
