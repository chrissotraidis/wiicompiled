#pragma once

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <mutex>
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

// libc++ on Windows truncates sub-millisecond waits to zero, so its timed condvar waits spin.
void wait_once(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
               std::chrono::nanoseconds timeout) noexcept;

template <typename Rep, typename Period, typename Pred>
bool wait_for(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
              std::chrono::duration<Rep, Period> timeout, Pred pred) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
  while (!pred()) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      return pred();
    }
    wait_once(cv, lock, remaining);
  }
  return true;
}

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
