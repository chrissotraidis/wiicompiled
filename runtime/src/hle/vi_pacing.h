#pragma once

#include <chrono>

// Retrace backlog policy, kept pure so it can be checked on the host.
//
// Each retrace advances the VI clock by one interval rather than to "now", so short host
// hitches are caught up and game time stays in step with wall time. After a long stall (the
// app in the background, a suspended process) that would replay every missed retrace back to
// back and the game would run several times too fast until it had caught up (#330). A backlog
// longer than kMaxRetraceBacklog is dropped instead. Loading stalls (1-2 s) stay below it and
// still catch up as before.
namespace vi_pacing {

inline constexpr std::chrono::milliseconds kMaxRetraceBacklog{3000};

// The time to record as the latest retrace: the scheduled stamp, or `now` when the stamp is
// further behind than the backlog limit.
template <class TimePoint>
constexpr TimePoint NextRetraceStamp(TimePoint scheduled, TimePoint now) noexcept {
    return now - scheduled > kMaxRetraceBacklog ? now : scheduled;
}

} // namespace vi_pacing
