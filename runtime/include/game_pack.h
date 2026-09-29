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
#define KARTPAD_GAME_PACK_ABI 2u

// Runtime state that lives in headers exists once, in the app: with
// MKW_GAME_PACK_MODULE the headers declare it extern for the pack. Thread-local
// ones use __thread, which reaches the app's variable directly; a C++
// thread_local declaration would call an accessor the app does not export.

struct CpuContext;

// Translated functions that the app's native code calls by name: it wraps or
// stands in for them (hle/os/os_init.cpp registers wrappers at the very address
// they wrap, so a registry lookup would find the wrapper). The pack passes the
// app a pointer to each, in this order.
#define KARTPAD_GAME_PACK_ORIGINALS(X) \
    X(8012B830) X(801A0620) X(801A1ED8) X(801A961C) \
    X(801AADE0) X(801D8D30) X(801D9E94) X(8055531C)

extern "C" {
struct KartPadGamePackInfo {
    uint32_t abi;
    const char* appVersion;  // The app version the pack was built for.
    uint32_t sda1Base;       // _SDA_BASE_ (r13)
    uint32_t sda2Base;       // _SDA2_BASE_ (r2)
    void (*initializeDataSections)();
    void (*const* originals)(CpuContext*);  // KARTPAD_GAME_PACK_ORIGINALS order
};
}

namespace GamePack {
// Published app: load the pack named by KARTPAD_GAME_PACK before any translated
// code runs; throws std::runtime_error with a player-readable message.
// Other builds link the game code directly and this does nothing.
void EnsureLoaded();
}
