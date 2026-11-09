#include "thread.hpp"

#include <SDL3/SDL_thread.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__vita__)
#include "internal.hpp"

#include <fmt/format.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace aurora::thread {
namespace {
#if defined(__vita__)
constexpr Module Log{"aurora::thread"};

// The threads named so far (register_current), for log_cpu: each one's time run and moves between
// cores at the last log (seen: it has been logged once).
struct Named {
  std::string name;
  SceUID id = 0;
  bool seen = false;
  uint64_t run = 0;
  int32_t moves = 0;
};
std::mutex sNamedMutex;
std::vector<Named> sNamed;
uint64_t sLastLog = 0;
#endif

struct Processor {
  uint16_t group = 0;
  uint32_t number = 0;
};

struct CacheDomain {
  std::vector<Processor> processors;
  uint8_t cacheLevel = 0;
};

std::mutex sDomainMutex;
std::optional<CacheDomain> sDomain;
bool sDomainConfigured = false;

#if defined(__linux__)
bool get_current_thread_affinity(cpu_set_t& set) noexcept {
#if defined(__ANDROID__)
  return sched_getaffinity(pid_t{0}, sizeof(set), &set) == 0;
#else
  return pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

bool set_current_thread_affinity(const cpu_set_t& set) noexcept {
#if defined(__ANDROID__)
  return sched_setaffinity(pid_t{0}, sizeof(set), &set) == 0;
#else
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

std::optional<std::string> read_line(const std::string& path) {
  std::ifstream input{path};
  std::string value;
  if (!std::getline(input, value) || value.empty()) {
    return std::nullopt;
  }
  return value;
}

std::vector<uint32_t> parse_cpu_list(const std::string& list) {
  std::vector<uint32_t> cpus;
  size_t tokenStart = 0;
  while (tokenStart < list.size()) {
    const size_t tokenEnd = list.find(',', tokenStart);
    const size_t end = tokenEnd == std::string::npos ? list.size() : tokenEnd;
    const size_t dash = list.find('-', tokenStart);

    uint32_t first = 0;
    const char* firstBegin = list.data() + tokenStart;
    const char* firstEnd = list.data() + (dash < end ? dash : end);
    if (std::from_chars(firstBegin, firstEnd, first).ec != std::errc{}) {
      return {};
    }

    uint32_t last = first;
    if (dash < end) {
      const char* lastBegin = list.data() + dash + 1;
      const char* lastEnd = list.data() + end;
      if (std::from_chars(lastBegin, lastEnd, last).ec != std::errc{} || last < first) {
        return {};
      }
    }

    for (uint32_t cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
      if (cpu == UINT32_MAX) {
        break;
      }
    }
    tokenStart = end + 1;
  }
  return cpus;
}

std::optional<CacheDomain> find_cache_domain() {
  const int currentCpu = sched_getcpu();
  if (currentCpu < 0) {
    return std::nullopt;
  }

  CacheDomain best;
  const std::string cacheRoot = "/sys/devices/system/cpu/cpu" + std::to_string(currentCpu) + "/cache";
  for (uint32_t index = 0; index < 32; ++index) {
    const std::string indexRoot = cacheRoot + "/index" + std::to_string(index);
    const auto levelText = read_line(indexRoot + "/level");
    const auto type = read_line(indexRoot + "/type");
    const auto cpuList = read_line(indexRoot + "/shared_cpu_list");
    if (!levelText || !type || !cpuList || (*type != "Unified" && *type != "Data")) {
      continue;
    }

    uint32_t level = 0;
    if (std::from_chars(levelText->data(), levelText->data() + levelText->size(), level).ec != std::errc{} ||
        level <= best.cacheLevel) {
      continue;
    }

    const auto cpus = parse_cpu_list(*cpuList);
    if (std::find(cpus.begin(), cpus.end(), static_cast<uint32_t>(currentCpu)) == cpus.end()) {
      continue;
    }

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (!get_current_thread_affinity(allowed)) {
      continue;
    }

    CacheDomain candidate;
    candidate.cacheLevel = static_cast<uint8_t>(level);
    for (const uint32_t cpu : cpus) {
      if (cpu < CPU_SETSIZE && CPU_ISSET(static_cast<int>(cpu), &allowed)) {
        candidate.processors.push_back({.number = cpu});
      }
    }
    if (!candidate.processors.empty()) {
      best = std::move(candidate);
    }
  }

  if (best.processors.empty()) {
    return std::nullopt;
  }
  return best;
}

bool apply_cache_domain(const CacheDomain& domain) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const auto& processor : domain.processors) {
    if (processor.number < CPU_SETSIZE) {
      CPU_SET(static_cast<int>(processor.number), &set);
    }
  }
  return set_current_thread_affinity(set);
}
#elif defined(_WIN32)
std::optional<CacheDomain> find_cache_domain() {
  PROCESSOR_NUMBER currentProcessor{};
  GetCurrentProcessorNumberEx(&currentProcessor);

  DWORD bufferSize = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &bufferSize);
  if (bufferSize == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return std::nullopt;
  }

  std::vector<uint8_t> buffer(bufferSize);
  auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationCache, first, &bufferSize)) {
    return std::nullopt;
  }

  CacheDomain best;
  size_t offset = 0;
  while (offset < bufferSize) {
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    const auto& cache = info->Cache;
    const KAFFINITY currentMask = KAFFINITY{1} << currentProcessor.Number;
    if ((cache.Type == CacheUnified || cache.Type == CacheData) && cache.Level > best.cacheLevel &&
        cache.GroupMask.Group == currentProcessor.Group && (cache.GroupMask.Mask & currentMask) != 0) {
      GROUP_AFFINITY allowed{};
      KAFFINITY mask = cache.GroupMask.Mask;
      if (GetThreadGroupAffinity(GetCurrentThread(), &allowed) && allowed.Group == cache.GroupMask.Group) {
        mask &= allowed.Mask;
      }

      CacheDomain candidate;
      candidate.cacheLevel = cache.Level;
      constexpr uint32_t bits = sizeof(KAFFINITY) * 8;
      for (uint32_t number = 0; number < bits; ++number) {
        if ((mask & (KAFFINITY{1} << number)) != 0) {
          candidate.processors.push_back({.group = cache.GroupMask.Group, .number = number});
        }
      }
      if (!candidate.processors.empty()) {
        best = std::move(candidate);
      }
    }
    if (info->Size == 0) {
      break;
    }
    offset += info->Size;
  }

  if (best.processors.empty()) {
    return std::nullopt;
  }
  return best;
}

bool apply_cache_domain(const CacheDomain& domain) noexcept {
  GROUP_AFFINITY affinity{};
  affinity.Group = domain.processors.front().group;
  for (const auto& processor : domain.processors) {
    if (processor.group == affinity.Group && processor.number < sizeof(KAFFINITY) * 8) {
      affinity.Mask |= KAFFINITY{1} << processor.number;
    }
  }
  return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
}
#else
std::optional<CacheDomain> find_cache_domain() { return std::nullopt; }
bool apply_cache_domain(const CacheDomain&) noexcept { return false; }
#endif

void pin_shared_cache() noexcept {
  std::lock_guard lock{sDomainMutex};
  if (!sDomainConfigured) {
    auto domain = find_cache_domain();
    if (domain && apply_cache_domain(*domain)) {
      sDomain = std::move(domain);
    }
    sDomainConfigured = true;
    return;
  }
  if (sDomain) {
    apply_cache_domain(*sDomain);
  }
}

SDL_ThreadPriority to_sdl_priority(Priority priority) noexcept {
  switch (priority) {
  case Priority::Low:
    return SDL_THREAD_PRIORITY_LOW;
  case Priority::Normal:
    return SDL_THREAD_PRIORITY_NORMAL;
  case Priority::High:
    return SDL_THREAD_PRIORITY_HIGH;
  }
  return SDL_THREAD_PRIORITY_NORMAL;
}

void set_thread_name(const std::string& name) noexcept {
#if defined(_WIN32)
  const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
  if (length <= 0) {
    return;
  }
  std::wstring wideName;
  wideName.resize(static_cast<size_t>(length));
  MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wideName.data(), length);
  SetThreadDescription(GetCurrentThread(), wideName.c_str());
#elif defined(__APPLE__)
  const std::string truncated = name.substr(0, 63);
  pthread_setname_np(truncated.c_str());
#elif defined(__linux__)
  const std::string truncated = name.substr(0, 15);
  pthread_setname_np(pthread_self(), truncated.c_str());
#endif
}
} // namespace

void set_current(const Options& options) noexcept {
  if (!options.name.empty()) {
    set_thread_name(options.name);
#ifdef TRACY_ENABLE
    tracy::SetThreadName(options.name.c_str());
#endif
    register_current(options.name);
  }

  SDL_SetCurrentThreadPriority(to_sdl_priority(options.priority));
  if (options.affinity == Affinity::SharedCache) {
    pin_shared_cache();
  }
#if defined(__vita__)
  // The Vita's scheduler moves a thread between cores as it likes, and runs one till it waits or
  // one of a higher priority is ready, whatever else of its priority waits. In 0.42.0 test 7's heavy
  // room the FIFO processor was busy 97% of each frame by its own clock but ran 86%: the frame reaper
  // (its priority, 6%), the DVD worker (above it, 4%) and a texture worker had last run on its core.
  // So the three busy threads each keep a core (game 1, FIFO 0, render worker 2) and the helpers that
  // can take one from them share the render worker's, which is busy about a third of the time.
  const SceUID self = sceKernelGetThreadId();
  if (options.vitaCore >= 0 && options.vitaCore <= 2) {
    const int result = sceKernelChangeThreadCpuAffinityMask(self, SCE_KERNEL_CPU_MASK_USER_0 << options.vitaCore);
    if (result < 0) {
      Log.warn("{}: could not keep to core {} ({:#x})", options.name, options.vitaCore, static_cast<uint32_t>(result));
    }
  }
  if (options.vitaPriority != 0) {
    const int result = sceKernelChangeThreadPriority(self, options.vitaPriority);
    if (result < 0) {
      Log.warn("{}: could not take priority {} ({:#x})", options.name, options.vitaPriority,
               static_cast<uint32_t>(result));
    }
  }
#endif
}

void register_current(const std::string& name) noexcept {
#if defined(__vita__)
  const SceUID id = sceKernelGetThreadId();
  std::lock_guard lock{sNamedMutex};
  for (Named& named : sNamed) {
    if (named.id == id) {
      named.name = name;
      return;
    }
  }
  sNamed.push_back({.name = name, .id = id});
#else
  (void)name;
#endif
}

void log_cpu() noexcept {
#if defined(__vita__)
  std::lock_guard lock{sNamedMutex};
  const uint64_t now = sceKernelGetProcessTimeWide();
  const uint64_t span = sLastLog != 0 ? now - sLastLog : 0;
  sLastLog = now;
  std::string threads;
  for (auto it = sNamed.begin(); it != sNamed.end();) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(it->id, &info) < 0) {
      it = sNamed.erase(it); // it has ended (a job thread with no work left)
      continue;
    }
    // Its run time and the span are both microseconds.
    const uint64_t run = info.runClocks;
    const std::string busy = it->seen && span != 0
                                 ? fmt::format("{:.0f}%", 100.0 * static_cast<double>(run - it->run) / static_cast<double>(span))
                                 : std::string("new");
    // Its ID too: a crash dump names threads by theirs.
    threads += fmt::format("{}{} ({:#x}): core {} (last {}, mask {:#x}), priority {}, {} busy, {} moves",
                           threads.empty() ? "" : " | ", it->name, static_cast<uint32_t>(it->id), info.currentCpuId,
                           info.lastExecutedCpuId,
                           static_cast<uint32_t>(info.currentCpuAffinityMask), info.currentPriority, busy,
                           it->seen ? info.changeCpuCount - it->moves : info.changeCpuCount);
    it->seen = true;
    it->run = run;
    it->moves = info.changeCpuCount;
    ++it;
  }
  Log.info("[threads] over {:.1f} s: {}", static_cast<double>(span) / 1e6, threads);
#endif
}

} // namespace aurora::thread
