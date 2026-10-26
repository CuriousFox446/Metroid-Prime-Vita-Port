// PS Vita process setup, run before main: memory budgets, clocks, and the data
// folder. Everything the port reads and writes lives in ux0:data/MetroidPrime/
// (app0: is read-only): the disc image, saves, settings and the log. Metroid Prime
// Demo, the same executable under its own title ID, writes in ux0:data/MetroidPrimeDemo/.

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

#include <sys/lock.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>

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

// Runs before every constructor of default priority, so the game's static
// initialisers (some reach Aurora: CStopwatch reads OSGetTime) find the environment
// set and their output, a std::terminate message among it, lands in the files.
__attribute__((constructor(101))) void vitaSetup() {
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
