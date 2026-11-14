// Guest preemption points.
//
// Guest threads are fibers on one host thread, so a running guest thread keeps the processor
// until it blocks. On the console a retrace or timer interrupt stops a low-priority thread the
// moment a higher-priority one becomes ready. Mario Kart Wii's movie player relies on that: the
// THP thumbnails in the main, single-player and online menus are decoded on a thread below the
// main thread, and decoding one movie frame can outlast the time the main thread has left before
// its next retrace. Without preemption the main thread started late whenever a movie frame was
// still decoding at the retrace, and a menu that had fallen behind stayed slow, down to about
// 15 frames per second on the Android emulator (found while investigating KartPad #554).
//
// The game pack calls RuntimeGuestPreemptionPoint on entry to each THP macroblock-row decoder
// (ApplyRuntimeCallOptions in abi_bridge.h). It delivers what an interrupt would have delivered
// by now, then lets the scheduler switch to a higher-priority thread that became ready. The
// interrupted thread resumes here when the scheduler next selects it.

#include <chrono>

#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "memory.h"
#include "os_internal.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// Four checks a millisecond is finer than any guest timer the menus use, and keeps the cost of
// a row that needs nothing to one clock read.
constexpr auto kMinCheckInterval = std::chrono::microseconds(250);

bool PreemptionEnabled()
{
#if defined(__ANDROID__)
    // For comparisons: `adb shell setprop debug.kartpad.guest_preemption 0` before starting the game.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.kartpad.guest_preemption", value) <= 0 || value[0] != '0';
    }();
    return enabled;
#else
    return true;
#endif
}

} // namespace

extern "C" __attribute__((visibility("default"))) void RuntimeGuestPreemptionPoint(CpuContext* ctx)
{
    // Guest scheduling runs on one host thread, so this needs no synchronisation.
    static Clock::time_point nextCheck{};
    const auto now = Clock::now();
    if (now < nextCheck) {
        return;
    }
    nextCheck = now + kMinCheckInterval;

    // Interrupts stay pending inside a guest critical section, a disabled scheduler, or a retrace
    // that is already being delivered.
    if (!ctx || !PreemptionEnabled() || !OS_HLE_InterruptsEnabled() || VI_HLE_IsAdvancingRetrace() ||
        ::Memory::Read32(kSchedulerIdleFlagAddr) != 0) {
        return;
    }

    // Handlers run on a private register file, as on the console, and cannot switch threads: they
    // only mark the threads they wake as ready.
    {
        GuestInterruptCallbackContext interrupt;
        OS_HLE_BeginDeferredGuestCallbacks();
        try {
            ProcessSleepTimers(interrupt.get());
        } catch (...) {
            OS_HLE_EndDeferredGuestCallbacks();
            throw;
        }
        OS_HLE_EndDeferredGuestCallbacks();
    }
    OS_HLE_ProcessAlarmsDeferred(8);
    Audio_HLE_PollDeferred();
    VI_HLE_ProcessRetracesDeferred(1);

    // The interrupt epilogue: switch if a higher-priority thread became ready. SelectThread and
    // the context save inside it write registers, so this thread gets its own back afterwards.
    if (::Memory::Read32(kSchedulerReschedCounterAddr) == 0) {
        return;
    }
    const CpuContext interrupted = *ctx;
    ctx->gpr[3] = 0;
    SelectThread_801a9c08(ctx);
    *ctx = interrupted;
}
