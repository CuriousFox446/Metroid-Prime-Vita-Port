#include "fifo.hpp"

#include "../thread.hpp"
#include "../vita_fifo_clock.hpp"
#include "command_processor.hpp"

#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#include <tracy/Tracy.hpp>

namespace aurora::gx::fifo {
namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

namespace {
constexpr Module Log{"aurora::gx::fifo"};
constexpr auto kProcessingMode = ProcessingMode::Thread;
constexpr uint32_t kDrawBatchSize = 1;

bool sFrameActive = false;
uint32_t sPendingDraws = 0;
std::atomic<uint64_t> sPublished{0};
std::atomic<uint64_t> sProcessed{0};
uint64_t sStreamBase = 0;
std::mutex sBufferMutex;
std::atomic<uint32_t> sWorkerWake{0};
thread::Thread sWorkerThread;
std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};

// queue_frame_end: the stream position just past the queued frame end (0 when none), and the
// thread that queued it, the only one finish_frame_end waits on.
uint64_t sPendingFrameEnd = 0;
std::thread::id sFrameEndProducer;

// aurora_take_drain_sites: the drains the producer waited in since the last take.
std::array<AuroraDrainSite, AURORA_DRAIN_SITES> sDrainSites{};
uint32_t sDrainSiteCount = 0;
const void* sDrainOrigin = nullptr;

void note_drain(const void* caller, uint64_t waitNs) noexcept {
  for (uint32_t i = 0; i < sDrainSiteCount; ++i) {
    auto& site = sDrainSites[i];
    if (site.caller == caller && site.origin == sDrainOrigin) {
      ++site.calls;
      site.waitNs += waitNs;
      return;
    }
  }
  if (sDrainSiteCount < sDrainSites.size()) {
    sDrainSites[sDrainSiteCount++] = {.caller = caller, .origin = sDrainOrigin, .calls = 1, .waitNs = waitNs};
  }
}

void dispatch_draw_done() noexcept {
  if (const auto callback = sDrawDoneCallback.load(std::memory_order_acquire); callback != nullptr) {
    callback();
  }
}

void wake_worker() noexcept {
  sWorkerWake.fetch_add(1, std::memory_order_release);
  sWorkerWake.notify_all();
}

void process_to(uint64_t target, std::memory_order order) noexcept {
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (processed < target) {
    ProcessResult result{};
    {
      std::lock_guard lock{sBufferMutex};
      // The producer may already be appending the next batch. Only its atomic
      // published watermark is shared with us; sBufferSize is producer-owned.
      const uint64_t published = sPublished.load(std::memory_order_acquire);
      AURORA_ASSERT(processed >= sStreamBase && target <= published,
                    "FIFO processing range [{}, {}) is outside published range [{}, {})", processed, target, sStreamBase,
                    published);
      const auto start = static_cast<uint32_t>(processed - sStreamBase);
      const auto size = static_cast<uint32_t>(target - processed);
      result = process(detail::sBufferData + start, size);
    }
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= target - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  target - processed);
    if (result.drawDone) {
      dispatch_draw_done();
    }
    processed += result.bytesProcessed;
    sProcessed.store(processed, order);
    sProcessed.notify_all();
  }
}

void worker_main(std::stop_token token) noexcept {
  std::stop_callback wakeOnStop{token, wake_worker};
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    if (published != processed) {
      process_to(published, std::memory_order_release);
      continue;
    }

    if (token.stop_requested()) {
      break;
    }
    vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kState);
    sWorkerWake.wait(event, std::memory_order_acquire);
    vita_gl::fifo_clock::mark(vita_gl::fifo_clock::kIdle);
  }
}

void start_worker() {
  if (kProcessingMode != ProcessingMode::Thread || sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread = thread::Thread{{
                                     .name = "Aurora FIFO processor",
                                     .affinity = thread::Affinity::SharedCache,
                                     .vitaCore = 2,
                                 },
                                 worker_main};
}

void stop_worker() {
  if (!sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread.request_stop();
  sWorkerThread.join();
}
} // namespace

ProcessingMode processing_mode() noexcept { return kProcessingMode; }

void init() {
  stop_worker();

  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  AURORA_ASSERT(detail::sBufferData != nullptr, "fifo::init: failed to allocate {} bytes", initialCapacity);
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;

  sFrameActive = false;
  sPendingDraws = 0;
  sStreamBase = 0;
  sPublished.store(0, std::memory_order_relaxed);
  sProcessed.store(0, std::memory_order_relaxed);
  sWorkerWake.store(0, std::memory_order_relaxed);

  start_worker();
}

void shutdown() { stop_worker(); }

void begin_frame() noexcept { sFrameActive = true; }

void end_frame() noexcept {
  sFrameActive = false;
  clear_draw_cache(); // command_processor
}

bool frame_active() noexcept { return sFrameActive; }

void queue_frame_end(std::function<void()> task) {
  AURORA_ASSERT(kProcessingMode == ProcessingMode::Thread && !detail::sInDisplayList && sFrameActive,
                "fifo::queue_frame_end: needs an active frame on the FIFO thread");
  finish_frame_end(); // one in flight at a time
  auto* queued = new std::function<void()>([task = std::move(task)] {
    clear_draw_cache(); // command_processor, as end_frame does once drained
    task();
  });
  write_u8(GX_AURORA);
  write_u16(GX_AURORA_PORT_FIFO_TASK);
  write_u64(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queued)));
  publish();
  sFrameActive = false;
  sPendingFrameEnd = sStreamBase + detail::sBufferSize;
  sFrameEndProducer = std::this_thread::get_id();
}

void finish_frame_end() noexcept {
  if (sPendingFrameEnd == 0 || std::this_thread::get_id() != sFrameEndProducer) {
    return;
  }
  ZoneScoped;
  uint64_t processed = sProcessed.load(std::memory_order_acquire);
  while (processed < sPendingFrameEnd) {
    sProcessed.wait(processed, std::memory_order_acquire);
    processed = sProcessed.load(std::memory_order_acquire);
  }
  sPendingFrameEnd = 0;
  // Nothing past the frame end was published (the frame is over), so the processor is idle:
  // drop what it processed, keeping what the producer wrote since (drain does this otherwise).
  std::lock_guard lock{sBufferMutex};
  const auto done = static_cast<uint32_t>(processed - sStreamBase);
  if (done != 0) {
    std::memmove(detail::sBufferData, detail::sBufferData + done, detail::sBufferSize - done);
    detail::sBufferSize -= done;
    sStreamBase = processed;
  }
}

void write_data_grow(const void* data, uint32_t length) {
  const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
  AURORA_ASSERT(needed64 <= std::numeric_limits<uint32_t>::max(), "fifo::write_data: buffer size overflow");
  const auto needed = static_cast<uint32_t>(needed64);
  const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
  const auto newCapacity = static_cast<uint32_t>(
      std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
  const auto grow = [newCapacity] {
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
    detail::sBufferData = resized;
  };
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    grow();
  } else {
    grow();
  }
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCapacity;
}

void publish() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }

  const uint64_t target = sStreamBase + detail::sBufferSize;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_release);
    if (kProcessingMode == ProcessingMode::Thread) {
      wake_worker();
    } else {
      process_to(target, std::memory_order_relaxed);
    }
  }
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {
  return sDrawDoneCallback.exchange(callback, std::memory_order_acq_rel);
}

void finish_draw() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }
  if (++sPendingDraws >= kDrawBatchSize) {
    publish();
  }
}

void patch_u32(uint32_t offset, uint32_t val) {
  AURORA_ASSERT(!detail::sInDisplayList && offset <= detail::sBufferSize &&
                    sizeof(uint32_t) <= detail::sBufferSize - offset,
                "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  AURORA_ASSERT(sStreamBase + offset >= sPublished.load(std::memory_order_relaxed),
                "fifo::patch_u32: offset {} is below the published watermark", offset);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  const uint32_t bytesWritten = detail::sDlWritePos;
  const uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

void set_drain_origin(const void* origin) noexcept { sDrainOrigin = origin; }

void drain() {
  if (detail::sBufferSize == 0) {
    return;
  }

  ZoneScoped;
  const void* caller = __builtin_return_address(0);
  const auto waitStart = std::chrono::steady_clock::now();
  const uint64_t target = sStreamBase + detail::sBufferSize;

  switch (kProcessingMode) {
  case ProcessingMode::Drain:
  case ProcessingMode::Inline:
    sPublished.store(target, std::memory_order_relaxed);
    process_to(target, std::memory_order_relaxed);
    break;
  case ProcessingMode::Thread: {
    sPublished.store(target, std::memory_order_release);
    wake_worker();

    uint64_t processed = sProcessed.load(std::memory_order_acquire);
    if (processed < target) {
      do {
        sProcessed.wait(processed, std::memory_order_acquire);
        processed = sProcessed.load(std::memory_order_acquire);
      } while (processed < target);
    }
    break;
  }
  }

  {
    std::lock_guard lock{sBufferMutex};
    sStreamBase = target;
    detail::sBufferSize = 0;
  }
  sPendingDraws = 0;
  note_drain(caller, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - waitStart)
                                                .count()));
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }

void clear_buffer() {
  const uint64_t processed = sProcessed.load(std::memory_order_acquire);
  AURORA_ASSERT(sPublished.load(std::memory_order_acquire) == processed,
                "fifo::clear_buffer: published commands are still pending");
  std::lock_guard lock{sBufferMutex};
  sStreamBase = processed;
  detail::sBufferSize = 0;
  sPendingDraws = 0;
}

} // namespace aurora::gx::fifo

uint32_t aurora_take_drain_sites(AuroraDrainSite out[AURORA_DRAIN_SITES]) {
  using namespace aurora::gx::fifo;
  const uint32_t count = sDrainSiteCount;
  std::copy_n(sDrainSites.begin(), count, out);
  sDrainSiteCount = 0;
  return count;
}

const void* aurora_drain_anchor(void) { return reinterpret_cast<const void*>(&aurora_take_drain_sites); }
