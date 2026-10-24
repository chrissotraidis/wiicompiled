#pragma once

#include <string>
#include "dolphin/pad.h" // For PADDeaZones and PADButtonMapping
#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_keyboard.h"
#include "SDL3/SDL_keycode.h"
#include "SDL3/SDL_mouse.h"
#include "logging.hpp"

#include <absl/container/flat_hash_map.h>

namespace aurora::gamepad {
struct GameController {
  SDL_Gamepad* m_controller = nullptr;
  bool m_isGameCube = false;
  Sint32 m_index = -1;
  bool m_hasRumble = false;
  PADDeadZones m_deadZones{
      .emulateTriggers = true,
      .useDeadzones = true,
      .stickDeadZone = 8000,
      .substickDeadZone = 8000,
      .leftTriggerActivationZone = 31150,
      .rightTriggerActivationZone = 31150,
  };
  uint16_t m_vid = 0;
  uint16_t m_pid = 0;
  std::array<PADButtonMapping, PAD_BUTTON_COUNT> m_buttonMapping{};
  std::array<PADButtonMapping, PAD_BUTTON_COUNT> m_altButtonMapping{};
  std::array<PADAxisMapping, PAD_AXIS_COUNT> m_axisMapping{};
  uint16_t m_rumbleIntensityLow = 32767;
  uint16_t m_rumbleIntensityHigh = 32767;
  bool m_forceDeviceRumble = false;
  bool m_mappingLoaded = false;
  // SDL forgets the player index of devices mapped after connect; this keeps the assigned port.
  Sint32 m_playerIndex = -1;
  // Stable identity for telling identical controllers apart across launches.
  std::string m_path;
  int32_t m_slot = -1;
  // GC Pocket+ keeps vibrating after a hard stop; it needs an ordinary stop instead.
  bool m_gameCubeUseOrdinaryStop = false;
  constexpr bool operator==(const GameController& other) const {
    return m_controller == other.m_controller && m_index == other.m_index;
  }
  uint8_t m_ledRed = 0xFF;
  uint8_t m_ledGreen = 0xFF;
  uint8_t m_ledBlue = 0xFF;
  bool m_isColorDirty = true;
  bool m_hasRgbLed = false;
  // KartPad standard gamepad bridge (Android): latest SDL state, plus presses latched until read.
  uint32_t m_standardButtons = 0;
  uint32_t m_standardButtonPresses = 0;
  int16_t m_standardLeftX = 0;
  int16_t m_standardLeftY = 0;
  int16_t m_standardLeftTrigger = 0;
  int16_t m_standardRightTrigger = 0;
  bool m_standardRumbleEnabled = false;
  bool m_standardRumbleDirty = false;
  float m_standardRumbleStrength = 1.0f;
};

GameController* get_controller_for_player(uint32_t player) noexcept;
Sint32 get_instance_for_player(uint32_t player) noexcept;
SDL_JoystickID add_controller(SDL_JoystickID which) noexcept;
// Re-reads a controller whose SDL mapping changed. Returns false for unknown controllers.
bool refresh_controller(SDL_JoystickID instance) noexcept;
void remove_controller(Uint32 instance) noexcept;
Sint32 player_index(Uint32 instance) noexcept;
void set_player_index(Uint32 instance, Sint32 index) noexcept;
std::string controller_name(Uint32 instance) noexcept;
bool is_gamecube(Uint32 instance) noexcept;
bool controller_has_rumble(Uint32 instance) noexcept;
void controller_rumble(uint32_t instance, uint16_t low_freq_intensity, uint16_t high_freq_intensity,
                       uint16_t duration_ms) noexcept;
void get_device_rumble_intensity(uint16_t* low_freq_intensity, uint16_t* high_freq_intensity) noexcept;
void set_device_rumble_intensity(uint16_t low_freq_intensity, uint16_t high_freq_intensity) noexcept;
uint32_t controller_count() noexcept;
void initialize() noexcept;
void update() noexcept;
void set_wpad_hooks(void (*update)(), void (*shutdown)()) noexcept;
void persist_controller_for_player(uint32_t player, const GameController* controller) noexcept;
extern absl::flat_hash_map<Uint32, GameController> g_GameControllers;

void set_mouse_pointer(float x, float y, bool valid) noexcept;
bool get_mouse_pointer(float* x, float* y) noexcept;

void set_mouse_scroll(float scrollX, float scrollY) noexcept;
void get_mouse_scroll(float* scrollX, float* scrollY) noexcept;

void shutdown() noexcept;
} // namespace aurora::gamepad

// KartPad standard gamepad bridge; the public half is in <aurora/input.hpp>.
namespace aurora::input {
void sample_standard_gamepad_state(gamepad::GameController& controller) noexcept;
void update_standard_gamepad_button(Uint32 instance, Uint8 button, bool pressed) noexcept;
void update_standard_gamepad_axis(Uint32 instance, Uint8 axis, Sint16 value) noexcept;
void flush_standard_gamepad_rumble() noexcept;
} // namespace aurora::input
