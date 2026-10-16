#pragma once

#include <array>
#include <SDL3/SDL_scancode.h>
#include <dolphin/pad.h>

// Shared by game startup and the native keyboard settings reset.
namespace KartPadKeyboard {

inline void ConfigureDefaultPort() {
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

} // namespace KartPadKeyboard
