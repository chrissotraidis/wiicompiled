#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::RetroRewind,
        "Retro Rewind",
    };
    return descriptor;
}

bool Select(Kind kind) noexcept {
    return kind == Kind::RetroRewind;
}

} // namespace RuntimeProduct
