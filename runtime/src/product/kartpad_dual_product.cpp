#include "runtime_product.h"

#include <atomic>

namespace RuntimeProduct {
namespace {
std::atomic<Kind> g_selectedKind{Kind::BaseGame};
constexpr Descriptor kBaseDescriptor{Kind::BaseGame, "Mario Kart Wii"};
constexpr Descriptor kRetroDescriptor{Kind::RetroRewind, "Retro Rewind"};
} // namespace

const Descriptor& Active() noexcept {
    return g_selectedKind.load(std::memory_order_acquire) == Kind::RetroRewind
        ? kRetroDescriptor
        : kBaseDescriptor;
}

bool Select(Kind kind) noexcept {
    g_selectedKind.store(kind, std::memory_order_release);
    return true;
}

} // namespace RuntimeProduct
