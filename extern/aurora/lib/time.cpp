#include <aurora/time.hpp>

#include "time_internal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

namespace aurora::time {
namespace {

native_clock::time_point default_native_now() noexcept {
#if defined(__vita__)
  // The process's own microsecond clock, steady from its start. 0.42.0 test 17's clock costs (a call,
  // on the Vita): sceKernelGetProcessTimeWide 0.70 us, steady_clock::now 5.66, and OSGetTime (this,
  // behind the game clock) 7.47. The game reads it thousands of times a frame. Every reader takes
  // differences or anchors to it, so its epoch is free to differ from steady_clock's.
  return native_clock::time_point{std::chrono::microseconds(static_cast<int64_t>(sceKernelGetProcessTimeWide()))};
#else
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return native_clock::time_point{std::chrono::duration_cast<native_clock::duration>(elapsed)};
#endif
}

std::atomic<internal::NowFunction> s_nativeNow{default_native_now};

// The game clock's state. Writers (a scale or pause change, a reset: rare) hold `writer` and step
// `sequence` around their change (odd while it is under way); readers (game_clock::now, behind every
// OSGetTime: thousands a frame, every CStopwatch among them) take no lock: they read the fields
// between two looks at the sequence and read again when a writer came between, or wait on `writer`
// while one is under way. A reader's std::shared_mutex was a pthread rwlock, which is kernel mutex
// calls on the PS Vita (vitasdk's pthreads), for every one of those calls (0.42.0 test 17).
struct ClockState {
  std::mutex writer;
  std::atomic<uint32_t> sequence{0};
  std::atomic<int64_t> nativeAnchor{default_native_now().time_since_epoch().count()};
  std::atomic<int64_t> gameAnchor{0};
  std::atomic<uint64_t> requestedScaleBits{std::bit_cast<uint64_t>(1.0)};
  std::atomic<uint32_t> pauseReasons{0};
};
static_assert(std::atomic<int64_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);

// The fields as one change left them.
struct Snapshot {
  int64_t nativeAnchor = 0;
  int64_t gameAnchor = 0;
  double requestedScale = 1.0;
  uint32_t pauseReasons = 0;
};

ClockState& state() {
  static ClockState clockState;
  return clockState;
}

native_clock::time_point get_native_now() noexcept { return s_nativeNow.load(std::memory_order_acquire)(); }

Snapshot load_fields(const ClockState& clockState) noexcept {
  return {
      .nativeAnchor = clockState.nativeAnchor.load(std::memory_order_relaxed),
      .gameAnchor = clockState.gameAnchor.load(std::memory_order_relaxed),
      .requestedScale = std::bit_cast<double>(clockState.requestedScaleBits.load(std::memory_order_relaxed)),
      .pauseReasons = clockState.pauseReasons.load(std::memory_order_relaxed),
  };
}

game_clock::time_point game_now(const Snapshot& snapshot, const native_clock::time_point nativeNow) noexcept {
  const game_clock::time_point gameAnchor{game_clock::duration{snapshot.gameAnchor}};
  if (snapshot.pauseReasons != 0 || snapshot.requestedScale == 0.0) {
    return gameAnchor;
  }

  return gameAnchor +
         std::chrono::duration_cast<game_clock::duration>(std::chrono::duration<double, std::nano>{
             (nativeNow.time_since_epoch().count() - snapshot.nativeAnchor) * snapshot.requestedScale});
}

// A consistent snapshot and the native time read with it (as the reader's lock held both before).
Snapshot read(ClockState& clockState, native_clock::time_point& nativeNow) noexcept {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const uint32_t before = clockState.sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) {
      break; // a writer is under way: wait for it below
    }
    const Snapshot snapshot = load_fields(clockState);
    nativeNow = get_native_now();
    std::atomic_thread_fence(std::memory_order_acquire);
    if (clockState.sequence.load(std::memory_order_relaxed) == before) {
      return snapshot;
    }
  }
  std::lock_guard lock{clockState.writer};
  nativeNow = get_native_now();
  return load_fields(clockState);
}

// A writer's change (`writer` held): the sequence odd from its start to its end.
void begin_write(ClockState& clockState) noexcept {
  clockState.sequence.store(clockState.sequence.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
}

void end_write(ClockState& clockState) noexcept {
  clockState.sequence.store(clockState.sequence.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void rebase_locked(ClockState& clockState, const native_clock::time_point nativeNow) noexcept {
  const Snapshot snapshot = load_fields(clockState);
  clockState.gameAnchor.store(game_now(snapshot, nativeNow).time_since_epoch().count(), std::memory_order_relaxed);
  clockState.nativeAnchor.store(nativeNow.time_since_epoch().count(), std::memory_order_relaxed);
}

} // namespace

native_clock::time_point native_clock::now() noexcept { return get_native_now(); }

game_clock::time_point game_clock::now() noexcept {
  native_clock::time_point nativeNow;
  const Snapshot snapshot = read(state(), nativeNow);
  return game_now(snapshot, nativeNow);
}

void set_scale(const float scale) noexcept {
  if (!std::isfinite(scale) || scale < 0.0f) {
    return;
  }

  auto& clockState = state();
  std::lock_guard lock{clockState.writer};
  const double clampedScale = std::min(static_cast<double>(scale), static_cast<double>(kMaximumTimeScale));
  if (std::bit_cast<double>(clockState.requestedScaleBits.load(std::memory_order_relaxed)) == clampedScale) {
    return;
  }
  begin_write(clockState);
  rebase_locked(clockState, get_native_now());
  clockState.requestedScaleBits.store(std::bit_cast<uint64_t>(clampedScale), std::memory_order_relaxed);
  end_write(clockState);
}

float scale() noexcept {
  native_clock::time_point nativeNow;
  return static_cast<float>(read(state(), nativeNow).requestedScale);
}

namespace internal {

void set_pause_reason(const PauseReason reason, const bool paused) noexcept {
  auto& clockState = state();
  std::lock_guard lock{clockState.writer};
  const auto mask = static_cast<uint32_t>(reason);
  const uint32_t reasons = clockState.pauseReasons.load(std::memory_order_relaxed);
  if (((reasons & mask) != 0) == paused) {
    return;
  }

  begin_write(clockState);
  rebase_locked(clockState, get_native_now());
  clockState.pauseReasons.store(paused ? (reasons | mask) : (reasons & ~mask), std::memory_order_relaxed);
  end_write(clockState);
}

void set_now_function(const NowFunction function) noexcept {
  s_nativeNow.store(function != nullptr ? function : default_native_now, std::memory_order_release);
  reset();
}

void reset() noexcept {
  auto& clockState = state();
  std::lock_guard lock{clockState.writer};
  begin_write(clockState);
  clockState.nativeAnchor.store(get_native_now().time_since_epoch().count(), std::memory_order_relaxed);
  clockState.gameAnchor.store(0, std::memory_order_relaxed);
  clockState.requestedScaleBits.store(std::bit_cast<uint64_t>(1.0), std::memory_order_relaxed);
  clockState.pauseReasons.store(0, std::memory_order_relaxed);
  end_write(clockState);
}

} // namespace internal
} // namespace aurora::time
