#pragma once

#include <aurora/gfx.h>

#include <cstddef>
#include <cstdint>

namespace aurora::gfx::presentation {
// Records a successful surface present. `duplicated` marks a present that repeated the previous
// image instead of showing new motion.
void record_present(bool duplicated) noexcept;
AuroraPresentTiming snapshot_timing() noexcept;

struct Schedule {
  uint64_t baseNanos = 0;
  uint64_t intervalNanos = 0;
};
// Schedule set by the producer for the frame currently being ended.
Schedule present_schedule() noexcept;
bool producer_paced() noexcept;

// Paces presents in blocking present modes to the display refresh so surplus frames are dropped
// instead of stalling the producer. Render worker only.
bool admit_display_present(uint64_t atNanos) noexcept;
// Reports how long acquiring and presenting the surface blocked.
void report_display_present_wait(uint64_t waitNanos) noexcept;
// Whether the display refreshes fast enough to show frames between native frames `intervalNanos` apart.
bool display_shows_interpolation(uint64_t intervalNanos) noexcept;

// steady_clock time in nanoseconds, the clock present schedules are expressed in.
uint64_t now_nanos() noexcept;
// Blocks until `deadlineNanos` with sub-millisecond precision; 0 returns immediately.
void wait_until(uint64_t deadlineNanos) noexcept;

// Guest-RAM write generation for a host range, or AURORA_GUEST_WRITE_UNTRACKED.
uint64_t guest_write_generation(const void* hostPtr, size_t size) noexcept;
// Reports a write Aurora made to guest RAM.
void notify_guest_write(const void* hostPtr, size_t size) noexcept;
} // namespace aurora::gfx::presentation
