#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::BaseGame,
        "KartPad",
    };
    return descriptor;
}

bool Select(Kind kind) noexcept {
    return kind == Kind::BaseGame;
}

} // namespace RuntimeProduct
