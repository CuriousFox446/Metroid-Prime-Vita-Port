#pragma once

// The PS Vita's side worker (0.42.0 test 18): one thread on the fourth core, the system's own, which
// the CapUnlocker plugin opens to games. The game thread hands it work whose results it does not
// read itself (a skinned model's vertices: only the FIFO processor reads those), and the jobs run one
// at a time, in the order they came. The FIFO processor waits for a job where the FIFO says to
// (GX_AURORA_PORT_SIDE_JOB_FENCE), the game thread where it must (a display list being recorded).
// Without the fourth core there is no worker: submit returns 0 and the caller does the work itself.
// The public face is GXPortSideJob* (dolphin/gx/GXExtra.h).

#include <cstdint>

namespace aurora::side_jobs {
using Fn = void (*)(void* data);

// Queues fn(data) and returns its number (above 0), or 0 when there is no worker. Any thread but
// the worker's own.
uint32_t submit(Fn fn, void* data) noexcept;
// 0.46.0 test 16: as submit, on whichever side worker has fewer jobs queued: the fourth core's or
// the second, on the first core (per-thread slot 2). For jobs in no order with each other (each
// object's particle step); its number works with done and wait, not with a FIFO fence.
uint32_t submit_any(Fn fn, void* data) noexcept;
// Whether the job has run (true for 0, no job).
bool done(uint32_t job) noexcept;
// Waits until the job has run, at most half a second (a hang, which is logged, then let go).
// `fifo`: the FIFO processor's wait, which the log counts apart from the others.
void wait(uint32_t job, bool fifo) noexcept;
// The background lane (0.42.0 test 23): a second thread on the fourth core, below MusyX's mixer, for
// long jobs nobody waits for soon (a movie frame's decode). Its own numbers: a job of one lane means
// nothing to the other's done and wait.
uint32_t submit_background(Fn fn, void* data) noexcept;
bool done_background(uint32_t job) noexcept;
void wait_background(uint32_t job) noexcept;
// The core 0 helper (0.46.0 test 4): a thread on the first core, beside the render worker (core 0
// had half its time free), for the FIFO processor's own work that each draw does apart (a skinned
// call's vertices), which the FIFO processor waits for by its frame's end. Its own numbers too.
// Its wait does not give up: a job writes into the frame being recorded. Since 0.46.0 test 12 the
// FIFO processor runs the jobs up to the one it waits for that the helper has not taken yet, so two
// threads may run them at once: a job must not share scratch with another, and an `ordered` one
// runs only once every job before it has.
uint32_t submit_helper(Fn fn, void* data, bool ordered = false) noexcept;
bool done_helper(uint32_t job) noexcept;
void wait_helper(uint32_t job) noexcept;
// The log's [side jobs] lines: what each worker ran and who waited for it, since the last lines.
void log_stats() noexcept;
} // namespace aurora::side_jobs
