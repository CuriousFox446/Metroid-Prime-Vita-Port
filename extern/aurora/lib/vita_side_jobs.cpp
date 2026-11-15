#include "vita_side_jobs.hpp"

#include "internal.hpp"
#include "thread.hpp"

#include <psp2/kernel/processmgr.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace aurora::side_jobs {
namespace {
constexpr Module Log{"aurora::side_jobs"};

// Jobs queued at most; a submit past it waits for the oldest to run. The game queues one a skinned
// model drawn, a few dozen a frame.
constexpr uint32_t kSlots = 256;
// Its jobs are plain computations.
constexpr size_t kStack = 256 * 1024;
// Its priority on the fourth core: above the helpers there with it (the program compiler and the
// texture workers, 191), since the FIFO processor may be waiting for it, and below the system's own
// threads, which the core is for.
constexpr int kPriority = 128;
// The longest anyone waits for a job. The worker's take a millisecond or less, so past this it has
// hung: that is logged, and the waiter goes on without it.
constexpr uint64_t kWaitLimitUs = 500 * 1000;

struct Job {
  Fn fn = nullptr;
  void* data = nullptr;
};

struct Worker {
  std::array<Job, kSlots> ring{};
  std::mutex submitMutex; // submitters one at a time
  std::atomic<uint32_t> submitted{0};
  std::atomic<uint32_t> finished{0};
  // Those waiting on `cv` (for a job, or a submitter for a free slot). The worker takes the mutex to
  // wake them only when there are some.
  std::atomic<uint32_t> waiters{0};
  std::mutex waitMutex;
  std::condition_variable cv;
  // For the log, since its last line.
  std::atomic<uint32_t> jobs{0};
  std::atomic<uint64_t> busyUs{0};
  std::atomic<uint32_t> fifoWaits{0};
  std::atomic<uint64_t> fifoWaitUs{0};
  std::atomic<uint32_t> fifoWaitMaxUs{0};
  std::atomic<uint32_t> otherWaits{0};
  std::atomic<uint64_t> otherWaitUs{0};
  std::atomic<uint32_t> hangs{0};
  uint64_t lastLog = 0; // the render worker's (log_stats)
};

std::atomic<Worker*> sWorker{nullptr};

uint64_t now_us() noexcept { return sceKernelGetProcessTimeWide(); }

bool has_run(const Worker& w, uint32_t job) noexcept {
  // seq_cst: a waiter counts itself in `waiters` and then reads this; the worker writes this and then
  // reads `waiters`. So one of them sees the other, and no wake-up is lost.
  return static_cast<int32_t>(w.finished.load(std::memory_order_seq_cst) - job) >= 0;
}

// Waits on the worker's condition until `ready()`, for at most kWaitLimitUs: false if it gave up.
// The limit is measured here in short timed waits, whatever clock the condition variable keeps.
template <typename Ready>
bool wait_until(Worker& w, const Ready& ready) noexcept {
  std::unique_lock lock{w.waitMutex};
  w.waiters.fetch_add(1, std::memory_order_seq_cst);
  const uint64_t start = now_us();
  bool ok = true;
  while (!ready()) {
    if (now_us() - start >= kWaitLimitUs) {
      ok = false;
      break;
    }
    w.cv.wait_for(lock, std::chrono::milliseconds(2));
  }
  w.waiters.fetch_sub(1, std::memory_order_seq_cst);
  return ok;
}

void* run(void* arg) {
  Worker& w = *static_cast<Worker*>(arg);
  thread::set_current({.name = "side worker",
                       .priority = thread::Priority::High,
                       .vitaCore = thread::kVitaSystemCore,
                       .vitaPriority = kPriority});
  uint32_t next = 1;
  for (;;) {
    uint32_t submitted = w.submitted.load(std::memory_order_acquire);
    while (submitted == next - 1) {
      w.submitted.wait(submitted, std::memory_order_acquire);
      submitted = w.submitted.load(std::memory_order_acquire);
    }
    const Job job = w.ring[next % kSlots];
    const uint64_t start = now_us();
    job.fn(job.data);
    w.busyUs.fetch_add(now_us() - start, std::memory_order_relaxed);
    w.jobs.fetch_add(1, std::memory_order_relaxed);
    w.finished.store(next, std::memory_order_seq_cst);
    if (w.waiters.load(std::memory_order_seq_cst) != 0) {
      std::lock_guard lock{w.waitMutex};
      w.cv.notify_all();
    }
    ++next;
  }
  return nullptr;
}

// The worker, started at the first submit; none without the fourth core.
Worker* worker() noexcept {
  static Worker* const started = []() -> Worker* {
    if (!thread::vita_core3_available()) {
      return nullptr;
    }
    auto* w = new Worker();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStack);
    pthread_t handle;
    const int result = pthread_create(&handle, &attr, &run, w);
    pthread_attr_destroy(&attr);
    if (result != 0) {
      Log.warn("side worker: not started ({}): the game thread keeps its work", result);
      delete w;
      return nullptr;
    }
    pthread_detach(handle);
    sWorker.store(w, std::memory_order_release);
    Log.info("side worker: started on core 3 (CPU skinning; the FIFO processor waits for a model's job where "
             "its draws begin)");
    return w;
  }();
  return started;
}
} // namespace

uint32_t submit(Fn fn, void* data) noexcept {
  Worker* const w = worker();
  if (w == nullptr || fn == nullptr) {
    return 0;
  }
  std::lock_guard lock{w->submitMutex};
  const uint32_t job = w->submitted.load(std::memory_order_relaxed) + 1;
  // Its slot was the job kSlots before it's, free once that one has run. Past the wait limit the
  // worker has hung: the caller does the work itself, and the slot stays as it is.
  if (job > kSlots && !has_run(*w, job - kSlots) && !wait_until(*w, [&] { return has_run(*w, job - kSlots); })) {
    if (w->hangs.fetch_add(1, std::memory_order_relaxed) == 0) {
      Log.error("side worker: job {} has not run after {} ms; the game thread does its work itself", job - kSlots,
                kWaitLimitUs / 1000);
    }
    return 0;
  }
  w->ring[job % kSlots] = {fn, data};
  w->submitted.store(job, std::memory_order_release);
  w->submitted.notify_one();
  return job;
}

bool done(uint32_t job) noexcept {
  const Worker* const w = sWorker.load(std::memory_order_acquire);
  return job == 0 || w == nullptr || has_run(*w, job);
}

void wait(uint32_t job, bool fifo) noexcept {
  Worker* const w = sWorker.load(std::memory_order_acquire);
  if (job == 0 || w == nullptr || has_run(*w, job)) {
    return;
  }
  const uint64_t start = now_us();
  const bool ok = wait_until(*w, [&] { return has_run(*w, job); });
  const uint64_t waited = now_us() - start;
  if (fifo) {
    w->fifoWaits.fetch_add(1, std::memory_order_relaxed);
    w->fifoWaitUs.fetch_add(waited, std::memory_order_relaxed);
    const auto waitedUs = static_cast<uint32_t>(std::min<uint64_t>(waited, UINT32_MAX));
    uint32_t max = w->fifoWaitMaxUs.load(std::memory_order_relaxed);
    while (waitedUs > max && !w->fifoWaitMaxUs.compare_exchange_weak(max, waitedUs, std::memory_order_relaxed)) {
    }
  } else {
    w->otherWaits.fetch_add(1, std::memory_order_relaxed);
    w->otherWaitUs.fetch_add(waited, std::memory_order_relaxed);
  }
  if (!ok && w->hangs.fetch_add(1, std::memory_order_relaxed) == 0) {
    Log.error("side worker: job {} has not run after {} ms (the worker has run {}): going on without it", job,
              kWaitLimitUs / 1000, w->finished.load(std::memory_order_relaxed));
  }
}

void log_stats() noexcept {
  Worker* const w = sWorker.load(std::memory_order_acquire);
  if (w == nullptr) {
    return;
  }
  const uint64_t now = now_us();
  const uint64_t span = w->lastLog != 0 ? now - w->lastLog : 0;
  w->lastLog = now;
  const uint32_t jobs = w->jobs.exchange(0, std::memory_order_relaxed);
  const uint64_t busyUs = w->busyUs.exchange(0, std::memory_order_relaxed);
  const uint32_t fifoWaits = w->fifoWaits.exchange(0, std::memory_order_relaxed);
  const uint64_t fifoWaitUs = w->fifoWaitUs.exchange(0, std::memory_order_relaxed);
  const uint32_t fifoWaitMaxUs = w->fifoWaitMaxUs.exchange(0, std::memory_order_relaxed);
  const uint32_t otherWaits = w->otherWaits.exchange(0, std::memory_order_relaxed);
  const uint64_t otherWaitUs = w->otherWaitUs.exchange(0, std::memory_order_relaxed);
  const uint32_t hangs = w->hangs.exchange(0, std::memory_order_relaxed);
  if (span == 0) {
    return;
  }
  const double seconds = static_cast<double>(span) / 1e6;
  Log.info("[side jobs] over {:.1f} s: {} jobs ({:.0f} a second) ran on core 3, busy {:.1f} ms a second ({:.0f}%); "
           "the FIFO processor waited for {} of them ({:.1f} ms in all, at most {:.2f} ms), the other threads for {} "
           "({:.1f} ms); {} hung",
           seconds, jobs, jobs / seconds, static_cast<double>(busyUs) / 1000.0 / seconds,
           100.0 * static_cast<double>(busyUs) / static_cast<double>(span), fifoWaits,
           static_cast<double>(fifoWaitUs) / 1000.0, fifoWaitMaxUs / 1000.0, otherWaits,
           static_cast<double>(otherWaitUs) / 1000.0, hangs);
}
} // namespace aurora::side_jobs
