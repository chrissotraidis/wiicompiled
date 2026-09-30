// The one symbol the published app looks up in a game pack.
#include "game_pack.h"
#include "generated/RuntimeConfig.h"

#ifndef KARTPAD_APP_VERSION
#error "A game pack records the app version it was built for"
#endif
#ifndef KARTPAD_PACK_FINGERPRINT
#error "A game pack records the interface fingerprint it was built for"
#endif

extern "C" void InitializeDataSections();

#define KARTPAD_DECLARE_ORIGINAL(hex) extern "C" void func_##hex(CpuContext*);
KARTPAD_GAME_PACK_ORIGINALS(KARTPAD_DECLARE_ORIGINAL)
#undef KARTPAD_DECLARE_ORIGINAL

static void (*const kOriginals[])(CpuContext*) = {
#define KARTPAD_ORIGINAL_ENTRY(hex) &func_##hex,
    KARTPAD_GAME_PACK_ORIGINALS(KARTPAD_ORIGINAL_ENTRY)
#undef KARTPAD_ORIGINAL_ENTRY
};

// Readable in the file without loading it (see KARTPAD_GAME_PACK_FINGERPRINT_PREFIX).
extern "C" __attribute__((used, visibility("default"))) const char kartpad_game_pack_fingerprint[] =
    KARTPAD_GAME_PACK_FINGERPRINT_PREFIX KARTPAD_PACK_FINGERPRINT;

extern "C" __attribute__((visibility("default"))) const KartPadGamePackInfo kartpad_game_pack_info = {
    KARTPAD_GAME_PACK_ABI,
    KARTPAD_APP_VERSION,
    RuntimeConfig::SDA1_BASE,
    RuntimeConfig::SDA2_BASE,
    &InitializeDataSections,
    kOriginals,
    kartpad_game_pack_fingerprint + sizeof(KARTPAD_GAME_PACK_FINGERPRINT_PREFIX) - 1,
};
