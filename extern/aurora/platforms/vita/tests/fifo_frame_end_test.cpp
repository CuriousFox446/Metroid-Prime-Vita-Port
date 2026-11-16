// Host test of the deferred frame end (fifo::queue_frame_end, finish_frame_end) against the real
// fifo.cpp and a stub command processor. Build and run (from this directory):
//   g++ -std=c++20 -DFMT_HEADER_ONLY -Ififo_stub -I../../../lib -I../../../include -I<fmt>/include \
//       fifo_frame_end_test.cpp ../../../lib/gx/fifo.cpp -o fifo_frame_end_test -pthread
//   ./fifo_frame_end_test

#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"
#include "thread.hpp"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char* module, const char* message, unsigned int) noexcept {
  std::fprintf(stderr, "[%s] %s\n", module, message);
}
namespace thread {
void set_current(const Options&) noexcept {}
} // namespace thread
} // namespace aurora

namespace fifo = aurora::gx::fifo;
using Clock = std::chrono::steady_clock;

namespace {
constexpr uint8_t kRecord = 0x01; // test command: u32 value, appended to sLog

std::mutex sLogMutex;
std::vector<uint32_t> sLog; // what the processor saw, in order
std::atomic<int> sDrawCacheClears{0};
std::atomic<bool> sTaskOnProducer{false};
std::thread::id sProducer;
int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void record(uint32_t value) {
  fifo::write_u8(kRecord);
  fifo::write_u32(value);
}

std::vector<uint32_t> log_copy() {
  std::lock_guard lock{sLogMutex};
  return sLog;
}

bool log_is(std::initializer_list<uint32_t> want) { return log_copy() == std::vector<uint32_t>(want); }

std::vector<uint32_t> range(uint32_t from, uint32_t to) {
  std::vector<uint32_t> out;
  for (uint32_t v = from; v <= to; ++v) {
    out.push_back(v);
  }
  return out;
}
} // namespace

// The stub processor: records, and the FIFO task the frame end queues.
namespace aurora::gx::fifo {
ProcessResult process(const uint8_t* data, uint32_t size) noexcept {
  uint32_t at = 0;
  while (at < size) {
    const uint8_t cmd = data[at++];
    if (cmd == kRecord) {
      std::lock_guard lock{sLogMutex};
      sLog.push_back(be32(data + at));
      at += 4;
    } else if (cmd == GX_AURORA) {
      const uint16_t sub = uint16_t(data[at] << 8 | data[at + 1]);
      at += 2;
      if (sub != GX_AURORA_PORT_FIFO_TASK) {
        std::printf("FAIL: unexpected aurora command 0x%04X\n", sub);
        std::abort();
      }
      uint64_t ptr = 0;
      for (int i = 0; i < 8; ++i) {
        ptr = ptr << 8 | data[at + i];
      }
      at += 8;
      std::unique_ptr<std::function<void()>> task{reinterpret_cast<std::function<void()>*>(uintptr_t(ptr))};
      (*task)();
    } else {
      std::printf("FAIL: unexpected command 0x%02X\n", cmd);
      std::abort();
    }
  }
  return {at, false};
}
void clear_draw_cache() noexcept { ++sDrawCacheClears; }
} // namespace aurora::gx::fifo

int main() {
  sProducer = std::this_thread::get_id();
  fifo::init();

  // Frame 1: records published as the frame goes, then a slow frame end.
  fifo::begin_frame();
  for (uint32_t v = 1; v <= 100; ++v) {
    record(v);
    if (v % 10 == 0) {
      fifo::publish();
    }
  }
  const auto queued = Clock::now();
  fifo::queue_frame_end([] {
    sTaskOnProducer = std::this_thread::get_id() == sProducer;
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the FIFO thread finishing the frame
    std::lock_guard lock{sLogMutex};
    sLog.push_back(1000);
  });
  check(Clock::now() - queued < std::chrono::milliseconds(50), "queue_frame_end returns without waiting");
  check(!fifo::frame_active(), "queue_frame_end ends the frame for the producer");

  // The game's next tick: commands written outside a frame stay unpublished.
  for (uint32_t v = 200; v <= 210; ++v) {
    record(v);
  }
  fifo::publish(); // no frame: a no-op

  // Another thread asking (the FIFO or render worker) does not wait.
  {
    const auto start = Clock::now();
    std::thread other([] { fifo::finish_frame_end(); });
    other.join();
    check(Clock::now() - start < std::chrono::milliseconds(50), "finish_frame_end off the producer does not wait");
  }

  fifo::finish_frame_end();
  {
    auto want = range(1, 100);
    want.push_back(1000);
    check(log_copy() == want, "frame end runs after the frame's commands, before the tick's");
  }
  check(!sTaskOnProducer, "the frame end runs on the FIFO thread");
  check(sDrawCacheClears == 1, "the FIFO thread clears its draw cache at the frame end");
  check(fifo::get_buffer_size() == 11 * 5, "the processed frame is dropped and the tick's commands kept");
  check(fifo::get_buffer_data()[0] == kRecord && be32(fifo::get_buffer_data() + 1) == 200,
        "the tick's commands moved to the front");

  // Frame 2 publishes the tick's commands first, then its own.
  fifo::begin_frame();
  record(300);
  fifo::publish();
  fifo::drain();
  {
    auto want = range(1, 100);
    want.push_back(1000);
    for (uint32_t v : range(200, 210)) {
      want.push_back(v);
    }
    want.push_back(300);
    check(log_copy() == want, "next frame processes the tick's commands, then its own");
  }

  // Frame 3: a drain in the tick (GXDrawDone) runs the frame end and everything after it.
  record(301);
  fifo::queue_frame_end([] {
    std::lock_guard lock{sLogMutex};
    sLog.push_back(2000);
  });
  record(400);
  fifo::drain();
  {
    const auto log = log_copy();
    check(log.size() >= 3 && log[log.size() - 3] == 301 && log[log.size() - 2] == 2000 && log.back() == 400,
          "a drain in the tick runs the frame end, then the tick's commands");
  }
  fifo::finish_frame_end(); // already ended: returns
  check(fifo::get_buffer_size() == 0, "nothing left after the drain");
  check(sDrawCacheClears == 2, "one draw cache clear per frame end");

  // Frame 4: the next queue_frame_end waits for a pending one (one in flight at a time).
  fifo::begin_frame();
  fifo::queue_frame_end([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard lock{sLogMutex};
    sLog.push_back(3000);
  });
  fifo::begin_frame(); // as aurora's begin_frame would, after its own finish_frame_end
  record(500);
  fifo::queue_frame_end([] {
    std::lock_guard lock{sLogMutex};
    sLog.push_back(4000);
  });
  fifo::finish_frame_end();
  {
    const auto log = log_copy();
    check(log.size() >= 3 && log[log.size() - 3] == 3000 && log[log.size() - 2] == 500 && log.back() == 4000,
          "frame ends stay in order");
  }

  fifo::shutdown();
  std::printf(g_failures == 0 ? "fifo_frame_end_test: all passed\n" : "fifo_frame_end_test: %d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
