// Game pack: translated game code shipped separately from the published app.
//
// The published KartPad app contains the runtime but no game code. PadForge
// builds the player's own game pack from their disc: one library with every
// translated function, its registration and dispatch tables, and the game's data
// sections. Loading the pack runs its static registrars, which register the
// functions with this runtime; the pack also supplies the values below.
#pragma once

#include <cstdint>

// Bump when the runtime/pack interface changes; the app refuses other values.
#define KARTPAD_GAME_PACK_ABI 1u

extern "C" {
struct KartPadGamePackInfo {
    uint32_t abi;
    const char* appVersion;  // The app version the pack was built for.
    uint32_t sda1Base;       // _SDA_BASE_ (r13)
    uint32_t sda2Base;       // _SDA2_BASE_ (r2)
    void (*initializeDataSections)();
};
}

namespace GamePack {
// Published app: load the pack named by KARTPAD_GAME_PACK before any translated
// code runs; throws std::runtime_error with a player-readable message.
// Other builds link the game code directly and this does nothing.
void EnsureLoaded();
}
