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
// ABI 3: a pack is accepted by its interface fingerprint, not the app version.
#define KARTPAD_GAME_PACK_ABI 3u

// The pack interface fingerprint (KARTPAD_PACK_FINGERPRINT) is computed by
// KartPad's builder (kartpad_builder.pack_fingerprint) from the staged runtime
// both the app and the pack are built from: the headers a pack compiles
// against, the definitions that reach them and the translation identity. An app
// update that changes none of them keeps working with the player's pack. The
// pack also exports this prefix followed by the fingerprint, so an app can read
// it from the file before loading it.
#define KARTPAD_GAME_PACK_FINGERPRINT_PREFIX "kartpad-pack-fingerprint:"

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
    const char* appVersion;  // The app version the pack was built for (messages only).
    uint32_t sda1Base;       // _SDA_BASE_ (r13)
    uint32_t sda2Base;       // _SDA2_BASE_ (r2)
    void (*initializeDataSections)();
    void (*const* originals)(CpuContext*);  // KARTPAD_GAME_PACK_ORIGINALS order
    const char* fingerprint;                // ABI 3: must equal the app's KARTPAD_PACK_FINGERPRINT
};
}

namespace GamePack {
// Published app: load the pack named by KARTPAD_GAME_PACK before any translated
// code runs; throws std::runtime_error with a player-readable message.
// Other builds link the game code directly and this does nothing.
void EnsureLoaded();
}
