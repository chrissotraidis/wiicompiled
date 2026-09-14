#include "guest_flat_memory.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#if TARGET_OS_IPHONE
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif
#include <unistd.h>
#include <vector>

namespace GuestFlat {
#if TARGET_OS_IPHONE
uint8_t* gFlatGuestBase = nullptr;
#endif
namespace {

struct Section {
    Backing backing = Backing::Owned;
    uint32_t ownedBase = 0;
    uint64_t size = 0;
    int fd = -1;
    uint8_t* host = nullptr;
};

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint64_t offset = 0;
    Section* section = nullptr;
};

uint8_t* g_base = nullptr;
bool g_initialized = false;
std::vector<RegionRequest> g_layout;
std::vector<Section> g_sections;
std::vector<Mapping> g_mappings;
std::mutex g_mutex;
std::atomic<uint32_t> g_mmio{0};
std::atomic<uint32_t> g_efb{0};
std::atomic<uint32_t> g_xguard{0};
std::atomic<uint32_t> g_unmapped{0};
std::atomic<uint32_t> g_unmappedRegions{0};

uint64_t RoundUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t OffsetFor(const RegionRequest& region)
{
    if (region.backing == Backing::Mem1)
        return region.base & 0x01ffffffu;
    if (region.backing == Backing::Mem2)
        return region.base & 0x0fffffffu;
    return 0;
}

bool SameKey(const Section& section, const RegionRequest& region)
{
    return section.backing == region.backing &&
           (region.backing != Backing::Owned || section.ownedBase == region.base);
}

bool SameLayout(const std::vector<RegionRequest>& lhs,
                const std::vector<RegionRequest>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].base != rhs[i].base || lhs[i].size != rhs[i].size ||
            lhs[i].backing != rhs[i].backing)
            return false;
    }
    return true;
}

[[noreturn]] void ThrowErrno(const char* operation)
{
    std::ostringstream message;
    message << operation << " failed: " << std::strerror(errno);
    throw std::runtime_error(message.str());
}

#if !TARGET_OS_IPHONE
int CreateSharedSection(uint64_t size, unsigned ordinal)
{
    const std::string name = "/kartpad-g7-" + std::to_string(getpid()) + "-" +
                             std::to_string(ordinal);
    const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        ThrowErrno("shm_open");
    shm_unlink(name.c_str());
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int saved = errno;
        close(fd);
        errno = saved;
        ThrowErrno("ftruncate");
    }
    return fd;
}
#endif

void SetProtection(uint32_t address, size_t length, int protection)
{
    if (!g_base || length == 0)
        return;
    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    const uint64_t first = static_cast<uint64_t>(address) & ~(pageSize - 1);
    const uint64_t last = RoundUp(static_cast<uint64_t>(address) + length, pageSize);
    mprotect(g_base + first, static_cast<size_t>(last - first), protection);
}

} // namespace

bool IsActive() { return g_initialized; }

void Initialize(const std::vector<RegionRequest>& regions)
{
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        if (!SameLayout(g_layout, regions))
            throw std::runtime_error("Flat guest memory was reinitialized with a different layout");
        for (auto& section : g_sections)
            std::memset(section.host, 0, static_cast<size_t>(section.size));
        return;
    }

    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    // Validate before any fixed overwrite; a bad layout must never map outside
    // our reservation or replace another guest region.
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        if (region.size == 0 || region.size > kGuestSpaceSize - region.base ||
            region.base % pageSize != 0 || OffsetFor(region) % pageSize != 0)
            throw std::runtime_error("Invalid flat guest memory region");
        const uint64_t end = static_cast<uint64_t>(region.base) + RoundUp(region.size, pageSize);
        for (size_t j = 0; j < i; ++j) {
            const auto& prior = regions[j];
            const uint64_t priorEnd = static_cast<uint64_t>(prior.base) + RoundUp(prior.size, pageSize);
            if (region.base < priorEnd && prior.base < end)
                throw std::runtime_error("Overlapping flat guest memory regions");
        }
    }
    const uint64_t reservationSize = kGuestSpaceSize + pageSize;
#if TARGET_OS_IPHONE
    void* reserved = mmap(nullptr, static_cast<size_t>(reservationSize), PROT_NONE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reserved == MAP_FAILED)
        ThrowErrno("mmap iOS flat guest reservation");
    g_base = static_cast<uint8_t*>(reserved);
    gFlatGuestBase = g_base;
#else
    void* requested = reinterpret_cast<void*>(kFixedFlatGuestBase);
    void* reserved = mmap(requested, static_cast<size_t>(reservationSize), PROT_NONE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reserved == MAP_FAILED)
        ThrowErrno("mmap flat guest reservation");
    if (reserved != requested) {
        munmap(reserved, static_cast<size_t>(reservationSize));
        throw std::runtime_error("Apple OS did not honor the fixed flat guest address hint");
    }
    g_base = static_cast<uint8_t*>(reserved);
#endif

    try {
        g_sections.reserve(regions.size());
        for (const auto& region : regions) {
            auto it = std::find_if(g_sections.begin(), g_sections.end(),
                                   [&](const Section& value) { return SameKey(value, region); });
            if (it == g_sections.end()) {
                g_sections.push_back(Section{region.backing,
                                             region.backing == Backing::Owned ? region.base : 0});
                it = std::prev(g_sections.end());
            }
            it->size = std::max(it->size, RoundUp(OffsetFor(region) + region.size, pageSize));
        }

        for (size_t i = 0; i < g_sections.size(); ++i) {
            auto& section = g_sections[i];
#if TARGET_OS_IPHONE
            // Anonymous pages avoid sending repeated guest RAM writes to a temporary file.
            void* host = mmap(nullptr, static_cast<size_t>(section.size), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
#else
            section.fd = CreateSharedSection(section.size, static_cast<unsigned>(i));
            void* host = mmap(nullptr, static_cast<size_t>(section.size), PROT_READ | PROT_WRITE,
                              MAP_SHARED, section.fd, 0);
#endif
            if (host == MAP_FAILED)
                ThrowErrno("mmap host guest-memory alias");
            section.host = static_cast<uint8_t*>(host);
        }

        g_mappings.reserve(regions.size());
        for (const auto& region : regions) {
            auto section = std::find_if(g_sections.begin(), g_sections.end(),
                                        [&](const Section& value) { return SameKey(value, region); });
            const uint64_t offset = OffsetFor(region);
            const uint64_t mappedSize = RoundUp(region.size, pageSize);
#if TARGET_OS_IPHONE
            // copy=FALSE is essential: host and all guest mirrors must share writes.
            // OVERWRITE is restricted to the PROT_NONE reservation owned above.
            vm_address_t target = reinterpret_cast<vm_address_t>(g_base + region.base);
            vm_prot_t currentProtection = VM_PROT_NONE;
            vm_prot_t maximumProtection = VM_PROT_NONE;
            const kern_return_t result = vm_remap(
                mach_task_self(), &target, mappedSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                mach_task_self(), reinterpret_cast<vm_address_t>(section->host + offset),
                FALSE, &currentProtection, &maximumProtection, VM_INHERIT_NONE);
            if (result != KERN_SUCCESS)
                throw std::runtime_error(std::string("vm_remap guest-memory alias failed: ") +
                                         mach_error_string(result));
#else
            void* view = mmap(g_base + region.base, static_cast<size_t>(mappedSize),
                              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                              section->fd, static_cast<off_t>(offset));
            if (view == MAP_FAILED || view != g_base + region.base)
                ThrowErrno("mmap fixed guest-memory alias");
#endif
            g_mappings.push_back(Mapping{region.base, region.size, offset, &*section});
        }

        for (auto& section : g_sections) {
            close(section.fd);
            section.fd = -1;
        }
        g_layout = regions;
        g_initialized = true;
    } catch (...) {
        // A failed allocation/remap must not leave stale aliases or backing pages.
        munmap(g_base, static_cast<size_t>(reservationSize));
        for (auto& section : g_sections) {
            if (section.host)
                munmap(section.host, static_cast<size_t>(section.size));
            if (section.fd >= 0)
                close(section.fd);
        }
        g_mappings.clear();
        g_sections.clear();
        g_layout.clear();
        g_base = nullptr;
#if TARGET_OS_IPHONE
        gFlatGuestBase = nullptr;
#endif
        throw;
    }
}

uint8_t* HostPointer(uint32_t guestAddress)
{
    for (const auto& mapping : g_mappings) {
        if (guestAddress >= mapping.base &&
            static_cast<uint64_t>(guestAddress - mapping.base) < mapping.size)
            return mapping.section->host + mapping.offset + (guestAddress - mapping.base);
    }
    return nullptr;
}

void ProtectDeferredRange(uint32_t address, size_t length)
{
    SetProtection(address, length, PROT_NONE);
}

void UnprotectDeferredRange(uint32_t address, size_t length)
{
    SetProtection(address, length, PROT_READ | PROT_WRITE);
}

void RegisterExecutableRange(uint32_t, uint32_t)
{
    // Initial Apple bring-up keeps executable guest pages writable. Mod-code
    // invalidation still takes the checked page-table path.
}

FaultCounters Counters()
{
    return {g_mmio.load(), g_efb.load(), g_xguard.load(), g_unmapped.load(),
            g_unmappedRegions.load()};
}

void LogFaultSummary() noexcept
{
    const auto counters = Counters();
    if (counters.mmio || counters.efb || counters.xguard || counters.unmapped)
        std::cerr << "[runtime] flat guest fault summary: mmio=" << counters.mmio
                  << " efb=" << counters.efb << " xguard=" << counters.xguard
                  << " unmapped=" << counters.unmapped << std::endl;
}

bool HandleAccessViolation(void*) noexcept { return false; }

} // namespace GuestFlat
