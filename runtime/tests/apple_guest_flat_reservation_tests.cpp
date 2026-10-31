#include "guest_flat_memory.h"

#include <cstdint>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

// The published Wii layout, as requested by Memory::Config::WiiDefaults().
std::vector<GuestFlat::RegionRequest> WiiLayout() {
    constexpr uint64_t kMem1 = 24u * 1024u * 1024u;
    constexpr uint64_t kMem2 = 128u * 1024u * 1024u;
    return {
        {0x00000000u, kMem1, GuestFlat::Backing::Mem1},
        {0x80000000u, kMem1, GuestFlat::Backing::Mem1},
        {0xC0000000u, kMem1, GuestFlat::Backing::Mem1},
        {0x10000000u, kMem2, GuestFlat::Backing::Mem2},
        {0x90000000u, kMem2, GuestFlat::Backing::Mem2},
        {0xD0000000u, kMem2, GuestFlat::Backing::Mem2},
        {0x81800000u, 0x200000u, GuestFlat::Backing::Owned},
        {0xE0000000u, 0x2000u, GuestFlat::Backing::Owned},
    };
}

int main() {
    // A 4 GiB reservation cannot fit in the default iOS virtual address space, so the
    // reservation must cover the layout with one guard page and no more.
    const auto wii = WiiLayout();
    const uint64_t needed = GuestFlat::ReservationSizeFor(wii);
    const uint64_t highestGuestByte = 0xE0002000ull;
    if (needed < highestGuestByte) {
        return Fail("reservation does not cover the highest guest byte");
    }
    if (needed >= 0x1'0000'0000ull) {
        return Fail("Wii layout still needs a full 4 GiB reservation");
    }
    if (needed > highestGuestByte + 0x10000ull) {
        return Fail("reservation wastes more than one host page past the layout");
    }

    // An empty layout still reserves one page rather than zero.
    if (GuestFlat::ReservationSizeFor({}) != static_cast<uint64_t>(getpagesize())) {
        return Fail("empty layout did not reserve exactly one page");
    }

    // A layout that reaches the top of the space is capped at the full span.
    const std::vector<GuestFlat::RegionRequest> full{
        {0xFF000000u, 0x01000000u, GuestFlat::Backing::Owned}};
    const uint64_t fullSize = GuestFlat::ReservationSizeFor(full);
    if (fullSize < 0x1'0000'0000ull || fullSize > 0x1'0000'0000ull + 0x10000ull) {
        return Fail("top-of-space layout was not clamped to the guest span");
    }

    std::cout << "flat guest reservation sizing passed: wii=" << needed
              << " highest=" << highestGuestByte << '\n';
    return 0;
}
