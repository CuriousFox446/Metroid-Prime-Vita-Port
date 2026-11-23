#include "vita_side_jobs.hpp"

#include "internal.hpp"
#include "thread.hpp"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

// The side worker's stack, for the game's per-thread statics (platform/include/port_thread_slot.h):
// a stack pointer in [low, low + span) is the side worker's. Written by it before its first job.
extern "C" {
uintptr_t gPortSideStackLow = 0;
uintptr_t gPortSideStackSpan = 0;
// The second side worker's (0.46.0 test 16): per-thread slot 2.
uintptr_t gPortSide2StackLow = 0;
uintptr_t gPortSide2StackSpan = 0;
// The game thread's stack below the wait where it runs a side job (0.46.0 test 18): per-thread slot
// 3 while the job runs (run_stolen), the span 0 the rest of the time. The low end is the stack's
// bottom, from the kernel, set before the first such job.
uintptr_t gPortGameJobStackLow = 0;
uintptr_t gPortGameJobStackSpan = 0;
}

namespace aurora::side_jobs {
namespace {
constexpr Module Log{"aurora::side_jobs"};

// Jobs queued at most; a submit past it waits for the oldest to run. The game queues one a skinned
// model drawn, a few dozen a frame.
constexpr uint32_t kSlots = 256;
// Its jobs are plain computations.
constexpr size_t kStack = 256 * 1024;
// The side worker's priority on the fourth core: first there, since the FIFO processor and the game
// thread wait for its short jobs (0.42.0 test 22 put MusyX's mixer above it, and those waits grew
// tenfold); then the mixer (129: platform/vita/vita_platform.cpp), then the background lane, then the
// helpers (the program compiler and the texture workers, 191). The system's own threads, which the
// core is for, stay above them all.
constexpr int kPriority = 128;
// The background lane's (0.42.0 test 23): long jobs nobody waits for soon (a movie frame's decode,
// 15 to 40 ms), below the mixer, which must not miss the audio device's buffer for one of them.
constexpr int kBackgroundPriority = 130;
// The core 0 helper's (0.46.0 test 4): above the render worker there (160, a third of the core in
// the fights), whose present the FIFO processor's frame feeds; below the DVD worker (112).
constexpr int kHelperPriority = 158;
constexpr int kHelperCore = 0;
// The second side worker's (0.46.0 test 16): on the first core, above the render worker. In test 15's
// Triclops room the game thread waited for the side worker's particle steps 2 s in 17 (7 ms a frame)
// while the fourth core, the mixer's too, ran them one by one, and the first and third cores were
// half idle. The steps are each object's own, in no order, so either worker takes one (submit_any).
// Test 16 had it below the helper (159), and in Phendrana's Sheegoth rooms the game thread waited for
// it 5 to 7 ms at a time (1.9 s in 21, against 0.2 s for the fourth core's): the helper's bursts for
// the FIFO processor held it off its core. Since test 17 it is above the helper: the game thread waits
// for its jobs within the tick, and the FIFO processor runs the helper's unclaimed jobs itself.
constexpr int kSecondPriority = 157;
constexpr int kSecondCore = 0;
// A second worker's job number: its own, with this bit (job numbers stay far below it).
constexpr uint32_t kSecondTag = 1u << 31;
// The longest anyone waits for a job. The worker's take a millisecond or less, so past this it has
// hung: that is logged, and the waiter goes on without it.
constexpr uint64_t kWaitLimitUs = 500 * 1000;
// The worker's sleep with nothing to run is timed too: a wake-up it misses, whatever loses it,
// costs at most this.
constexpr auto kSleepSlice = std::chrono::milliseconds(4);

struct Job {
  Fn fn = nullptr;
  void* data = nullptr;
  bool ordered = false;   // runs after every job before it has (the helper's, where two threads run them)
  bool stealable = false; // submit_any's: the game thread may run it while it waits (wait_game)
};

struct Worker {
  const char* name = "side worker";
  int priority = kPriority;
  int core = thread::kVitaSystemCore;
  bool sideStack = false; // exports its stack for the per-thread statics (the side workers only)
  int stackSlot = 1;      // which per-thread slot that stack is (port_thread_slot.h): 1 or 2
  std::array<Job, kSlots> ring{};
  std::mutex submitMutex; // submitters one at a time
  std::atomic<uint32_t> submitted{0};
  std::atomic<uint32_t> finished{0};
  // The core 0 helper's (0.46.0 test 12): its jobs are claimed one by one, by it or by the FIFO
  // processor while that waits (wait_helper), so they may end out of order: each slot keeps the
  // number of the last job run in it, and `finished` moves on over those that have run in a row.
  // The side workers' too since 0.46.0 test 17, whose stealable jobs the game thread claims while it
  // waits (wait_game).
  bool claiming = false;
  std::atomic<uint32_t> claimed{0};
  std::array<std::atomic<uint32_t>, kSlots> doneSeq{};
  std::atomic<uint32_t> stolen{0};
  std::atomic<uint64_t> stolenUs{0};
  // The worker sleeps on these while it has nothing to run, and a submitter wakes it through them
  // when it is asleep. Not std::atomic's wait on `submitted`: without a futex (newlib), libstdc++
  // parks every atomic's waiters on one of 16 condition variables picked by address. In 0.46.0
  // test 1 the worker woke 17 to 40 times a job there, against 1.5 in 0.42.0's tests, and slept
  // through jobs the game thread waited for (205 past 500 ms in three minutes, none before).
  std::mutex sleepMutex;
  std::condition_variable sleepCv;
  std::atomic<bool> sleeping{false};
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
std::atomic<Worker*> sBackground{nullptr};
std::atomic<Worker*> sHelper{nullptr};
std::atomic<Worker*> sSecond{nullptr};

uint64_t now_us() noexcept { return sceKernelGetProcessTimeWide(); }

bool has_run(const Worker& w, uint32_t job) noexcept {
  // seq_cst: a waiter counts itself in `waiters` and then reads this; the worker writes this and then
  // reads `waiters`. So one of them sees the other, and no wake-up is lost.
  return static_cast<int32_t>(w.finished.load(std::memory_order_seq_cst) - job) >= 0;
}

// Whether this one job has run (0.46.0 test 18), where has_run asks for every job up to it. A claiming
// worker's jobs may end out of order, and the side workers' waiters need only their own: the game
// thread's for a step need not wait on a step another thread runs ahead of it. A slot's `doneSeq` keeps
// its job's number from its end until the slot's next job ends, which is after has_run(job). The
// helper's waits keep has_run (its `ordered` jobs and the FIFO processor's loop rely on it), and so
// does a submit's wait for a free slot (`finished` must have passed it).
bool ran(const Worker& w, uint32_t job) noexcept {
  return has_run(w, job) || (w.claiming && w.doneSeq[job % kSlots].load(std::memory_order_seq_cst) == job);
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

// A claimed job run, on whichever thread claimed it, and its end counted: `finished` moves on over
// every job that has run in a row from it.
void run_claimed(Worker& w, uint32_t job) noexcept {
  const Job j = w.ring[job % kSlots];
  if (j.ordered) {
    // After every job before it: the one the other thread runs, if any, is short.
    while (!has_run(w, job - 1)) {
      sceKernelDelayThread(20);
    }
  }
  j.fn(j.data);
  // All seq_cst: two threads ending neighbouring jobs at once each write their slot and then read
  // `finished` and the other's slot. With weaker orders both could read the other's old value, and
  // `finished` would stop short of the later job until a later job ended: at the frame's end there
  // is none, and its wait would never end. In one total order, one of them sees the other.
  w.doneSeq[job % kSlots].store(job, std::memory_order_seq_cst);
  uint32_t f = w.finished.load(std::memory_order_seq_cst);
  while (w.doneSeq[(f + 1) % kSlots].load(std::memory_order_seq_cst) == f + 1) {
    // seq_cst as below (has_run, wait_until): a waiter that counted itself sees this or is woken.
    if (w.finished.compare_exchange_weak(f, f + 1, std::memory_order_seq_cst)) {
      ++f;
    }
  }
  if (w.waiters.load(std::memory_order_seq_cst) != 0) {
    std::lock_guard lock{w.waitMutex};
    w.cv.notify_all();
  }
}

// The helper's loop: as run's, each job claimed first.
void run_claiming(Worker& w) {
  for (;;) {
    uint32_t c = w.claimed.load(std::memory_order_seq_cst);
    if (c == w.submitted.load(std::memory_order_seq_cst)) {
      std::unique_lock lock{w.sleepMutex};
      w.sleeping.store(true, std::memory_order_seq_cst);
      while (w.claimed.load(std::memory_order_seq_cst) == w.submitted.load(std::memory_order_seq_cst)) {
        w.sleepCv.wait_for(lock, kSleepSlice);
      }
      w.sleeping.store(false, std::memory_order_relaxed);
      continue;
    }
    if (!w.claimed.compare_exchange_weak(c, c + 1, std::memory_order_seq_cst)) {
      continue;
    }
    const uint64_t start = now_us();
    run_claimed(w, c + 1);
    w.busyUs.fetch_add(now_us() - start, std::memory_order_relaxed);
    w.jobs.fetch_add(1, std::memory_order_relaxed);
  }
}

void* run(void* arg) {
  Worker& w = *static_cast<Worker*>(arg);
  if (w.sideStack) {
    // Every frame the jobs make lies below this one, in the stack's kStack bytes from about here
    // down: the range leaves out its last 16 KiB, which more than covers what started the thread.
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    (w.stackSlot == 2 ? gPortSide2StackLow : gPortSideStackLow) = sp - (kStack - 16 * 1024);
    (w.stackSlot == 2 ? gPortSide2StackSpan : gPortSideStackSpan) = kStack - 16 * 1024;
  }
  thread::set_current({.name = w.name,
                       .priority = thread::Priority::High,
                       .vitaCore = w.core,
                       .vitaPriority = w.priority});
  if (w.claiming) {
    run_claiming(w);
    return nullptr;
  }
  uint32_t next = 1;
  for (;;) {
    if (w.submitted.load(std::memory_order_acquire) == next - 1) {
      // seq_cst: the worker marks itself asleep and then reads `submitted`; a submitter writes it and
      // then reads `sleeping`. So one of them sees the other: either the worker finds the job, or the
      // submitter wakes it, through the mutex (it cannot slip in between the check and the wait).
      std::unique_lock lock{w.sleepMutex};
      w.sleeping.store(true, std::memory_order_seq_cst);
      while (w.submitted.load(std::memory_order_seq_cst) == next - 1) {
        w.sleepCv.wait_for(lock, kSleepSlice);
      }
      w.sleeping.store(false, std::memory_order_relaxed);
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

// A worker thread on `core`; none on the fourth core without it open.
Worker* start_worker(const char* name, int priority, int core, bool sideStack, std::atomic<Worker*>& slot,
                     const char* what, bool claiming = false, int stackSlot = 1) noexcept {
  if (core == thread::kVitaSystemCore && !thread::vita_core3_available()) {
    return nullptr;
  }
  auto* w = new Worker();
  w->name = name;
  w->priority = priority;
  w->core = core;
  w->sideStack = sideStack;
  w->stackSlot = stackSlot;
  w->claiming = claiming;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kStack);
  pthread_t handle;
  const int result = pthread_create(&handle, &attr, &run, w);
  pthread_attr_destroy(&attr);
  if (result != 0) {
    Log.warn("{}: not started ({}): the game thread keeps its work", name, result);
    delete w;
    return nullptr;
  }
  pthread_detach(handle);
  slot.store(w, std::memory_order_release);
  Log.info("{}: started on core {} at priority {} ({})", name, core, priority, what);
  return w;
}

// Each started at its first submit.
Worker* side_worker() noexcept {
  static Worker* const started =
      start_worker("side worker", kPriority, thread::kVitaSystemCore, true, sWorker,
                   "CPU skinning, which the FIFO processor waits for where a model's draws begin; the actors' "
                   "and the projectiles' particle steps, which the game thread finishes later in the tick, "
                   "running those still untaken itself then",
                   true);
  return started;
}

Worker* second_worker() noexcept {
  static Worker* const started =
      start_worker("side worker 2", kSecondPriority, kSecondCore, true, sSecond,
                   "particle steps beside the fourth core's side worker, whichever has fewer queued", true, 2);
  return started;
}

Worker* background_worker() noexcept {
  static Worker* const started = start_worker("side worker (background lane)", kBackgroundPriority,
                                              thread::kVitaSystemCore, false, sBackground, "the movies' video decode");
  return started;
}

Worker* helper_worker() noexcept {
  static Worker* const started =
      start_worker("core 0 helper", kHelperPriority, kHelperCore, false, sHelper,
                   "the FIFO processor's skinned calls' vertices and long immediate draws, which its frame's end "
                   "waits for, running those still unclaimed itself then",
                   true);
  return started;
}

uint32_t submit_to(Worker* w, Fn fn, void* data, bool ordered = false, bool stealable = false) noexcept {
  if (w == nullptr || fn == nullptr) {
    return 0;
  }
  std::lock_guard lock{w->submitMutex};
  const uint32_t job = w->submitted.load(std::memory_order_relaxed) + 1;
  // Its slot was the job kSlots before it's, free once that one has run. Past the wait limit the
  // worker has hung: the caller does the work itself, and the slot stays as it is.
  if (job > kSlots && !has_run(*w, job - kSlots) && !wait_until(*w, [&] { return has_run(*w, job - kSlots); })) {
    if (w->hangs.fetch_add(1, std::memory_order_relaxed) == 0) {
      Log.error("{}: job {} has not run after {} ms; the game thread does its work itself", w->name, job - kSlots,
                kWaitLimitUs / 1000);
    }
    return 0;
  }
  w->ring[job % kSlots] = {fn, data, ordered, stealable};
  w->submitted.store(job, std::memory_order_seq_cst);
  if (w->sleeping.load(std::memory_order_seq_cst)) {
    // Through the mutex: a worker between its check and its wait holds it, so this waits for the
    // wait to begin, and the notify reaches it.
    { std::lock_guard sleep{w->sleepMutex}; }
    w->sleepCv.notify_one();
  }
  return job;
}

bool done_in(const Worker* w, uint32_t job) noexcept { return job == 0 || w == nullptr || has_run(*w, job); }

// `alone`: done once the job itself has run (ran), not every job before it (the side workers').
void wait_in(Worker* w, uint32_t job, bool fifo, bool alone = false) noexcept {
  const auto over = [&] { return alone ? ran(*w, job) : has_run(*w, job); };
  if (job == 0 || w == nullptr || over()) {
    return;
  }
  const uint64_t start = now_us();
  const bool ok = wait_until(*w, over);
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
    Log.error("{}: job {} has not run after {} ms (the worker has run {}): going on without it", w->name, job,
              kWaitLimitUs / 1000, w->finished.load(std::memory_order_relaxed));
  }
}

// The game thread: the thread of the first submit_any (the particle steps, which only it queues).
std::atomic<SceUID> sGameThread{0};
// The game thread's own: it is running a job in wait_game, whose own waits only wait.
bool sGameStealing = false;

// The game thread's stack bottom, for slot 3 (gPortGameJobStackLow), asked of the kernel at its first
// wait. Without it the game thread runs no side job: it waits, as before test 17.
bool game_stack_known() noexcept {
  static const bool known = [] {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    const auto low = reinterpret_cast<uintptr_t>(info.stack);
    if (result < 0 || low == 0 || sp - low >= static_cast<uintptr_t>(info.stackSize)) {
      Log.warn("the game thread's stack is not known ({:#x}): it waits for its side jobs without running any",
               static_cast<uint32_t>(result));
      return false;
    }
    gPortGameJobStackLow = low;
    Log.info("the game thread runs the particle steps no side worker has taken when it waits for them, in "
             "per-thread slot 3 (its stack below the wait: {:#x} to {:#x})",
             low, sp);
    return true;
  }();
  return known;
}

// A job the game thread runs in its wait: what it calls lies below here on the game thread's stack,
// per-thread slot 3 (port_thread_slot.h) while it runs. The compiler barriers keep the span's writes
// on either side of the job, whose code reads it (the game thread alone reads its own range).
void run_stolen(Worker& w, uint32_t job) noexcept {
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  gPortGameJobStackSpan = sp - gPortGameJobStackLow;
  __asm__ volatile("" ::: "memory");
  run_claimed(w, job);
  __asm__ volatile("" ::: "memory");
  gPortGameJobStackSpan = 0;
}

// Job c + 1 of `w` claimed and run here (run_stolen), counted as the worker's stolen ones.
void run_stolen_counted(Worker& w, uint32_t job, uint64_t& ranUs) noexcept {
  const uint64_t ranStart = now_us();
  run_stolen(w, job);
  const uint64_t took = now_us() - ranStart;
  ranUs += took;
  w.stolen.fetch_add(1, std::memory_order_relaxed);
  w.stolenUs.fetch_add(took, std::memory_order_relaxed);
}

// Whether `w`'s next job (the first no thread has claimed) is queued and one the game thread may run.
// The slot read is the job's: it is not reused before the job has run, which needs it claimed first,
// and a claim made since this read fails the caller's compare-exchange.
bool can_steal(const Worker& w) noexcept {
  const uint32_t c = w.claimed.load(std::memory_order_seq_cst);
  return c != w.submitted.load(std::memory_order_seq_cst) && w.ring[(c + 1) % kSlots].stealable;
}

// That next job claimed and run here when it is stealable: true when one ran, or a worker claimed it
// first (worth looking again at once), false when there was none to take.
bool steal_next(Worker& w, uint64_t& ranUs) noexcept {
  uint32_t c = w.claimed.load(std::memory_order_seq_cst);
  if (c == w.submitted.load(std::memory_order_seq_cst) || !w.ring[(c + 1) % kSlots].stealable) {
    return false;
  }
  if (w.claimed.compare_exchange_weak(c, c + 1, std::memory_order_seq_cst)) {
    run_stolen_counted(w, c + 1, ranUs);
  }
  return true;
}

// The other side worker of the two (the one `w` is not), or none.
Worker* other_side_worker(const Worker& w) noexcept {
  Worker* const first = sWorker.load(std::memory_order_acquire);
  Worker* const second = sSecond.load(std::memory_order_acquire);
  Worker* const other = &w == first ? second : &w == second ? first : nullptr;
  return other != nullptr && other->claiming ? other : nullptr;
}

// The game thread's wait for a side worker's job (0.46.0 test 17): the stealable jobs up to it that
// no worker has taken yet run here, in their order, as the FIFO processor does with the helper's
// (wait_helper). In test 16's Sheegoth rooms the game thread waited for the particle steps 11 ms a
// frame, 5 to 7 ms at a time, for jobs the second worker had not begun. A job the worker has taken
// it waits for, and one of the worker's own kind (a skinned model's vertices, submit's) it leaves to
// the worker: it waits till that is taken, then goes on. Since test 18 the jobs it runs have slot 3
// (run_stolen): in test 17 they ran in the game thread's slot 0, where a projectile's step, taking
// itself for the game thread, waited for itself (CProjectileWeapon::PortFinishStep), and the game
// froze. And it waits for its job alone (ran), not for those still running ahead of it.
// Since test 55, while its job runs on a worker (or waits behind one of the worker's own kind), it
// runs the next untaken steps of either worker, those queued after its job too. In test 53's ending
// (MLVL 13D79165) it waited 12 ms a frame for 77 of the 214 effects' steps, every one already taken:
// it waits for them in the order they began, and the workers take them in that order, so it was
// always behind them and ran 9 a frame. The steps are each object's own, in no order (two workers
// already run them side by side), and each is needed within the tick, which these waits end.
void wait_game(Worker& w, uint32_t job) noexcept {
  const uint64_t start = now_us();
  uint64_t ranUs = 0;
  bool waited = false;
  bool ok = true;
  sGameStealing = true;
  Worker* const other = other_side_worker(w);
  while (!ran(w, job)) {
    uint32_t c = w.claimed.load(std::memory_order_seq_cst);
    if (static_cast<int32_t>(c - job) < 0) {
      // Job c + 1 was queued before `job` was: its slot holds it till it has run.
      if (w.ring[(c + 1) % kSlots].stealable) {
        if (w.claimed.compare_exchange_weak(c, c + 1, std::memory_order_seq_cst)) {
          run_stolen_counted(w, c + 1, ranUs);
        }
        continue;
      }
    } else if (steal_next(w, ranUs)) {
      // `job` runs on the worker: the steps queued after it meanwhile.
      continue;
    }
    if (other != nullptr && steal_next(*other, ranUs)) {
      continue;
    }
    waited = true;
    // Till `job` has run, the worker claims another (one of its own kind taken, or one to run here),
    // or the other worker has one to run here.
    ok = wait_until(w, [&] {
      return ran(w, job) || w.claimed.load(std::memory_order_seq_cst) != c || (other != nullptr && can_steal(*other));
    });
    if (!ok) {
      break;
    }
  }
  sGameStealing = false;
  if (waited) {
    const uint64_t spent = now_us() - start;
    w.otherWaits.fetch_add(1, std::memory_order_relaxed);
    w.otherWaitUs.fetch_add(spent > ranUs ? spent - ranUs : 0, std::memory_order_relaxed);
  }
  if (!ok && w.hangs.fetch_add(1, std::memory_order_relaxed) == 0) {
    Log.error("{}: job {} has not run after {} ms (the worker has run {}): going on without it", w.name, job,
              kWaitLimitUs / 1000, w.finished.load(std::memory_order_relaxed));
  }
}

void log_worker(Worker* w, const char* lane) noexcept {
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
  const uint32_t stolen = w->stolen.exchange(0, std::memory_order_relaxed);
  const uint64_t stolenUs = w->stolenUs.exchange(0, std::memory_order_relaxed);
  if (span == 0) {
    return;
  }
  const double seconds = static_cast<double>(span) / 1e6;
  // Who runs a claiming worker's jobs while waiting: the FIFO processor the helper's, the game thread
  // the side workers' (0.46.0 test 17; its waits then count only the time it did not run any).
  const bool gameRuns = w->claiming && w->sideStack;
  const std::string fifoRan =
      w->claiming && !gameRuns ? fmt::format(", and ran {} of them itself while it waited ({:.1f} ms in all)", stolen,
                                             static_cast<double>(stolenUs) / 1000.0)
                               : std::string{};
  const std::string gameRan =
      gameRuns ? fmt::format(", and the game thread ran {} of them itself while it waited ({:.1f} ms in all)", stolen,
                             static_cast<double>(stolenUs) / 1000.0)
               : std::string{};
  Log.info("[side jobs]{} over {:.1f} s: {} jobs ({:.0f} a second) ran on core {}, busy {:.1f} ms a second ({:.0f}%); "
           "the FIFO processor waited for {} of them ({:.1f} ms in all, at most {:.2f} ms){}, the other threads for {} "
           "({:.1f} ms){}; {} hung",
           lane, seconds, jobs, jobs / seconds, w->core, static_cast<double>(busyUs) / 1000.0 / seconds,
           100.0 * static_cast<double>(busyUs) / static_cast<double>(span), fifoWaits,
           static_cast<double>(fifoWaitUs) / 1000.0, fifoWaitMaxUs / 1000.0, fifoRan, otherWaits,
           static_cast<double>(otherWaitUs) / 1000.0, gameRan, hangs);
}
} // namespace

uint32_t submit(Fn fn, void* data) noexcept { return submit_to(side_worker(), fn, data); }

uint32_t submit_any(Fn fn, void* data) noexcept {
  if (sGameThread.load(std::memory_order_relaxed) == 0) {
    sGameThread.store(sceKernelGetThreadId(), std::memory_order_relaxed);
  }
  Worker* const first = side_worker();
  Worker* const second = second_worker();
  const auto queued = [](const Worker* w) {
    return w->submitted.load(std::memory_order_relaxed) - w->finished.load(std::memory_order_relaxed);
  };
  // The one with fewer jobs queued; the fourth core's on a tie (or alone).
  if (second != nullptr && (first == nullptr || queued(second) < queued(first))) {
    const uint32_t job = submit_to(second, fn, data, false, true);
    if (job != 0) {
      return job | kSecondTag;
    }
  }
  return submit_to(first, fn, data, false, true);
}

bool done(uint32_t job) noexcept {
  Worker* const w = (job & kSecondTag) != 0 ? sSecond.load(std::memory_order_acquire)
                                             : sWorker.load(std::memory_order_acquire);
  const uint32_t own = job & ~kSecondTag;
  return own == 0 || w == nullptr || ran(*w, own);
}

void wait(uint32_t job, bool fifo) noexcept {
  Worker* const w = (job & kSecondTag) != 0 ? sSecond.load(std::memory_order_acquire)
                                             : sWorker.load(std::memory_order_acquire);
  const uint32_t own = job & ~kSecondTag;
  // The game thread runs what it can (wait_game); not inside a job it runs there, nor any other thread
  // (the FIFO processor: slot 3 is the game thread's stack).
  if (!fifo && w != nullptr && w->claiming && own != 0 &&
      sceKernelGetThreadId() == sGameThread.load(std::memory_order_relaxed) && !sGameStealing &&
      game_stack_known()) {
    if (!ran(*w, own)) {
      wait_game(*w, own);
    }
    return;
  }
  wait_in(w, own, fifo, true);
}

uint32_t submit_background(Fn fn, void* data) noexcept { return submit_to(background_worker(), fn, data); }

bool done_background(uint32_t job) noexcept { return done_in(sBackground.load(std::memory_order_acquire), job); }

void wait_background(uint32_t job) noexcept { wait_in(sBackground.load(std::memory_order_acquire), job, false); }

uint32_t submit_helper(Fn fn, void* data, bool ordered) noexcept {
  return submit_to(helper_worker(), fn, data, ordered);
}

bool done_helper(uint32_t job) noexcept { return done_in(sHelper.load(std::memory_order_acquire), job); }

void wait_helper(uint32_t job) noexcept {
  Worker* const w = sHelper.load(std::memory_order_acquire);
  // 0.46.0 test 12: the jobs up to it the helper has not claimed yet run here first. Core 0 was
  // full in the fights (the render worker a third of it, the helper up to 56%, the DVD worker above
  // both), and the FIFO processor waited for the helper for up to half its time on the third core
  // (test 11's Thardus fight: 5.9 s in 12.3 s, 20 ms a frame at its end).
  while (w != nullptr && !done_in(w, job)) {
    uint32_t c = w->claimed.load(std::memory_order_seq_cst);
    if (static_cast<int32_t>(c - job) < 0) {
      if (w->claimed.compare_exchange_weak(c, c + 1, std::memory_order_seq_cst)) {
        const uint64_t start = now_us();
        run_claimed(*w, c + 1);
        w->stolen.fetch_add(1, std::memory_order_relaxed);
        w->stolenUs.fetch_add(now_us() - start, std::memory_order_relaxed);
      }
      continue;
    }
    // The rest are the helper's: past the limit (logged) it waits again, as the job writes into the
    // frame being recorded.
    wait_in(w, job, true);
  }
}

void log_stats() noexcept {
  log_worker(sWorker.load(std::memory_order_acquire), "");
  log_worker(sBackground.load(std::memory_order_acquire), " background lane");
  log_worker(sHelper.load(std::memory_order_acquire), " core 0 helper");
  log_worker(sSecond.load(std::memory_order_acquire), " side worker 2");
}
} // namespace aurora::side_jobs
