// PS Vita process setup, run before main: memory budgets, clocks, and the data
// folder. Everything the port reads and writes lives in ux0:data/MetroidPrime/
// (app0: is read-only): the disc image, saves, settings and the log. Metroid Prime
// Demo, the same executable under its own title ID, writes in ux0:data/MetroidPrimeDemo/.

#include "port_log_file.h"

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <sys/lock.h>
#include <sys/types.h>

#include <cxxabi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <malloc.h>
#include <new>
#include <string>
#include <typeinfo>

extern "C" {
// newlib's heap. Aurora's decoded textures and the game's allocations come from it.
// With the executable's ~52 MB of static data it has to fit the extended memory
// budget (ATTRIBUTE2=12 in param.sfo, CMakeLists.txt), leaving room for thread
// stacks and the system's own allocations.
int _newlib_heap_size_user = 224 * 1024 * 1024;
// The decompiled engine recurses deeply in places (script loading, collision).
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

// POSIX calls newlib leaves out that SQLite's unix VFS references (Aurora's caches).
// The Vita's filesystems have no owners and no symbolic links.
int fchown(int, uid_t, gid_t) {
  errno = ENOSYS;
  return -1;
}
ssize_t readlink(const char*, char*, size_t) {
  errno = EINVAL; // what readlink says of a path that is not a link
  return -1;
}
// fmt (built from source) locks a stream around a print; the Vita's newlib declares
// these but does not build them. Its per-stream recursive lock is the one its own
// stdio functions take (newlib's _flockfile, which is C only).
void flockfile(FILE* file) {
  if ((file->_flags & __SSTR) == 0) {
    __lock_acquire_recursive(file->_lock);
  }
}
void funlockfile(FILE* file) {
  if ((file->_flags & __SSTR) == 0) {
    __lock_release_recursive(file->_lock);
  }
}
// astcenc's aligned allocations; newlib has memalign, whose blocks free() takes.
int posix_memalign(void** out, size_t alignment, size_t size) {
  void* block = memalign(alignment, size);
  if (block == nullptr) {
    return ENOMEM;
  }
  *out = block;
  return 0;
}
}

namespace {

constexpr const char* kDataDir = "ux0:data/MetroidPrime/";
// Metroid Prime Demo: this eboot.bin packaged again under its own title ID (CMakeLists.txt's
// MP_VITA_DEMO_TITLEID), so it installs beside the game. Its saves, settings, shader cache and
// logs are its own; the disc is the one in its folder, else the game's.
constexpr const char* kDemoDataDir = "ux0:data/MetroidPrimeDemo/";

// The title ID this app was installed under (the game's or the demo's), empty if unreadable.
std::string titleId() {
  char id[256] = {};
  // 12: the running application's title ID.
  if (sceAppMgrAppParamGetString(0, 12, id, sizeof(id)) < 0) {
    return {};
  }
  return id;
}

bool hasDiscExtension(const char* name) {
  const size_t len = std::strlen(name);
  const auto endsWith = [&](const char* ext) {
    const size_t n = std::strlen(ext);
    return len > n && strcasecmp(name + len - n, ext) == 0;
  };
  return endsWith(".iso") || endsWith(".gcm");
}

// The first .iso/.gcm in a data folder, when MP_DISC does not name one. Says what
// the folder holds, since nothing else on the Vita will.
std::string findDisc(const char* dataDir) {
  const std::string folder(dataDir, std::strlen(dataDir) - 1); // sceIoDopen without the slash
  const SceUID dir = sceIoDopen(folder.c_str());
  if (dir < 0) {
    std::fprintf(stderr, "vita: cannot list %s (error 0x%08x)\n", folder.c_str(), static_cast<unsigned>(dir));
    return {};
  }
  std::string found;
  int entries = 0;
  SceIoDirent entry{};
  while (sceIoDread(dir, &entry) > 0) {
    ++entries;
    const bool isDir = SCE_S_ISDIR(entry.d_stat.st_mode);
    const bool disc = !isDir && hasDiscExtension(entry.d_name);
    std::fprintf(stderr, "vita:   %s%s  %lld bytes  mode 0x%x%s\n", entry.d_name, isDir ? "/" : "",
                 static_cast<long long>(entry.d_stat.st_size), static_cast<unsigned>(entry.d_stat.st_mode),
                 disc && found.empty() ? "  <- disc" : "");
    if (disc && found.empty()) {
      found = std::string(dataDir) + entry.d_name;
    }
  }
  sceIoDclose(dir);
  std::fprintf(stderr, "vita: %d entries in %s\n", entries, folder.c_str());
  return found;
}

// Failed allocations. When newlib's malloc has nothing for a request, operator new throws
// std::bad_alloc, which no thread here catches: a run of tallon5 (873832b) aborted that way on
// the GX FIFO thread, and neither the log nor the crash dump said what had been asked for, or by
// whom. The first few failures say so in the log first: the size, the thread, the heap, and the
// Thumb code addresses on the stack above (return addresses mostly: each, less operator new's
// address here plus its address in the ELF, is one in the ELF, for addr2line).
std::atomic<int> sFailedAllocations{0};

[[gnu::noinline]] void reportFailedAllocation(std::size_t size, const void* caller) {
  constexpr int kReports = 8;
  if (sFailedAllocations.fetch_add(1, std::memory_order_relaxed) >= kReports) {
    return;
  }
  const struct mallinfo heap = mallinfo();
  SceKernelThreadInfo thread{};
  thread.size = sizeof(thread);
  const SceUID id = sceKernelGetThreadId();
  const bool known = sceKernelGetThreadInfo(id, &thread) >= 0;
  const auto self = reinterpret_cast<uintptr_t>(static_cast<void* (*)(std::size_t)>(&::operator new));
  char line[768];
  int used = sceClibSnprintf(line, sizeof(line),
                             "vita: out of memory: operator new(%u) failed on thread 0x%08X (%s), called from 0x%08X; "
                             "heap: %u KiB in use, %u KiB free, %u KiB taken from the system of %u KiB; operator new "
                             "at 0x%08X; code addresses on the stack:",
                             static_cast<unsigned>(size), static_cast<unsigned>(id), known ? thread.name : "?",
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(caller)),
                             static_cast<unsigned>(heap.uordblks / 1024), static_cast<unsigned>(heap.fordblks / 1024),
                             static_cast<unsigned>(heap.arena / 1024),
                             static_cast<unsigned>(_newlib_heap_size_user / 1024), static_cast<unsigned>(self));
  if (known) {
    // From above this buffer up, 4 KiB at most and not past the stack's top; the code is some 16 MiB.
    const uintptr_t top = reinterpret_cast<uintptr_t>(thread.stack) + thread.stackSize;
    const uintptr_t from = (reinterpret_cast<uintptr_t>(line + sizeof(line)) + 3) & ~uintptr_t{3};
    int found = 0;
    for (uintptr_t at = from; at + 4 <= top && at < from + 4096 && found < 24; at += 4) {
      const uint32_t value = *reinterpret_cast<const volatile uint32_t*>(at);
      if ((value & 1) != 0 && value - (self - 0x01000000u) < 0x02000000u && used < static_cast<int>(sizeof(line)) - 16) {
        used += sceClibSnprintf(line + used, sizeof(line) - used, " 0x%08X", static_cast<unsigned>(value));
        ++found;
      }
    }
  }
  used = std::min(used, static_cast<int>(sizeof(line)) - 2);
  line[used++] = '\n';
  PortLogFile::WriteRaw(line, static_cast<size_t>(used));
}

void* allocate(std::size_t size, const void* caller) {
  if (size == 0) {
    size = 1;
  }
  for (;;) {
    if (void* block = std::malloc(size)) {
      return block;
    }
    if (const std::new_handler handler = std::get_new_handler()) {
      handler();
      continue;
    }
    reportFailedAllocation(size, caller);
    throw std::bad_alloc();
  }
}

// An uncaught exception. libstdc++'s own report (its verbose terminate handler) goes to stderr,
// and the abort after it stopped the thread that writes stderr's lines to the log first
// (port_log_file.cpp): what was thrown never reached the file.
[[noreturn]] void onTerminate() {
  const std::exception_ptr current = std::current_exception();
  const char* type = "";
  const char* what = "";
  if (current) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
    }
    if (const std::type_info* info = abi::__cxa_current_exception_type(); info != nullptr) {
      int status = 0;
      const char* demangled = abi::__cxa_demangle(info->name(), nullptr, nullptr, &status);
      type = demangled != nullptr ? demangled : info->name();
    }
  }
  char line[512];
  const auto id = static_cast<unsigned>(sceKernelGetThreadId());
  int used = current ? sceClibSnprintf(line, sizeof(line), "vita: std::terminate on thread 0x%08X: uncaught %s%s%s", id,
                                       type, what[0] != '\0' ? ": " : "", what)
                     : sceClibSnprintf(line, sizeof(line), "vita: std::terminate on thread 0x%08X, no exception in flight", id);
  used = std::min(used, static_cast<int>(sizeof(line)) - 2);
  line[used++] = '\n';
  PortLogFile::WriteRaw(line, static_cast<size_t>(used));
  std::abort();
}

// Runs before every constructor of default priority, so the game's static
// initialisers (some reach Aurora: CStopwatch reads OSGetTime) find the environment
// set and their output, a std::terminate message among it, lands in the files.
__attribute__((constructor(101))) void vitaSetup() {
  std::set_terminate(onTerminate);
  {
    // The highest clocks a homebrew app may set; the stock 333 MHz CPU clock is not enough.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    const std::string title = titleId();
    const bool demo = title == MP_VITA_DEMO_TITLEID;
    const char* const dataDir = demo ? kDemoDataDir : kDataDir;
    sceIoMkdir(dataDir, 0777);
    // Nothing reads the Vita's stdout or stderr; keep them as files, line-buffered so
    // a crash leaves the last lines behind.
    const std::string dir(dataDir);
    if (std::freopen((dir + "stdout.txt").c_str(), "w", stdout) != nullptr) {
      std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen((dir + "stderr.txt").c_str(), "w", stderr) != nullptr) {
      std::setvbuf(stderr, nullptr, _IOLBF, 0);
    }
    std::fprintf(stderr, "vita: title ID %s%s\n", title.empty() ? "unreadable" : title.c_str(),
                 demo ? ": Metroid Prime Demo" : "");

    setenv("MP_USER_PATH", dataDir, 0);
    // Log presented/throughput FPS once a second. The front end runs (the boot logos, the
    // title, file select), so a save can be loaded: the headless build, whose menus could not
    // be seen, booted straight into a new game instead (MP_FAST_BOOT).
    setenv("MP_TRACE_TIMING", "1", 0);
    if (demo) {
      // The demo is the frigate only. It boots straight into a new game (MP_FAST_BOOT), so no
      // save can be loaded, and the frigate's way out starts a new game again; the port menu
      // does not open (MP_DEMO, PortDebug::Demo).
      setenv("MP_FAST_BOOT", "1", 1);
      setenv("MP_DEMO", "1", 1);
    }
    // The retail world geometry's display lists made into GPU-side triangle lists once,
    // instead of re-read primitive by primitive every frame (port_retail_resident.h).
    setenv("MP_RETAIL_RESIDENT", "1", 0);
    // Aurora's staging buffers (five of them, each holding a frame's vertices, uniforms,
    // indices, arrays and texture uploads) are sized for a desktop frame that sends all its
    // geometry: about 30 MiB each, 150 MiB in all, which the null GPU backs with heap. With
    // the geometry resident a frame sends little of it, so the vertex, index and array
    // parts start at 1 MiB (mask 1|4|8; uniforms, one block a draw, keep theirs). A frame
    // that needs more spills into extra buffers. The [heap] line says what a frame used.
    setenv("AURORA_STAGING_START_KIB", "1024", 0);
    setenv("AURORA_STAGING_START_MASK", "13", 0);
    // The uniforms took at most 3.7 MiB a frame in the hangar, of 8 MiB a buffer: 4 MiB
    // saves 20 MiB over the five (and the device copy), and a bigger frame spills.
    setenv("AURORA_STAGING_START_UNIFORM_KIB", "4096", 0);
    // The PC port's own lighting (sun shadows, volumetric fog, bloom, reflection probes;
    // projected actor shadows are off in options.cpp) is FIFO work the Vita has no room
    // for, and the retail game never had it.
    // The FIFO thread finishes each frame while the game ticks the next, instead of the game
    // waiting for it at the frame's end (Aurora's AURORA_DEFERRED_FRAME_END): the tick no
    // longer adds to the FIFO thread's time. The wait that is left shows as begin-wait.
    setenv("AURORA_DEFERRED_FRAME_END", "1", 0);
    setenv("MP_SHADOWS", "0", 0);
    setenv("MP_VOLFOG", "0", 0);
    setenv("MP_BLOOM", "0", 0);
    setenv("MP_PBR_PROBE", "0", 0);
    // The zone profiler ([zones] lines): with the geometry resident its per-primitive
    // zones hardly run, so its clock reads no longer dominate; on for the per-draw
    // breakdown, at some cost to the [timing] numbers.
    setenv("MP_PROFILE", "1", 0);
    // Nothing can show or answer a box here: no disc dialog or message boxes, and
    // Aurora keeps running on its Null backend instead of exiting (which a missing
    // GPU means on a desktop, and is the point of this build).
    setenv("MP_NO_DISC_DIALOG", "1", 0);
    const char* disc = std::getenv("MP_DISC");
    if (disc == nullptr || disc[0] == '\0') {
      std::string found = findDisc(dataDir);
      if (found.empty() && demo) {
        found = findDisc(kDataDir);
      }
      if (!found.empty()) {
        std::fprintf(stderr, "vita: disc %s\n", found.c_str());
        setenv("MP_DISC", found.c_str(), 1);
      } else {
        std::fprintf(stderr, "vita: no .iso or .gcm in %s%s\n", dataDir, demo ? " or ux0:data/MetroidPrime/" : "");
      }
    }
  }
}

} // namespace

void* operator new(std::size_t size) { return allocate(size, __builtin_return_address(0)); }
void* operator new[](std::size_t size) { return allocate(size, __builtin_return_address(0)); }
// No report for these: their callers expect the failure.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return std::malloc(size == 0 ? 1 : size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return std::malloc(size == 0 ? 1 : size); }
