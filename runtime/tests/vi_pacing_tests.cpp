// Host check for the VI retrace backlog policy (#330).
// clang++ -std=c++20 -I runtime/src/hle runtime/tests/vi_pacing_tests.cpp && ./a.out
#include "vi_pacing.h"

#include <cstdio>

using namespace std::chrono;
using Clock = steady_clock;

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    };
    const Clock::time_point now{seconds(1000)};
    const auto interval = microseconds(16666);
    // On time or slightly behind: keep the schedule so short hitches still catch up.
    expect(vi_pacing::NextRetraceStamp(now, now) == now, "on time keeps stamp");
    expect(vi_pacing::NextRetraceStamp(now - interval * 3, now) == now - interval * 3, "3 frames behind keeps stamp");
    expect(vi_pacing::NextRetraceStamp(now - seconds(2), now) == now - seconds(2), "2 s loading stall keeps stamp");
    expect(vi_pacing::NextRetraceStamp(now - vi_pacing::kMaxRetraceBacklog, now) == now - vi_pacing::kMaxRetraceBacklog, "exactly at limit keeps stamp");
    // Long stalls: drop the backlog.
    expect(vi_pacing::NextRetraceStamp(now - vi_pacing::kMaxRetraceBacklog - milliseconds(1), now) == now, "just past limit resyncs");
    expect(vi_pacing::NextRetraceStamp(now - minutes(5), now) == now, "5 minute background resyncs");
    // A stamp in the future is never moved.
    expect(vi_pacing::NextRetraceStamp(now + interval, now) == now + interval, "future stamp kept");
    // After a 5 minute pause no retraces are due, instead of about 18000.
    const auto last = vi_pacing::NextRetraceStamp(now - minutes(5), now);
    int due = 0;
    for (auto t = last + interval; t <= now; t += interval) ++due;
    expect(due == 0, "no backlog after resync");
    if (failures) std::printf("%d failed\n", failures); else std::printf("vi_pacing: all passed\n");
    return failures ? 1 : 0;
}
