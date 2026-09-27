#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_bindings.h"
#include "wii_remote_input.h"
#include "wup028_adapter.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
#include <chrono>
#include <string>
#include <vector>
#endif

namespace {

std::atomic<bool> g_rumbleEnabled{true};

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

#if defined(__APPLE__) && TARGET_OS_SIMULATOR
// Simulator-only scripted input for automated checks; never compiled into device builds.
// KARTPAD_SIM_INPUT="A@5000,DOWN@7000,A@7500+20000" presses a GameCube button on port 0 at a
// time in ms after the first PAD read, held 150 ms or for the given +duration. Pass it with
// SIMCTL_CHILD_KARTPAD_SIM_INPUT=... xcrun simctl launch.
struct SimPress {
    uint16_t button;
    uint64_t at;
    uint64_t until;
};

uint16_t SimButton(const std::string& name) {
    static const std::pair<const char*, uint16_t> kNames[] = {
        {"A", PAD_BUTTON_A}, {"B", PAD_BUTTON_B}, {"X", PAD_BUTTON_X}, {"Y", PAD_BUTTON_Y},
        {"START", PAD_BUTTON_START}, {"Z", PAD_TRIGGER_Z}, {"L", PAD_TRIGGER_L}, {"R", PAD_TRIGGER_R},
        {"UP", PAD_BUTTON_UP}, {"DOWN", PAD_BUTTON_DOWN}, {"LEFT", PAD_BUTTON_LEFT}, {"RIGHT", PAD_BUTTON_RIGHT},
    };
    for (const auto& [key, value] : kNames) {
        if (name == key) return value;
    }
    return 0;
}

std::vector<SimPress> ParseSimInput() {
    std::vector<SimPress> presses;
    const char* script = std::getenv("KARTPAD_SIM_INPUT");
    if (script == nullptr) return presses;
    std::string all(script);
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find(',', start);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(start, end - start);
        start = end + 1;
        const size_t atPos = item.find('@');
        if (atPos == std::string::npos) continue;
        const uint16_t button = SimButton(item.substr(0, atPos));
        const size_t plus = item.find('+', atPos);
        const uint64_t at = std::strtoull(item.c_str() + atPos + 1, nullptr, 10);
        const uint64_t hold = plus == std::string::npos ? 150 : std::strtoull(item.c_str() + plus + 1, nullptr, 10);
        if (button != 0) presses.push_back({button, at, at + hold});
    }
    std::fprintf(stderr, "[input] simulator script with %zu presses\n", presses.size());
    return presses;
}

void ApplySimInput(PADStatus& status) {
    static const std::vector<SimPress> presses = ParseSimInput();
    if (presses.empty()) return;
    static const auto origin = std::chrono::steady_clock::now();
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count());
    for (const auto& press : presses) {
        if (now >= press.at && now < press.until) status.button |= press.button;
    }
    status.err = PAD_ERR_NONE;
}
#endif

void ConfigureDefaultKeyboardPort() {
    constexpr std::array buttonBindings{
        PADKeyButtonBinding{SDL_SCANCODE_RETURN, PAD_BUTTON_A},
        PADKeyButtonBinding{SDL_SCANCODE_BACKSPACE, PAD_BUTTON_B},
        PADKeyButtonBinding{SDL_SCANCODE_Q, PAD_BUTTON_X},
        PADKeyButtonBinding{SDL_SCANCODE_E, PAD_BUTTON_Y},
        PADKeyButtonBinding{SDL_SCANCODE_SPACE, PAD_BUTTON_START},
        PADKeyButtonBinding{SDL_SCANCODE_LSHIFT, PAD_TRIGGER_Z},
        PADKeyButtonBinding{SDL_SCANCODE_LCTRL, PAD_TRIGGER_L},
        PADKeyButtonBinding{SDL_SCANCODE_LALT, PAD_TRIGGER_R},
        PADKeyButtonBinding{SDL_SCANCODE_UP, PAD_BUTTON_UP},
        PADKeyButtonBinding{SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
        PADKeyButtonBinding{SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT},
        PADKeyButtonBinding{SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
    };
    constexpr std::array axisBindings{
        PADKeyAxisBinding{SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 0},
        PADKeyAxisBinding{SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 0},
        PADKeyAxisBinding{SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 0},
        PADKeyAxisBinding{SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 0},
        PADKeyAxisBinding{SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 0},
        PADKeyAxisBinding{SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 0},
        PADKeyAxisBinding{SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 0},
        PADKeyAxisBinding{SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 0},
        PADKeyAxisBinding{SDL_SCANCODE_LCTRL, PAD_AXIS_TRIGGER_L, 0},
        PADKeyAxisBinding{SDL_SCANCODE_LALT, PAD_AXIS_TRIGGER_R, 0},
    };

    for (const auto& binding : buttonBindings) {
        PADSetKeyButtonBinding(0, binding);
    }
    for (const auto& binding : axisBindings) {
        PADSetKeyAxisBinding(0, binding);
    }
    PADSetKeyboardActive(0, TRUE);
}

} // namespace

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

extern "C" uint32_t PAD__Init_HLE()
{
    Wup028Adapter::Initialize();
    if (!PADInit()) {
        return 0;
    }
    ConfigureDefaultKeyboardPort();
    return 1;
}
PPC_NATIVE_OVERRIDE(801AF2F0, PAD__Init_HLE, uint32_t, (), ());

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory.
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    // Keep looking for a Bluetooth Wii Remote that dropped out (or was turned on late).
    WiiRemoteInput::Poll();
    uint32_t rumbleMask = PADRead(statuses);
    // Wii Remotes reach the game through KPAD, not as GameCube pads. This also
    // applies while input is blocked (overlay open) so the port does not flip
    // between "connected" and "no controller" every time the overlay toggles.
    WiiRemoteInput::HideRemotesFromPad(statuses, PAD_CHANMAX);

    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
    ApplySimInput(statuses[0]);
#endif

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
PPC_NATIVE_OVERRIDE(801AF44C, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF0DC, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF1E4, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    PADControlMotor(chan, command);
}
PPC_NATIVE_OVERRIDE_VOID(801AF908, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
