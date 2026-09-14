#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "wup028_adapter.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_scancode.h>
#include <dolphin/pad.h>

namespace {

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

extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    std::array<PADStatus, PAD_CHANMAX> adapterStatuses{};
    uint32_t rumbleMask = PADRead(statuses);
    if (Wup028Adapter::Read(adapterStatuses) && !PADIsInputBlocked()) {
        for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
            if (adapterStatuses[port].err == PAD_ERR_NONE) {
                statuses[port] = adapterStatuses[port];
                rumbleMask |= PAD_CHAN0_BIT >> port;
            }
        }
    }

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
    if (!Wup028Adapter::SetRumble(static_cast<uint32_t>(chan), command == PAD_MOTOR_RUMBLE)) {
        PADControlMotor(chan, command);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801AF908, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
