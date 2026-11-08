#pragma once

#include <concepts>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace aurora::thread {

enum class Priority { Low, Normal, High };
enum class Affinity { None, SharedCache };

struct Options {
  std::string name;
  Priority priority = Priority::Normal;
  Affinity affinity = Affinity::None;
};

// Applies thread options to the current thread
void set_current(const Options& options) noexcept;

// PS Vita: names the current thread for the thread log (log_cpu) without changing its priority or
// affinity; set_current names its threads so too. Elsewhere nothing.
void register_current(const std::string& name) noexcept;
// PS Vita: logs each named thread's core (its affinity mask, the core it is on and the one it last
// ran on), priority, the share of the time since the last call it ran, and its moves between cores.
// Elsewhere nothing.
void log_cpu() noexcept;

class Thread {
public:
  Thread() noexcept = default;

  template <typename Function>
    requires std::invocable<std::decay_t<Function>&, std::stop_token>
  explicit Thread(Options options, Function&& function)
  : mThread{[options = std::move(options), function = std::forward<Function>(function)](std::stop_token token) mutable {
    set_current(options);
    std::invoke(function, token);
  }} {}

  Thread(Thread&&) noexcept = default;
  Thread& operator=(Thread&&) noexcept = default;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  ~Thread() = default;

  [[nodiscard]] bool joinable() const noexcept { return mThread.joinable(); }
  [[nodiscard]] std::thread::id get_id() const noexcept { return mThread.get_id(); }
  [[nodiscard]] std::stop_token get_stop_token() const noexcept { return mThread.get_stop_token(); }
  bool request_stop() noexcept { return mThread.request_stop(); }
  void join() { mThread.join(); }
  std::jthread::native_handle_type native_handle() { return mThread.native_handle(); }

private:
  std::jthread mThread;
};

} // namespace aurora::thread
