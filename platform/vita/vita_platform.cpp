// PS Vita process setup, run before main: memory budgets, clocks, and the data
// folder. Everything the port reads and writes lives in ux0:data/MetroidPrime/
// (app0: is read-only): the disc image, saves, settings and the log.

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

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

bool hasDiscExtension(const char* name) {
  const size_t len = std::strlen(name);
  const auto endsWith = [&](const char* ext) {
    const size_t n = std::strlen(ext);
    return len > n && strcasecmp(name + len - n, ext) == 0;
  };
  return endsWith(".iso") || endsWith(".gcm");
}

// The first .iso/.gcm in the data folder, when MP_DISC does not name one.
std::string findDisc() {
  const SceUID dir = sceIoDopen(kDataDir);
  if (dir < 0) {
    return {};
  }
  std::string found;
  SceIoDirent entry{};
  while (found.empty() && sceIoDread(dir, &entry) > 0) {
    if (SCE_S_ISREG(entry.d_stat.st_mode) && hasDiscExtension(entry.d_name)) {
      found = std::string(kDataDir) + entry.d_name;
    }
  }
  sceIoDclose(dir);
  return found;
}

struct VitaSetup {
  VitaSetup() {
    // The highest clocks a homebrew app may set; the stock 333 MHz CPU clock is not enough.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceIoMkdir(kDataDir, 0777);
    // Nothing reads the Vita's stdout or stderr; keep them as files, line-buffered so
    // a crash leaves the last lines behind.
    const std::string dir(kDataDir);
    if (std::freopen((dir + "stdout.txt").c_str(), "w", stdout) != nullptr) {
      std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen((dir + "stderr.txt").c_str(), "w", stderr) != nullptr) {
      std::setvbuf(stderr, nullptr, _IOLBF, 0);
    }

    setenv("MP_USER_PATH", kDataDir, 0);
    // The headless build exists to time the game: log presented/throughput FPS once a
    // second, and boot straight into a new game, since the menus cannot be seen.
    setenv("MP_TRACE_TIMING", "1", 0);
    setenv("MP_FAST_BOOT", "1", 0);
    const char* disc = std::getenv("MP_DISC");
    if (disc == nullptr || disc[0] == '\0') {
      const std::string found = findDisc();
      if (!found.empty()) {
        setenv("MP_DISC", found.c_str(), 1);
      } else {
        std::fprintf(stderr, "vita: no .iso or .gcm in %s\n", kDataDir);
      }
    }
  }
};

VitaSetup s_setup;

} // namespace
