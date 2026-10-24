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

// The first .iso/.gcm in the data folder, when MP_DISC does not name one. Says what
// the folder holds when there is none, since nothing else on the Vita will.
std::string findDisc() {
  const std::string folder(kDataDir, std::strlen(kDataDir) - 1); // sceIoDopen without the slash
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
      found = std::string(kDataDir) + entry.d_name;
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
    // Nothing can show or answer a box here: no disc dialog or message boxes, and
    // Aurora keeps running on its Null backend instead of exiting (which a missing
    // GPU means on a desktop, and is the point of this build).
    setenv("MP_NO_DISC_DIALOG", "1", 0);
    const char* disc = std::getenv("MP_DISC");
    if (disc == nullptr || disc[0] == '\0') {
      const std::string found = findDisc();
      if (!found.empty()) {
        std::fprintf(stderr, "vita: disc %s\n", found.c_str());
        setenv("MP_DISC", found.c_str(), 1);
      } else {
        std::fprintf(stderr, "vita: no .iso or .gcm in %s\n", kDataDir);
      }
    }
  }
}

} // namespace
