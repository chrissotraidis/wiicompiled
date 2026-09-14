#pragma once
#include <SDL3/SDL_gamepad.h>
#include <algorithm>
#include <cstdint>

namespace kartpad::binding {
// Keep existing SDL button IDs and the legacy unbound sentinel unchanged.
inline constexpr uint32_t LeftTrigger = 0x10000;
inline constexpr uint32_t RightTrigger = 0x10001;
inline constexpr uint32_t Unbound = UINT32_MAX;
inline bool valid(uint32_t value) {
  return value < SDL_GAMEPAD_BUTTON_COUNT || value == LeftTrigger ||
         value == RightTrigger || value == Unbound;
}
inline bool pressed(SDL_Gamepad *pad, uint32_t value, int leftThreshold, int rightThreshold) {
  if (!pad) return false;
  if (value == LeftTrigger || value == RightTrigger) {
    const bool left = value == LeftTrigger;
    // Zero must not make a released trigger count as pressed.
    return SDL_GetGamepadAxis(pad, left ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
           >= std::clamp(left ? leftThreshold : rightThreshold, 1, 32767);
  }
  return value < SDL_GAMEPAD_BUTTON_COUNT && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(value));
}
}
