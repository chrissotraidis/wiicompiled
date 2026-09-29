#pragma once

#include <atomic>
#include <cstdint>

namespace RuntimeGameGraphicsOptions {

// Game pack (include/game_pack.h): one copy, in the app.
#if defined(MKW_GAME_PACK_MODULE)
extern std::atomic<uint32_t> g_disabledPostProcessingPaths;
#else
inline std::atomic<uint32_t> g_disabledPostProcessingPaths{0};
#endif

inline std::atomic<uint32_t>& DisabledPostProcessingPathsState() noexcept {
    return g_disabledPostProcessingPaths;
}

inline uint32_t DisabledPostProcessingPaths() noexcept {
    return DisabledPostProcessingPathsState().load(std::memory_order_relaxed);
}

inline void SetDisabledPostProcessingPaths(uint32_t disabledMask) noexcept {
    DisabledPostProcessingPathsState().store(disabledMask, std::memory_order_relaxed);
}

inline uint32_t FilterScnRendererPathMask(uint32_t pathMask) noexcept {
    return pathMask & ~(DisabledPostProcessingPaths() | 0x20u);
}

} // namespace RuntimeGameGraphicsOptions
