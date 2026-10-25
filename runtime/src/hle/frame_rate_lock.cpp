// "30 FPS races": every race runs in Mario Kart Wii's own 30 frames-per-second mode.
//
// Adapted from DriftDroid's frame_rate_lock.cpp (github.com/dorPXP/driftdroid, GPL-3.0), with the
// Retro Rewind call site for KartPad's pack 6.13.4.
//
// When a race scene starts, the game picks 30 FPS for three or four screens (and two screens
// online) and passes the answer to GameScene::setFramerate (0x8051AF84, r4 = 1 for 30 FPS). The
// game logic then takes two steps per drawn frame and the display shows each frame for two
// retraces, so the race plays at normal speed while drawing half as often. This option makes
// that answer "30 FPS" for every race, so a phone that cannot hold 60 plays at full speed
// instead of in slow motion. Only the race scene's own call is changed (by return address);
// menus and every other scene choose for themselves. Time Trial and ghost races (game modes 2
// and 5) keep 60, as Retro Rewind itself does, so recorded ghosts stay 60 FPS ghosts.
//
// Retro Rewind 6.13.4 decides again after the game: its routine at 0x8183B7B4 calls
// setFramerate with the pack's own 30 FPS setting (returning to 0x8183B82C), and forces 60 in
// modes 2 and 5 when the scene kind (+0x28) is 2. That call is changed the same way.

#include "abi_bridge.h"
#include "memory.h"

#include <atomic>

namespace {

constexpr uint32_t kSetFramerateReturnInRaceScene = 0x80554274u;
constexpr uint32_t kSetFramerateReturnInRetroRewind = 0x8183B82Cu;
constexpr uint32_t kThirtyFramesPerSecond = 1u;
constexpr uint32_t kRacedataPointer = 0x809BD728u;  // Racedata::sInstance
constexpr uint32_t kGameModeOffset = 0xB70u;

bool SixtyOnlyMode() {
    if (!Memory::Contains(kRacedataPointer, 4)) {
        return false;
    }
    const uint32_t racedata = Memory::Read32(kRacedataPointer);
    if (!Memory::Contains(racedata + kGameModeOffset, 4)) {
        return false;
    }
    const uint32_t mode = Memory::Read32(racedata + kGameModeOffset);
    return mode == 2u || mode == 5u;
}

std::atomic<bool> g_enabled{false};

} // namespace

extern "C" __attribute__((visibility("default"))) void RuntimeSetThirtyFpsRaces(bool enabled) noexcept {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) void RuntimeFrameRateLockHook(CpuContext* ctx) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    if ((ctx->lr == kSetFramerateReturnInRaceScene || ctx->lr == kSetFramerateReturnInRetroRewind) &&
        !SixtyOnlyMode()) {
        ctx->gpr[4] = kThirtyFramesPerSecond;
    }
}

