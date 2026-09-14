#include "fiber_manager.h"
#include "aurora_events.h"
#include "memory.h"
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "runtime_log.h"

// Defined in hle/os/os_sleep.cpp; the sleep-timer table is file-local there.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>

#if defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <TargetConditionals.h>
#include <ucontext.h>
#endif

namespace {
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
struct IOSFiberContext {
    uint64_t x19 = 0;
    uint64_t x20 = 0;
    uint64_t x21 = 0;
    uint64_t x22 = 0;
    uint64_t x23 = 0;
    uint64_t x24 = 0;
    uint64_t x25 = 0;
    uint64_t x26 = 0;
    uint64_t x27 = 0;
    uint64_t x28 = 0;
    uint64_t fp = 0;
    uint64_t lr = 0;
    uint64_t sp = 0;
    uint64_t reserved = 0;
    uint64_t d8 = 0;
    uint64_t d9 = 0;
    uint64_t d10 = 0;
    uint64_t d11 = 0;
    uint64_t d12 = 0;
    uint64_t d13 = 0;
    uint64_t d14 = 0;
    uint64_t d15 = 0;
};

static_assert(sizeof(IOSFiberContext) == 176);
extern "C" void KartPadSwitchIOSFiber(IOSFiberContext* source,
                                        const IOSFiberContext* target);

__asm__(
    ".text\n"
    ".align 2\n"
    ".globl _KartPadSwitchIOSFiber\n"
    "_KartPadSwitchIOSFiber:\n"
    "stp x19, x20, [x0, #0]\n"
    "stp x21, x22, [x0, #16]\n"
    "stp x23, x24, [x0, #32]\n"
    "stp x25, x26, [x0, #48]\n"
    "stp x27, x28, [x0, #64]\n"
    "stp x29, x30, [x0, #80]\n"
    "mov x9, sp\n"
    "str x9, [x0, #96]\n"
    "stp d8, d9, [x0, #112]\n"
    "stp d10, d11, [x0, #128]\n"
    "stp d12, d13, [x0, #144]\n"
    "stp d14, d15, [x0, #160]\n"
    "ldp x19, x20, [x1, #0]\n"
    "ldp x21, x22, [x1, #16]\n"
    "ldp x23, x24, [x1, #32]\n"
    "ldp x25, x26, [x1, #48]\n"
    "ldp x27, x28, [x1, #64]\n"
    "ldp x29, x30, [x1, #80]\n"
    "ldr x9, [x1, #96]\n"
    "mov sp, x9\n"
    "ldp d8, d9, [x1, #112]\n"
    "ldp d10, d11, [x1, #128]\n"
    "ldp d12, d13, [x1, #144]\n"
    "ldp d14, d15, [x1, #160]\n"
    "mov x0, x19\n"
    "ret\n");
#endif

struct AppleFiber {
#if TARGET_OS_IPHONE
    IOSFiberContext iosContext{};
#endif
    ucontext_t context{};
    std::unique_ptr<std::byte[]> stack;
};

thread_local AppleFiber* g_currentAppleFiber = nullptr;

void* CreateAppleSchedulerFiber()
{
    auto fiber = std::make_unique<AppleFiber>();
#if !TARGET_OS_IPHONE
    if (getcontext(&fiber->context) != 0) {
        return nullptr;
    }
#endif
    g_currentAppleFiber = fiber.get();
    return fiber.release();
}

void SwitchHostFiber(void* handle)
{
    auto* target = static_cast<AppleFiber*>(handle);
    AppleFiber* source = g_currentAppleFiber;
    if (!source || !target || source == target) {
        return;
    }

    g_currentAppleFiber = target;
#if TARGET_OS_IPHONE
    KartPadSwitchIOSFiber(&source->iosContext, &target->iosContext);
#else
    if (swapcontext(&source->context, &target->context) != 0) {
        const int error = errno;
        g_currentAppleFiber = source;
        RT_LOG(RT_TAG_OS) << "FATAL: swapcontext failed: " << std::strerror(error) << std::endl;
        std::abort();
    }
#endif
    g_currentAppleFiber = source;
}

void* CurrentHostFiber()
{
    return g_currentAppleFiber;
}

void DeleteHostFiber(void* handle)
{
    delete static_cast<AppleFiber*>(handle);
}
#elif defined(__ANDROID__)
struct AndroidFiberContext {
    uint64_t x19 = 0;
    uint64_t x20 = 0;
    uint64_t x21 = 0;
    uint64_t x22 = 0;
    uint64_t x23 = 0;
    uint64_t x24 = 0;
    uint64_t x25 = 0;
    uint64_t x26 = 0;
    uint64_t x27 = 0;
    uint64_t x28 = 0;
    uint64_t fp = 0;
    uint64_t lr = 0;
    uint64_t sp = 0;
    uint32_t fpcr = 0;
    uint32_t fpsr = 0;
    uint64_t d8 = 0;
    uint64_t d9 = 0;
    uint64_t d10 = 0;
    uint64_t d11 = 0;
    uint64_t d12 = 0;
    uint64_t d13 = 0;
    uint64_t d14 = 0;
    uint64_t d15 = 0;
};

static_assert(sizeof(AndroidFiberContext) == 176);
extern "C" void KartPadSwitchAndroidRuntimeFiber(AndroidFiberContext* source,
                                                   const AndroidFiberContext* target);

struct AndroidFiber {
    AndroidFiberContext context{};
    std::unique_ptr<std::byte[]> stack;
};

thread_local AndroidFiber* g_currentAndroidFiber = nullptr;

void* CreateAndroidSchedulerFiber()
{
    auto fiber = std::make_unique<AndroidFiber>();
    g_currentAndroidFiber = fiber.get();
    return fiber.release();
}

void SwitchHostFiber(void* handle)
{
    auto* target = static_cast<AndroidFiber*>(handle);
    AndroidFiber* source = g_currentAndroidFiber;
    if (!source || !target || source == target) {
        return;
    }

    g_currentAndroidFiber = target;
    KartPadSwitchAndroidRuntimeFiber(&source->context, &target->context);
    g_currentAndroidFiber = source;
}

void* CurrentHostFiber()
{
    return g_currentAndroidFiber;
}

void DeleteHostFiber(void* handle)
{
    delete static_cast<AndroidFiber*>(handle);
}
#elif defined(_WIN32)
void SwitchHostFiber(void* handle) { SwitchToFiber(handle); }
void* CurrentHostFiber() { return GetCurrentFiber(); }
void DeleteHostFiber(void* handle) { DeleteFiber(handle); }
#endif
} // namespace

namespace Fiber {

std::mutex GuestFiberManager::s_mutex;
std::unordered_map<uint32_t, GuestFiber> GuestFiberManager::s_fibers;
std::vector<void*> GuestFiberManager::s_fibersPendingDelete;
void* GuestFiberManager::s_schedulerFiber = nullptr;
uint32_t GuestFiberManager::s_currentGuestThread = 0;
bool GuestFiberManager::s_initialized = false;
thread_local CpuContext* GuestFiberManager::s_cpuContext = nullptr;

void GuestFiberManager::PurgePendingFibers() {
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    std::vector<void*> toDelete;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        toDelete.swap(s_fibersPendingDelete);
    }
    const void* current = CurrentHostFiber();
    for (void* f : toDelete) {
        if (f && f != current) {
            DeleteHostFiber(f);
        }
    }
#endif
}

// Global VI retrace counter
std::atomic<uint32_t> g_viRetracePendingCount{0};

// =============================================================================
// Guest OS memory layout constants
// =============================================================================
namespace {
constexpr uint32_t kOSCurrentContextAddr = 0x800000d4u;
constexpr uint32_t kOSRunningContextAddr = 0x800000e4u;  // OSGetCurrentThread reads this
constexpr uint32_t kThreadQueueArrayAddr = 0x803477b0u;
constexpr uint32_t kSchedulerPendingFlagAddr = 0x80386920u;
constexpr uint32_t kSchedulerReschedCounterAddr = 0x8038691cu;
constexpr uint32_t kSchedulerIdleFlagAddr = 0x80386918u;

// OSThread structure offsets
constexpr uint32_t kThreadStateOffset = 0x2C8u;
constexpr uint32_t kThreadAttrOffset = 0x2CAu;
constexpr uint32_t kThreadSuspendOffset = 0x2CCu;
constexpr uint32_t kThreadPriorityOffset = 0x2D0u;
constexpr uint32_t kThreadExitValueOffset = 0x2D8u;
constexpr uint32_t kThreadNextOffset = 0x2E0u;
constexpr uint32_t kThreadPrevOffset = 0x2E4u;
constexpr uint32_t kThreadQueueOffset = 0x2DCu;
constexpr uint32_t kThreadJoinQueueOffset = 0x2E8u;

// OSContext offsets (for saving/loading fiber context)
constexpr uint32_t kCtxGprOffset = 0x00u;
constexpr uint32_t kCtxCrOffset = 0x80u;
constexpr uint32_t kCtxLrOffset = 0x84u;
constexpr uint32_t kCtxCtrOffset = 0x88u;
constexpr uint32_t kCtxXerOffset = 0x8Cu;
constexpr uint32_t kCtxSrr0Offset = 0x198u;
constexpr uint32_t kCtxSrr1Offset = 0x19Cu;
constexpr uint32_t kCtxGqrOffset = 0x1A8u;

void ClearPendingMaskForEmptyGuestQueue(uint32_t queueEntry)
{
    if (queueEntry < kThreadQueueArrayAddr ||
        ((queueEntry - kThreadQueueArrayAddr) % 8u) != 0) {
        return;
    }

    const uint32_t priority = (queueEntry - kThreadQueueArrayAddr) / 8u;
    if (priority >= 32 || Memory::Read32(queueEntry) != 0) {
        return;
    }

    const uint32_t pending = Memory::Read32(kSchedulerPendingFlagAddr);
    Memory::Write32(kSchedulerPendingFlagAddr, pending & ~(1u << (31u - priority)));
}

void RemoveGuestThreadFromQueue(uint32_t threadPtr)
{
    const uint32_t queuePtr = Memory::Read32(threadPtr + kThreadQueueOffset);
    if (queuePtr == 0) {
        return;
    }

    const uint32_t next = Memory::Read32(threadPtr + kThreadNextOffset);
    const uint32_t prev = Memory::Read32(threadPtr + kThreadPrevOffset);

    if (next != 0) {
        Memory::Write32(next + kThreadPrevOffset, prev);
    } else {
        Memory::Write32(queuePtr + 4u, prev);
    }

    if (prev != 0) {
        Memory::Write32(prev + kThreadNextOffset, next);
    } else {
        Memory::Write32(queuePtr, next);
    }

    Memory::Write32(threadPtr + kThreadQueueOffset, 0);
    Memory::Write32(threadPtr + kThreadNextOffset, 0);
    Memory::Write32(threadPtr + kThreadPrevOffset, 0);
    ClearPendingMaskForEmptyGuestQueue(queuePtr);
}

void WakeGuestThreadsOnQueueNoSwitch(uint32_t queueAddr)
{
    constexpr int kMaxWake = 256;

    for (int woke = 0; woke < kMaxWake; ++woke) {
        const uint32_t thread = Memory::Read32(queueAddr);
        if (thread == 0) {
            return;
        }

        const uint32_t next = Memory::Read32(thread + kThreadNextOffset);
        if (next == 0) {
            Memory::Write32(queueAddr + 4u, 0);
        } else {
            Memory::Write32(next + kThreadPrevOffset, 0);
        }
        Memory::Write32(queueAddr, next);
        Memory::Write32(thread + kThreadNextOffset, 0);
        Memory::Write32(thread + kThreadPrevOffset, 0);

        const uint16_t state = Memory::Read16(thread + kThreadStateOffset);
        if (state == 0 || state == 8) {
            Memory::Write32(thread + kThreadQueueOffset, 0);
            continue;
        }

        Memory::Write16(thread + kThreadStateOffset, 1);
        const int32_t suspend = static_cast<int32_t>(Memory::Read32(thread + kThreadSuspendOffset));
        if (suspend >= 1) {
            Memory::Write32(thread + kThreadQueueOffset, 0);
            continue;
        }

        int32_t priority = static_cast<int32_t>(Memory::Read32(thread + kThreadPriorityOffset));
        priority = std::clamp(priority, 0, 31);

        const uint32_t runQueue = kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
        const uint32_t tail = Memory::Read32(runQueue + 4u);
        if (tail == 0) {
            Memory::Write32(runQueue, thread);
        } else {
            Memory::Write32(tail + kThreadNextOffset, thread);
        }
        Memory::Write32(thread + kThreadPrevOffset, tail);
        Memory::Write32(thread + kThreadNextOffset, 0);
        Memory::Write32(runQueue + 4u, thread);
        Memory::Write32(thread + kThreadQueueOffset, runQueue);

        const uint32_t pending = Memory::Read32(kSchedulerPendingFlagAddr);
        Memory::Write32(kSchedulerPendingFlagAddr, pending | (1u << (31u - static_cast<uint32_t>(priority))));
        Memory::Write32(kSchedulerReschedCounterAddr, 1);

        GuestFiberManager::ResumeGuestThread(thread);
    }

    RT_LOG(RT_TAG_OS) << "WakeGuestThreadsOnQueueNoSwitch: hit safety limit at 0x"
              << std::hex << queueAddr << std::dec << std::endl;
}
} // namespace

// =============================================================================
// GuestFiberManager Implementation
// =============================================================================

void GuestFiberManager::Initialize() {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (s_initialized) {
        return;
    }
    
#if defined(_WIN32)
    // Convert the main thread to a fiber (the scheduler fiber)
    s_schedulerFiber = ConvertThreadToFiber(nullptr);
    if (!s_schedulerFiber) {
        // May already be a fiber
        s_schedulerFiber = GetCurrentFiber();
        if (!s_schedulerFiber) {
            RT_LOG(RT_TAG_OS) << "FATAL: Failed to initialize scheduler fiber!" << std::endl;
            ShowRuntimeFatalPopup("guest scheduler initialization failed",
                                  "Windows could not create the scheduler fiber required to run guest threads.");
            std::abort();
        }
    }
    
#elif defined(__APPLE__)
    s_schedulerFiber = CreateAppleSchedulerFiber();
    if (!s_schedulerFiber) {
        RT_LOG(RT_TAG_OS) << "FATAL: Failed to initialize macOS scheduler fiber: "
                          << std::strerror(errno) << std::endl;
        std::abort();
    }
#elif defined(__ANDROID__)
    s_schedulerFiber = CreateAndroidSchedulerFiber();
    if (!s_schedulerFiber) {
        RT_LOG(RT_TAG_OS) << "FATAL: Failed to initialize Android scheduler fiber!" << std::endl;
        std::abort();
    }
#else
    RT_LOG(RT_TAG_OS) << "WARNING: Fiber support not available on this platform!" << std::endl;
    s_schedulerFiber = nullptr;
#endif
    
    s_currentGuestThread = 0;
    s_initialized = true;
}

void GuestFiberManager::Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);

#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    for (auto& [addr, fiber] : s_fibers) {
        if (fiber.fiber && !fiber.isSchedulerFiber) {
            DeleteHostFiber(fiber.fiber);
            fiber.fiber = nullptr;
        }
    }
    s_fibers.clear();
    
#if defined(_WIN32)
    // Convert scheduler fiber back to thread
    if (s_schedulerFiber) {
        ConvertFiberToThread();
        s_schedulerFiber = nullptr;
    }
#elif defined(__APPLE__)
    if (s_schedulerFiber) {
        DeleteHostFiber(s_schedulerFiber);
        s_schedulerFiber = nullptr;
        g_currentAppleFiber = nullptr;
    }
#else
    if (s_schedulerFiber) {
        DeleteHostFiber(s_schedulerFiber);
        s_schedulerFiber = nullptr;
        g_currentAndroidFiber = nullptr;
    }
#endif
#endif
    
    s_initialized = false;
}

bool GuestFiberManager::IsInitialized() {
    return s_initialized;
}

bool GuestFiberManager::IsOnSchedulerFiber() {
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    return !s_initialized || CurrentHostFiber() == s_schedulerFiber;
#else
    return true;
#endif
}

bool GuestFiberManager::CreateGuestFiber(uint32_t guestThreadAddr, uint32_t entryPoint,
                                          uint32_t entryArg, uint32_t stackBase) {
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "CreateGuestFiber called before initialization!" << std::endl;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Check if fiber already exists for this thread - if so, reset it
    auto existingIt = s_fibers.find(guestThreadAddr);
    if (existingIt != s_fibers.end()) {
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
        // Delete the old fiber if it exists and is not the scheduler fiber
        if (existingIt->second.fiber && !existingIt->second.isSchedulerFiber) {
            DeleteHostFiber(existingIt->second.fiber);
        }
#endif
        s_fibers.erase(existingIt);
    }
    
    GuestFiber gf;
    gf.entryPoint = entryPoint;
    gf.entryArg = entryArg;
    gf.state = ThreadState::WAITING; // Starts suspended
    gf.terminated = false;
    gf.isSchedulerFiber = false;
    
    // Initialize CPU context with entry point info
    std::memset(&gf.cpuContext, 0, sizeof(CpuContext));
    gf.cpuContext.gpr[1] = stackBase; // Stack pointer
    gf.cpuContext.gpr[3] = entryArg;  // First argument
    gf.cpuContext.lr = 0;             // No return address
    gf.cpuContext.pc = entryPoint;
    gf.cpuContext.srr0 = entryPoint;
    
#if defined(_WIN32)
    // Create Windows fiber with reasonable stack size
    // Use host stack size (64KB should be plenty for translated code)
    constexpr size_t kHostStackSize = 64 * 1024;
    gf.fiber = CreateFiber(kHostStackSize, FiberProc, reinterpret_cast<void*>(static_cast<uintptr_t>(guestThreadAddr)));
    
    if (!gf.fiber) {
        DWORD err = GetLastError();
        RT_LOG(RT_TAG_OS) << "CreateFiber failed for thread 0x"
                  << std::hex << guestThreadAddr 
                  << " error=" << std::dec << err << std::endl;
        return false;
    }
#elif defined(__APPLE__)
    constexpr size_t kHostStackSize = 256 * 1024;
    auto appleFiber = std::make_unique<AppleFiber>();
    appleFiber->stack = std::make_unique<std::byte[]>(kHostStackSize);
#if TARGET_OS_IPHONE
    auto trampoline = +[](uint32_t threadAddr) {
        GuestFiberManager::FiberProc(
            reinterpret_cast<void*>(static_cast<uintptr_t>(threadAddr)));
        std::abort();
    };
    const uintptr_t stackTop =
        (reinterpret_cast<uintptr_t>(appleFiber->stack.get()) + kHostStackSize) &
        ~static_cast<uintptr_t>(0x0f);
    appleFiber->iosContext.x19 = guestThreadAddr;
    appleFiber->iosContext.sp = stackTop;
    appleFiber->iosContext.lr = reinterpret_cast<uintptr_t>(trampoline);
#else
    if (getcontext(&appleFiber->context) != 0) {
        RT_LOG(RT_TAG_OS) << "getcontext failed for thread 0x" << std::hex << guestThreadAddr
                          << ": " << std::strerror(errno) << std::dec << std::endl;
        return false;
    }
    appleFiber->context.uc_stack.ss_sp = appleFiber->stack.get();
    appleFiber->context.uc_stack.ss_size = kHostStackSize;
    appleFiber->context.uc_stack.ss_flags = 0;
    appleFiber->context.uc_link = &static_cast<AppleFiber*>(s_schedulerFiber)->context;
    auto trampoline = +[](uint32_t threadAddr) {
        GuestFiberManager::FiberProc(
            reinterpret_cast<void*>(static_cast<uintptr_t>(threadAddr)));
    };
    makecontext(&appleFiber->context,
                reinterpret_cast<void (*)()>(trampoline), 1, guestThreadAddr);
#endif
    gf.fiber = appleFiber.release();
#elif defined(__ANDROID__)
    constexpr size_t kHostStackSize = 256 * 1024;
    auto androidFiber = std::make_unique<AndroidFiber>();
    androidFiber->stack = std::make_unique<std::byte[]>(kHostStackSize);
    auto trampoline = +[](uint32_t threadAddr) {
        GuestFiberManager::FiberProc(
            reinterpret_cast<void*>(static_cast<uintptr_t>(threadAddr)));
        std::abort();
    };
    const uintptr_t stackTop =
        (reinterpret_cast<uintptr_t>(androidFiber->stack.get()) + kHostStackSize) &
        ~static_cast<uintptr_t>(0x0f);
    androidFiber->context.x19 = guestThreadAddr;
    androidFiber->context.sp = stackTop;
    androidFiber->context.lr = reinterpret_cast<uintptr_t>(trampoline);
    gf.fiber = androidFiber.release();
#else
    gf.fiber = nullptr;
#endif
    
    s_fibers[guestThreadAddr] = gf;
    
    
    return true;
}

void GuestFiberManager::ResumeGuestThread(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        RT_LOG(RT_TAG_OS) << "ResumeGuestThread: no fiber for 0x"
                  << std::hex << guestThreadAddr << std::dec << std::endl;
        return;
    }
    
    if (it->second.terminated) {
        RT_LOG(RT_TAG_OS) << "ResumeGuestThread: thread 0x"
                  << std::hex << guestThreadAddr << " already terminated" << std::dec << std::endl;
        return;
    }
    
    it->second.state = ThreadState::READY;

}

void GuestFiberManager::SuspendGuestThread(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        return;
    }
    
    it->second.state = ThreadState::WAITING;
    
}

void GuestFiberManager::ExitGuestThread(uint32_t guestThreadAddr, ThreadState finalState) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        return;
    }
    
    it->second.state = finalState;
    it->second.terminated = true;
    if (s_currentGuestThread == guestThreadAddr) {
        s_currentGuestThread = 0;
    }
    
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    if (it->second.fiber && !it->second.isSchedulerFiber) {
        const void* current = CurrentHostFiber();
        if (it->second.fiber == current) {
            s_fibersPendingDelete.push_back(it->second.fiber);
        } else {
            DeleteHostFiber(it->second.fiber);
        }
        it->second.fiber = nullptr;
    }
#endif
}

void GuestFiberManager::SwitchToThread(uint32_t guestThreadAddr, CpuContext* cpu) {
    PurgePendingFibers();
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "SwitchToThread called before initialization!" << std::endl;
        return;
    }
    
    void* fiberHandle = nullptr;
    uint32_t previousThread = 0;
    CpuContext callerContext{};
    const bool haveCallerContext = (cpu != nullptr);
    CpuContext targetContext{};
    bool haveTargetContext = false;

    if (cpu) {
        callerContext = *cpu;
    }
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        
        auto it = s_fibers.find(guestThreadAddr);
        if (it == s_fibers.end()) {
            RT_LOG(RT_TAG_OS) << "SwitchToThread: no fiber for 0x"
                      << std::hex << guestThreadAddr << std::dec << std::endl;
            return;
        }
        
        fiberHandle = it->second.fiber;
        
        if (!fiberHandle || it->second.terminated) {
            RT_LOG(RT_TAG_OS) << "SwitchToThread: invalid fiber for 0x"
                      << std::hex << guestThreadAddr << std::dec << std::endl;
            return;
        }
        
        // Remember what thread we're switching from
        previousThread = s_currentGuestThread;
        
        // Save current thread's CPU context if switching from a guest thread
        if (s_currentGuestThread != 0 && cpu) {
            auto currentIt = s_fibers.find(s_currentGuestThread);
            if (currentIt != s_fibers.end()) {
                currentIt->second.cpuContext = *cpu;
            }
        }
        
        // Update current thread
        s_currentGuestThread = guestThreadAddr;
        it->second.state = ThreadState::RUNNING;

        if (cpu) {
            targetContext = it->second.cpuContext;
            haveTargetContext = true;
        }
    }
    
    // Store CPU context pointer for the target fiber to use
    s_cpuContext = cpu;
    
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    // Check if we're already on the target fiber (e.g., switching to main thread
    // when we're already on the scheduler fiber)
    void* currentFiber = CurrentHostFiber();
    if (currentFiber == fiberHandle) {
        // Already executing on the target host fiber. This is common for the
        // default guest thread, which also owns the scheduler fiber. Keep the
        // live CPU context instead of restoring a possibly stale saved copy
        // from before the guest thread slept.
        return;
    }
    
    if (cpu && haveTargetContext) {
        *cpu = targetContext;
        // FPSCR travels with the guest-thread context, and its NI bit is
        // modeled through the per-host-thread MXCSR; every context restore
        // must re-mirror it.
        MkwApplyHostNiMode(cpu->fpscr);
    }

    // Switch to the target fiber (the target fiber will load its own context)
    SwitchHostFiber(fiberHandle);
#if defined(__ANDROID__)
    // A guest fiber may have deferred Java-backed SDL work to this stack.
    ServicePendingAuroraEventsOnScheduler();
#endif
    
    // When we return here, the fiber that issued SwitchToThread has resumed.
    // That does not automatically mean the previous guest thread became runnable
    // again; a different thread may simply have yielded back to the scheduler.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        uint32_t runningContext = 0;
        uint32_t currentContext = 0;
        try {
            runningContext = Memory::Read32(kOSRunningContextAddr);
            currentContext = Memory::Read32(kOSCurrentContextAddr);
        } catch (const Memory::AccessViolation&) {
            runningContext = 0;
            currentContext = 0;
        }

        const bool resumedPreviousThread =
            previousThread != 0 &&
            runningContext == previousThread &&
            currentContext == previousThread;

        if (resumedPreviousThread) {
            auto it = s_fibers.find(previousThread);
            if (it != s_fibers.end()) {
                if (cpu) {
                    *cpu = it->second.cpuContext;
                    MkwApplyHostNiMode(cpu->fpscr);
                }
                it->second.state = ThreadState::RUNNING;
            }
            s_currentGuestThread = previousThread;
        } else {
            if (cpu && haveCallerContext) {
                *cpu = callerContext;
                MkwApplyHostNiMode(cpu->fpscr);
            }
            s_currentGuestThread = 0;
        }
    }
#endif
}

uint32_t GuestFiberManager::GetCurrentGuestThread() {
    return s_currentGuestThread;
}

GuestFiber* GuestFiberManager::GetFiber(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    return it != s_fibers.end() ? &it->second : nullptr;
}

bool GuestFiberManager::HasFiber(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_fibers.count(guestThreadAddr) > 0;
}

bool GuestFiberManager::IsTerminated(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    return it != s_fibers.end() && it->second.terminated;
}

bool GuestFiberManager::RegisterMainThreadAsFiber(uint32_t guestThreadAddr, CpuContext* cpu) {
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "RegisterMainThreadAsFiber called before initialization!" << std::endl;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Check if already registered
    if (s_fibers.count(guestThreadAddr)) {
        return true;
    }
    
    // Register the current host fiber (scheduler fiber) as this guest thread's fiber
    GuestFiber gf;
    gf.fiber = s_schedulerFiber;  // The main fiber IS the scheduler fiber
    gf.entryPoint = 0;
    gf.entryArg = 0;
    gf.state = ThreadState::RUNNING;
    gf.terminated = false;
    gf.isSchedulerFiber = true;  // This is special - it's both scheduler AND main thread
    
    // Copy current CPU context
    if (cpu) {
        gf.cpuContext = *cpu;
    }
    
    s_fibers[guestThreadAddr] = gf;
    s_currentGuestThread = guestThreadAddr;
    
    return true;
}

void GuestFiberManager::ProcessTimerEvents(CpuContext* cpu) {
    constexpr uint32_t kMaxRetracesPerSlice = 16;
    const uint32_t pending = g_viRetracePendingCount.exchange(0, std::memory_order_acq_rel);
    if (pending == 0) {
        return;
    }

    const uint32_t retracesToProcess = std::min(pending, kMaxRetracesPerSlice);
    const uint32_t remaining = pending - retracesToProcess;
    if (remaining != 0) {
        g_viRetracePendingCount.fetch_add(remaining, std::memory_order_release);
    }

    for (uint32_t i = 0; i < retracesToProcess; ++i) {
        VI_HLE_ForceRetrace(cpu);
    }
}

#if defined(_WIN32)
void CALLBACK GuestFiberManager::FiberProc(void* param)
#else
void GuestFiberManager::FiberProc(void* param)
#endif
{
    uint32_t guestThreadAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
    
    
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    // Get our fiber info
    GuestFiber* fiber = nullptr;
    uint32_t entryPoint = 0;
    uint32_t entryArg = 0;
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_fibers.find(guestThreadAddr);
        if (it == s_fibers.end()) {
            RT_LOG(RT_TAG_OS) << "FiberProc: fiber not found!" << std::endl;
            SwitchHostFiber(s_schedulerFiber);
            return;
        }
        fiber = &it->second;
        entryPoint = fiber->entryPoint;
        entryArg = fiber->entryArg;
    }
    
    // Get the CPU context
    CpuContext* cpu = s_cpuContext;
    if (!cpu) {
        cpu = &GetPersistentCpuContext();
    }
    
    // Guest OSContext: r2 (TOC/SDA2) at 0x08, r13 (SDA) at 0x34. Both must load correctly or
    // translated code loses access to global/static data.
    try {
        // Load r2 (TOC/SDA2 pointer)
        cpu->gpr[2] = Memory::Read32(guestThreadAddr + 0x08u);
        // Load r13 (SDA pointer) - this is CRITICAL for global data access
        cpu->gpr[13] = Memory::Read32(guestThreadAddr + 0x34u);
        // Load stack pointer from guest context
        cpu->gpr[1] = Memory::Read32(guestThreadAddr + 0x04u);
        // Load saved LR
        cpu->lr = Memory::Read32(guestThreadAddr + 0x84u);
        
    } catch (const Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "Failed to load guest context from 0x" << std::hex << guestThreadAddr
                  << ": " << e.what() << std::dec << std::endl;
    }
    
    // Set up context for thread entry
    cpu->gpr[3] = entryArg;
    cpu->pc = entryPoint;
    cpu->srr0 = entryPoint;
    
    // Create a CpuContextScope for this fiber
    CpuContextScope scope(cpu);
    
    int startDeferAttempts = 0;
    while (entryPoint == 0x8024373c) { // EGG::Thread::start
        uint32_t vtable = 0;
        uint32_t startFn = 0;
        try {
            vtable = Memory::Read32(entryArg);
            if (vtable >= 0x80000000u) {
                startFn = Memory::Read32(vtable + 0x0Cu);
            }
        } catch (const Memory::AccessViolation&) {
            vtable = 0;
            startFn = 0;
        }


        if (vtable >= 0x80000000u && startFn >= 0x80000000u) {
            break;
        }

        if (startDeferAttempts++ > 50) {
            RT_LOG(RT_TAG_OS) << "EGG::Thread::start target still invalid (vtable=0x" << std::hex << vtable
                      << ", fn=0x" << startFn << ") after retries; continuing anyway." << std::dec << std::endl;
            break;
        }
        SwitchHostFiber(s_schedulerFiber);
    }

    // The deferral loop above yields to the scheduler and therefore can resume
    // with registers from a different guest fiber in the shared CpuContext.
    cpu->gpr[3] = entryArg;
    cpu->pc = entryPoint;
    cpu->srr0 = entryPoint;

    
    // Call the translated thread entry function
    const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(entryPoint);
    if (info) {
        InvokeIndirectCpu(entryPoint, cpu);
    } else {
        RT_LOG(RT_TAG_OS) << "Thread entry 0x" << std::hex << entryPoint
                  << " not found in registry!" << std::dec << std::endl;
    }
    
    // Thread entry functions normally return into OSExitThread on hardware.
    // Our host fiber call boundary observes the return directly, so complete the
    // guest OSThread lifecycle here before handing control back to the scheduler.

    try {
        RemoveGuestThreadFromQueue(guestThreadAddr);
        const uint16_t attributes = Memory::Read16(guestThreadAddr + kThreadAttrOffset);
        const bool detached = (attributes & 1u) != 0;
        const uint16_t finalState = detached ? 0u : static_cast<uint16_t>(ThreadState::MORIBUND);
        if (!detached) {
            Memory::Write32(guestThreadAddr + kThreadExitValueOffset, 0);
        }
        Memory::Write16(guestThreadAddr + kThreadStateOffset, finalState);
        WakeGuestThreadsOnQueueNoSwitch(guestThreadAddr + kThreadJoinQueueOffset);
        if (Memory::Read32(kOSRunningContextAddr) == guestThreadAddr) {
            Memory::Write32(kOSRunningContextAddr, 0);
        }
        if (Memory::Read32(kOSCurrentContextAddr) == guestThreadAddr) {
            Memory::Write32(kOSCurrentContextAddr, 0);
        }
        Memory::Write32(kSchedulerReschedCounterAddr, 1);
    } catch (const Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "Thread return cleanup failed for 0x" << std::hex
                  << guestThreadAddr << " at 0x" << e.address() << std::dec
                  << " (" << e.reason() << ")" << std::endl;
    }
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_fibers.find(guestThreadAddr);
        if (it != s_fibers.end()) {
            it->second.terminated = true;
            it->second.state = ThreadState::MORIBUND;
        }
        s_currentGuestThread = 0;
    }
    
    // Return to scheduler
    SwitchHostFiber(s_schedulerFiber);
#else
    (void)guestThreadAddr;
    RT_LOG(RT_TAG_OS) << "Fibers not supported on this platform!" << std::endl;
#endif
}

} // namespace Fiber
