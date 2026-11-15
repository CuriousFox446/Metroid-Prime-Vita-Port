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
// Whether the job has run (true for 0, no job).
bool done(uint32_t job) noexcept;
// Waits until the job has run, at most half a second (a hang, which is logged, then let go).
// `fifo`: the FIFO processor's wait, which the log counts apart from the others.
void wait(uint32_t job, bool fifo) noexcept;
// The log's [side jobs] line: what the worker ran and who waited for it, since the last line.
void log_stats() noexcept;
} // namespace aurora::side_jobs
