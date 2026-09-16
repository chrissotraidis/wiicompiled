#pragma once
#if defined(__ANDROID__) || defined(__APPLE__)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

namespace kartpad::diagnostics {
// Opt-in coarse function probes. Inclusive elapsed/thread CPU, never GPU timing
// or statistical samples of arbitrary native functions. Per-thread state avoids
// locking an audio callback; aggregate output is capped to 120 windows/function.
struct FunctionWindow {
  const char *name;
  unsigned calls = 0, windows = 0, unavailable = 0;
  long long wall = 0, cpu = 0, maximum = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
inline bool function_timing_enabled() {
  static const bool enabled = [] { const char *s = std::getenv("KARTPAD_FUNCTION_TIMING"); return s && std::strcmp(s,"1") == 0; }();
  return enabled;
}
inline long long thread_cpu_ns() {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return -1;
  return static_cast<long long>(value.tv_sec) * 1000000000LL + value.tv_nsec;
}
class FunctionScope {
  FunctionWindow *window_;
  std::chrono::steady_clock::time_point start_;
  long long cpu_ = -1;
 public:
  explicit FunctionScope(FunctionWindow& w) : window_(function_timing_enabled() && w.windows < 120 ? &w : nullptr) {
    if (window_) { start_ = std::chrono::steady_clock::now(); cpu_ = thread_cpu_ns(); }
  }
  ~FunctionScope() {
    if (!window_) return;
    const auto end = std::chrono::steady_clock::now();
    const auto cpu = thread_cpu_ns();
    auto& w = *window_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end-start_).count();
    w.wall += elapsed; if (elapsed > w.maximum) w.maximum = elapsed;
    ++w.calls;
    if (cpu_ >= 0 && cpu >= cpu_) w.cpu += cpu-cpu_; else ++w.unavailable;
    if (end-w.last < std::chrono::seconds(5)) return;
    ++w.windows;
    std::fprintf(stderr,"[KartPadFunction] function=%s window=%u calls=%u inclusive_wall_ms=%.3f inclusive_cpu_ms=%.3f max_wall_ms=%.3f interval_ms=%.3f cpu_missing=%u final=%u\n",
      w.name, w.windows, w.calls, w.wall/1e6, w.unavailable ? -1.0 : w.cpu/1e6, w.maximum/1e6,
      std::chrono::duration<double,std::milli>(end-w.last).count(), w.unavailable, w.windows==120);
    w.last=end; w.calls=0; w.wall=0; w.cpu=0; w.maximum=0; w.unavailable=0;
  }
};
}
#define KARTPAD_FUNCTION_SCOPE(label) \
  static thread_local kartpad::diagnostics::FunctionWindow kartpadFunctionWindow{label}; \
  kartpad::diagnostics::FunctionScope kartpadFunctionScope(kartpadFunctionWindow)

#else
#define KARTPAD_FUNCTION_SCOPE(label) ((void)0)
#endif
