#include "game_pack.h"

#if defined(MKW_GAME_PACK_APP) && MKW_GAME_PACK_APP

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "abi_bridge.h"
#include "game_graphics_options.h"
#include "isa/ppc_isa_fpenv.h"
#include "recomp_mod_loader.h"
#include "system_bridge.h"

#ifndef KARTPAD_APP_VERSION
#error "The published app records its version for the game pack check"
#endif

// Supplied by the pack instead of the generated RuntimeConfig.h.
namespace RuntimeConfig {
uint32_t SDA1_BASE = 0;
uint32_t SDA2_BASE = 0;
}

namespace {
void (*g_initializeDataSections)() = nullptr;
void (*const* g_originals)(CpuContext*) = nullptr;
enum OriginalIndex {
#define KARTPAD_ORIGINAL_INDEX(hex) kOriginal_##hex,
    KARTPAD_GAME_PACK_ORIGINALS(KARTPAD_ORIGINAL_INDEX)
#undef KARTPAD_ORIGINAL_INDEX
};
}

// The pack declares these header variables extern (MKW_GAME_PACK_MODULE) and
// binds to the app's copy. Referencing them here guarantees the app defines
// and exports each one, whatever else the runtime happens to use.
extern "C" __attribute__((used, visibility("default"))) void kartpad_game_pack_shared_state() {
    (void)&g_currentCpuContext;
    (void)&g_mkwHostNiActive;
    (void)&g_mkwNiFlushThreshold;
    (void)&RecompMod::g_currentTranslatedExecutionAddress;
    (void)&g_publishedStaticIndirectDispatchTable;
    (void)&g_indirectResolvedDispatchMemo;
    (void)&g_indirectRawDispatchMemo;
    (void)TranslatedFunctionRegistry::IsLookupPublished();
    (void)&RuntimeGameGraphicsOptions::g_disabledPostProcessingPaths;
}

extern "C" void InitializeDataSections() {
    if (!g_initializeDataSections) {
        throw std::runtime_error("The game pack is not loaded.");
    }
    g_initializeDataSections();
}

// The app's native code calls a few translated functions by name (see
// KARTPAD_GAME_PACK_ORIGINALS). Other builds link those calls directly; here
// they go to the pack's own functions. Hidden, so they never interpose on the
// pack's definitions.
#define KARTPAD_PACK_FUNCTION(hex) \
    extern "C" __attribute__((visibility("hidden"))) void func_##hex(CpuContext* ctx) { \
        g_originals[kOriginal_##hex](ctx); \
    }
KARTPAD_GAME_PACK_ORIGINALS(KARTPAD_PACK_FUNCTION)
#undef KARTPAD_PACK_FUNCTION

void GamePack::EnsureLoaded() {
    if (g_initializeDataSections) {
        return;
    }
    const char* path = std::getenv("KARTPAD_GAME_PACK");
    if (!path || !*path) {
        throw std::runtime_error(
            "No game pack is installed. Build yours with PadForge from your own disc, "
            "then add it in KartPad.");
    }
    void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char* reason = dlerror();
        throw std::runtime_error(std::string("The game pack could not be loaded: ") +
                                 (reason ? reason : "unknown error"));
    }
    const auto* info = static_cast<const KartPadGamePackInfo*>(dlsym(handle, "kartpad_game_pack_info"));
    if (!info || info->abi != KARTPAD_GAME_PACK_ABI || !info->initializeDataSections ||
        !info->originals) {
        throw std::runtime_error("This file is not a KartPad game pack for this app. Rebuild it with PadForge.");
    }
    if (!info->appVersion || std::strcmp(info->appVersion, KARTPAD_APP_VERSION) != 0) {
        throw std::runtime_error(std::string("This game pack was built for KartPad ") +
                                 (info->appVersion ? info->appVersion : "?") +
                                 " but this app is " KARTPAD_APP_VERSION
                                 ". Rebuild it with PadForge for this version.");
    }
    RuntimeConfig::SDA1_BASE = info->sda1Base;
    RuntimeConfig::SDA2_BASE = info->sda2Base;
    g_originals = info->originals;
    g_initializeDataSections = info->initializeDataSections;
}

#else

void GamePack::EnsureLoaded() {}

#endif
