#include "input.hpp"
#include "internal.hpp"

#include <aurora/input.hpp>

#include "magic_enum.hpp"

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace aurora::input {
Module Log("aurora::input");
absl::flat_hash_map<Uint32, GameController> g_GameControllers;

namespace {
std::atomic_bool g_standardGamepadsActive{true};
std::mutex g_standardGamepadBridgeMutex;

constexpr uint32_t kPortPreferencesMagic = SBIG('CPRT');
constexpr uint32_t kPortPreferencesVersion = 2;
constexpr uint32_t kMaxPersistedStringLength = 256;

enum class PortPreferenceState : uint8_t {
  Unset = 0,
  None = 1,
  Controller = 2,
};

struct ControllerIdentity {
  std::string guid;
  std::string serial;
};

struct PortPreference {
  PortPreferenceState state = PortPreferenceState::Unset;
  ControllerIdentity identity;
};

std::array<PortPreference, PAD_MAX_CONTROLLERS> g_portPreferences;
bool g_portPreferencesLoaded = false;

std::string port_preferences_path() {
  if (g_config.userPath == nullptr) {
    return {};
  }

  std::string path{g_config.userPath};
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += "controller_ports.dat";
  return path;
}

std::string normalize_serial(const char* serial) {
  std::string normalized;
  if (serial == nullptr) {
    return normalized;
  }

  for (const char* c = serial; *c != '\0'; ++c) {
    if (*c == ':' || *c == '-') {
      continue;
    }
    normalized += *c >= 'A' && *c <= 'Z' ? static_cast<char>(*c - 'A' + 'a') : *c;
  }
  return normalized;
}

bool serial_matches(const std::string& saved, const std::string& current) {
  return !saved.empty() && !current.empty() && normalize_serial(saved.c_str()) == normalize_serial(current.c_str());
}

ControllerIdentity controller_identity(const GameController& controller) {
  ControllerIdentity identity;
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(controller.m_controller)), guid, sizeof(guid));
  identity.guid = guid;
  identity.serial = normalize_serial(SDL_GetGamepadSerial(controller.m_controller));
  return identity;
}

bool read_exact(SDL_IOStream* file, void* dst, size_t size) {
  auto* bytes = static_cast<uint8_t*>(dst);
  size_t total = 0;
  while (total < size) {
    const size_t read = SDL_ReadIO(file, bytes + total, size - total);
    if (read == 0) {
      return false;
    }
    total += read;
  }
  return true;
}

bool write_exact(SDL_IOStream* file, const void* src, size_t size) {
  auto* bytes = static_cast<const uint8_t*>(src);
  size_t total = 0;
  while (total < size) {
    const size_t written = SDL_WriteIO(file, bytes + total, size - total);
    if (written == 0) {
      return false;
    }
    total += written;
  }
  return true;
}

template <typename T>
bool read_value(SDL_IOStream* file, T& value) {
  return read_exact(file, &value, sizeof(value));
}

template <typename T>
bool write_value(SDL_IOStream* file, const T& value) {
  return write_exact(file, &value, sizeof(value));
}

bool read_string(SDL_IOStream* file, std::string& value) {
  uint32_t size = 0;
  if (!read_value(file, size) || size > kMaxPersistedStringLength) {
    return false;
  }

  value.resize(size);
  return size == 0 || read_exact(file, value.data(), size);
}

bool write_string(SDL_IOStream* file, const std::string& value) {
  const uint32_t size = static_cast<uint32_t>(std::min<size_t>(value.size(), kMaxPersistedStringLength));
  return write_value(file, size) && (size == 0 || write_exact(file, value.data(), size));
}

bool read_identity(SDL_IOStream* file, ControllerIdentity& identity) {
  return read_string(file, identity.guid) && read_string(file, identity.serial);
}

bool write_identity(SDL_IOStream* file, const ControllerIdentity& identity) {
  return write_string(file, identity.guid) && write_string(file, identity.serial);
}

bool read_port_preferences_file(std::array<PortPreference, PAD_MAX_CONTROLLERS>& preferences) {
  const auto path = port_preferences_path();
  if (path.empty()) {
    return true;
  }

  SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "rb");
  if (file == nullptr) {
    return true;
  }

  uint32_t magic = 0;
  uint32_t version = 0;
  bool ok = read_value(file, magic) && read_value(file, version) && magic == kPortPreferencesMagic &&
            version == kPortPreferencesVersion;

  for (auto& preference : preferences) {
    uint8_t state = 0;
    ControllerIdentity identity;
    ok = ok && read_value(file, state) && state <= static_cast<uint8_t>(PortPreferenceState::Controller) &&
         read_identity(file, identity);
    if (ok) {
      preference.state = static_cast<PortPreferenceState>(state);
      preference.identity = std::move(identity);
    }
  }

  SDL_CloseIO(file);
  if (!ok) {
    Log.warn("Ignoring invalid controller port preference file '{}'", path);
  }
  return ok;
}

void ensure_port_preferences_loaded() {
  if (g_portPreferencesLoaded) {
    return;
  }

  std::array<PortPreference, PAD_MAX_CONTROLLERS> preferences;
  if (read_port_preferences_file(preferences)) {
    g_portPreferences = std::move(preferences);
  }
  g_portPreferencesLoaded = true;
}

void save_port_preferences() {
  const auto path = port_preferences_path();
  if (path.empty()) {
    return;
  }

  if (!SDL_CreateDirectory(g_config.userPath)) {
    Log.warn("Failed to create controller port preference directory '{}': {}", g_config.userPath, SDL_GetError());
    return;
  }

  SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "wb");
  if (file == nullptr) {
    Log.warn("Failed to open controller port preference file '{}': {}", path, SDL_GetError());
    return;
  }

  bool ok = write_value(file, kPortPreferencesMagic) && write_value(file, kPortPreferencesVersion);
  for (const auto& preference : g_portPreferences) {
    const auto state = static_cast<uint8_t>(preference.state);
    ok = ok && write_value(file, state) && write_identity(file, preference.identity);
  }

  if (!SDL_FlushIO(file)) {
    ok = false;
  }
  if (!SDL_CloseIO(file)) {
    ok = false;
  }
  if (!ok) {
    Log.warn("Failed to write controller port preference file '{}': {}", path, SDL_GetError());
  }
}

enum class IdentityMatch {
  None,
  Fallback,
  Exact,
};

IdentityMatch identity_match(const ControllerIdentity& saved, const ControllerIdentity& current) {
  if (saved.guid.empty()) {
    return IdentityMatch::None;
  }
  if (saved.guid == current.guid) {
    return saved.serial.empty() || serial_matches(saved.serial, current.serial) ? IdentityMatch::Exact
                                                                                : IdentityMatch::None;
  }
  if (!serial_matches(saved.serial, current.serial)) {
    return IdentityMatch::None;
  }

  // Attempt to match against VID/PID as a fallback if the GUID changes
  uint16_t savedVendor = 0;
  uint16_t savedProduct = 0;
  uint16_t currentVendor = 0;
  uint16_t currentProduct = 0;
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(saved.guid.c_str()), &savedVendor, &savedProduct, nullptr, nullptr);
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(current.guid.c_str()), &currentVendor, &currentProduct, nullptr, nullptr);
  return savedVendor != 0 && savedVendor == currentVendor && savedProduct != 0 && savedProduct == currentProduct
             ? IdentityMatch::Fallback
             : IdentityMatch::None;
}

void assign_player_index(GameController& controller, int32_t port) {
  SDL_SetGamepadPlayerIndex(controller.m_controller, port);
  controller.m_playerIndex = port;
}

// SDL forgets the index for devices mapped after connect, so player_index() falls
// back to the cached copy; both have to move together or a port looks doubly taken.
int32_t effective_player_index(const GameController& controller) {
  const int32_t player = SDL_GetGamepadPlayerIndex(controller.m_controller);
  return player >= 0 ? player : controller.m_playerIndex;
}

bool is_instance_claimed(const std::array<Uint32, PAD_MAX_CONTROLLERS>& claimedControllers, size_t claimedCount,
                         Uint32 instance) {
  return std::find(claimedControllers.begin(), claimedControllers.begin() + claimedCount, instance) !=
         claimedControllers.begin() + claimedCount;
}

void apply_port_preferences() noexcept {
  ensure_port_preferences_loaded();
  if (!std::any_of(g_portPreferences.begin(), g_portPreferences.end(),
                   [](const auto& preference) { return preference.state != PortPreferenceState::Unset; })) {
    return;
  }

  for (auto& [instance, controller] : g_GameControllers) {
    const int32_t player = effective_player_index(controller);
    if (player >= 0 && player < PAD_MAX_CONTROLLERS && g_portPreferences[player].state != PortPreferenceState::Unset) {
      // Keep SDL's default player assignment from taking explicitly configured ports
      assign_player_index(controller, -1);
    }
  }

  std::array<Uint32, PAD_MAX_CONTROLLERS> claimedControllers{};
  size_t claimedCount = 0;
  for (uint32_t port = 0; port < g_portPreferences.size(); ++port) {
    const auto& preference = g_portPreferences[port];
    if (preference.state != PortPreferenceState::Controller) {
      continue;
    }

    Uint32 fallbackInstance = 0;
    GameController* fallbackController = nullptr;
    for (auto& [instance, controller] : g_GameControllers) {
      if (is_instance_claimed(claimedControllers, claimedCount, instance)) {
        continue;
      }

      switch (identity_match(preference.identity, controller_identity(controller))) {
      case IdentityMatch::Exact:
        assign_player_index(controller, static_cast<int32_t>(port));
        claimedControllers[claimedCount++] = instance;
        fallbackController = nullptr;
        break;
      case IdentityMatch::Fallback:
        // Prefer any later exact match before claiming a fallback candidate
        if (fallbackController == nullptr) {
          fallbackInstance = instance;
          fallbackController = &controller;
        }
        continue;
      case IdentityMatch::None:
        continue;
      }
      break;
    }

    if (fallbackController != nullptr) {
      assign_player_index(*fallbackController, static_cast<int32_t>(port));
      claimedControllers[claimedCount++] = fallbackInstance;
    }
  }
}

// Ports are explicit assignments. SDL may choose a player index at connection
// time, but accepting it would make a newly connected controller silently take
// over a game port before the user assigns it in the controller menu.
void ensure_player_index(GameController& controller) noexcept {
  assign_player_index(controller, -1);
}
} // namespace

GameController* get_controller_for_player(uint32_t player) noexcept {
  for (auto& [which, controller] : g_GameControllers) {
    if (player_index(which) == player) {
      return &controller;
    }
  }

  return nullptr;
}

static GameController* resolve_standard_gamepad(
    uint32_t player, bool allowSingleUnassigned) noexcept {
  GameController* controller = nullptr;
  for (auto& [instance, candidate] : g_GameControllers) {
    (void)instance;
    if (candidate.m_playerIndex == static_cast<int32_t>(player)) {
      controller = &candidate;
      break;
    }
  }
  if (controller == nullptr && player == 0 && allowSingleUnassigned &&
      g_GameControllers.size() == 1) {
    auto& [instance, candidate] = *g_GameControllers.begin();
    (void)instance;
    if (candidate.m_playerIndex < 0) {
      controller = &candidate;
    }
  }
  return controller;
}

bool standard_gamepad_connected(uint32_t player, bool allowSingleUnassigned) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  if (!g_standardGamepadsActive.load(std::memory_order_acquire)) return false;
  const GameController* controller =
      resolve_standard_gamepad(player, allowSingleUnassigned);
  return controller != nullptr && controller->m_controller != nullptr;
}

bool read_standard_gamepad_state(uint32_t player, bool allowSingleUnassigned,
                                 StandardGamepadState* state) noexcept {
  if (state == nullptr) {
    return false;
  }
  *state = {};

  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  if (!g_standardGamepadsActive.load(std::memory_order_acquire)) {
    return false;
  }

  GameController* controller =
      resolve_standard_gamepad(player, allowSingleUnassigned);
  if (controller == nullptr || controller->m_controller == nullptr) {
    return false;
  }

  state->connected = true;
  state->buttons = controller->m_standardButtons | controller->m_standardButtonPresses;
  controller->m_standardButtonPresses = 0;
  state->leftX = controller->m_standardLeftX;
  state->leftY = controller->m_standardLeftY;
  state->leftTrigger = controller->m_standardLeftTrigger;
  state->rightTrigger = controller->m_standardRightTrigger;
  return true;
}

bool set_standard_gamepad_rumble(uint32_t player, bool allowSingleUnassigned,
                                 bool enabled) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  if (enabled &&
      !g_standardGamepadsActive.load(std::memory_order_acquire)) {
    return false;
  }
  GameController* controller =
      resolve_standard_gamepad(player, allowSingleUnassigned);
  if (controller == nullptr || controller->m_controller == nullptr ||
      !controller->m_hasRumble) {
    return false;
  }

  controller->m_standardRumbleEnabled = enabled;
  controller->m_standardRumbleDirty = true;
  return true;
}

void set_standard_gamepads_active(bool active) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  const bool previous =
      g_standardGamepadsActive.exchange(active, std::memory_order_acq_rel);
  if (previous == active) {
    return;
  }

  if (!active) {
    for (auto& [instance, controller] : g_GameControllers) {
      (void)instance;
      if (controller.m_controller != nullptr && controller.m_hasRumble) {
        controller.m_standardRumbleEnabled = false;
        controller.m_standardRumbleDirty = true;
      }
    }
  }
  Log.info("Standard gamepads {}", active ? "resumed" : "suspended");
}

uint32_t list_standard_gamepads(uint32_t* instances, int32_t* players,
                                uint32_t capacity) noexcept {
  if (capacity != 0 && (instances == nullptr || players == nullptr)) {
    return 0;
  }
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  std::vector<uint32_t> ordered;
  ordered.reserve(g_GameControllers.size());
  for (const auto& [instance, controller] : g_GameControllers) {
    (void)controller;
    ordered.push_back(instance);
  }
  std::sort(ordered.begin(), ordered.end());
  const uint32_t written = std::min<uint32_t>(
      capacity, static_cast<uint32_t>(ordered.size()));
  for (uint32_t index = 0; index < written; ++index) {
    instances[index] = ordered[index];
    players[index] = g_GameControllers.at(ordered[index]).m_playerIndex;
  }
  return written;
}

bool copy_standard_gamepad_name(uint32_t instance, char* name,
                                size_t capacity) noexcept {
  if (name == nullptr || capacity == 0) {
    return false;
  }
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  const auto found = g_GameControllers.find(instance);
  if (found == g_GameControllers.end() || found->second.m_controller == nullptr) {
    name[0] = '\0';
    return false;
  }
  const char* source = SDL_GetGamepadName(found->second.m_controller);
  if (source == nullptr) {
    source = "Controller";
  }
  const size_t length = std::min(std::strlen(source), capacity - 1);
  std::memcpy(name, source, length);
  name[length] = '\0';
  return true;
}

bool assign_standard_gamepad(uint32_t instance, uint32_t player) noexcept {
  if (player >= PAD_MAX_CONTROLLERS) {
    return false;
  }
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  const auto found = g_GameControllers.find(instance);
  if (found == g_GameControllers.end()) {
    return false;
  }
  ensure_port_preferences_loaded();
  GameController& selected = found->second;
  const int32_t oldPlayer = selected.m_playerIndex;
  if (oldPlayer >= 0 && oldPlayer < PAD_MAX_CONTROLLERS &&
      oldPlayer != static_cast<int32_t>(player)) {
    g_portPreferences[oldPlayer].state = PortPreferenceState::None;
    g_portPreferences[oldPlayer].identity = {};
  }
  for (auto& [otherInstance, controller] : g_GameControllers) {
    if (otherInstance != instance &&
        controller.m_playerIndex == static_cast<int32_t>(player)) {
      assign_player_index(controller, -1);
    }
  }
  assign_player_index(selected, static_cast<int32_t>(player));
  g_portPreferences[player].state = PortPreferenceState::Controller;
  g_portPreferences[player].identity = controller_identity(selected);
  save_port_preferences();
  return true;
}

bool clear_standard_gamepad_player(uint32_t player) noexcept {
  if (player >= PAD_MAX_CONTROLLERS) {
    return false;
  }
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  ensure_port_preferences_loaded();
  for (auto& [instance, controller] : g_GameControllers) {
    (void)instance;
    if (controller.m_playerIndex == static_cast<int32_t>(player)) {
      assign_player_index(controller, -1);
    }
  }
  g_portPreferences[player].state = PortPreferenceState::None;
  g_portPreferences[player].identity = {};
  save_port_preferences();
  return true;
}

namespace {
uint32_t standard_button_mask(Uint8 button) noexcept {
  switch (static_cast<SDL_GamepadButton>(button)) {
  case SDL_GAMEPAD_BUTTON_SOUTH: return kStandardGamepadSouth;
  case SDL_GAMEPAD_BUTTON_EAST: return kStandardGamepadEast;
  case SDL_GAMEPAD_BUTTON_WEST: return kStandardGamepadWest;
  case SDL_GAMEPAD_BUTTON_NORTH: return kStandardGamepadNorth;
  case SDL_GAMEPAD_BUTTON_BACK: return kStandardGamepadBack;
  case SDL_GAMEPAD_BUTTON_START: return kStandardGamepadStart;
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return kStandardGamepadLeftShoulder;
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kStandardGamepadRightShoulder;
  case SDL_GAMEPAD_BUTTON_DPAD_UP: return kStandardGamepadDpadUp;
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kStandardGamepadDpadDown;
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kStandardGamepadDpadLeft;
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kStandardGamepadDpadRight;
  default: return 0;
  }
}

void sample_standard_gamepad_state(GameController& controller) noexcept {
  controller.m_standardButtons = 0;
  for (int button = SDL_GAMEPAD_BUTTON_SOUTH; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
    const uint32_t mask = standard_button_mask(static_cast<Uint8>(button));
    if (mask != 0 && SDL_GetGamepadButton(controller.m_controller, static_cast<SDL_GamepadButton>(button))) {
      controller.m_standardButtons |= mask;
    }
  }
  controller.m_standardLeftX = SDL_GetGamepadAxis(controller.m_controller, SDL_GAMEPAD_AXIS_LEFTX);
  controller.m_standardLeftY = SDL_GetGamepadAxis(controller.m_controller, SDL_GAMEPAD_AXIS_LEFTY);
  controller.m_standardLeftTrigger = SDL_GetGamepadAxis(controller.m_controller, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  controller.m_standardRightTrigger = SDL_GetGamepadAxis(controller.m_controller, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}
} // namespace

void update_standard_gamepad_button(Uint32 instance, Uint8 button, bool pressed) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  const auto it = g_GameControllers.find(instance);
  if (it == g_GameControllers.end()) {
    return;
  }
  const uint32_t mask = standard_button_mask(button);
  if (pressed) {
    it->second.m_standardButtons |= mask;
    it->second.m_standardButtonPresses |= mask;
  } else {
    it->second.m_standardButtons &= ~mask;
  }
}

void update_standard_gamepad_axis(Uint32 instance, Uint8 axis, Sint16 value) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  const auto it = g_GameControllers.find(instance);
  if (it == g_GameControllers.end()) {
    return;
  }
  switch (static_cast<SDL_GamepadAxis>(axis)) {
  case SDL_GAMEPAD_AXIS_LEFTX: it->second.m_standardLeftX = value; break;
  case SDL_GAMEPAD_AXIS_LEFTY: it->second.m_standardLeftY = value; break;
  case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: it->second.m_standardLeftTrigger = value; break;
  case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: it->second.m_standardRightTrigger = value; break;
  default: break;
  }
}

void flush_standard_gamepad_rumble() noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  for (auto& [instance, controller] : g_GameControllers) {
    (void)instance;
    if (!controller.m_standardRumbleDirty || controller.m_controller == nullptr || !controller.m_hasRumble) {
      continue;
    }
    const bool enabled = controller.m_standardRumbleEnabled &&
                         g_standardGamepadsActive.load(std::memory_order_acquire);
    const uint16_t low = enabled ? controller.m_rumbleIntensityLow : 0;
    const uint16_t high = enabled ? controller.m_rumbleIntensityHigh : 0;
    if (!SDL_RumbleGamepad(controller.m_controller, low, high, enabled ? 0xffffffffu : 0u)) {
      Log.warn("Failed to {} standard gamepad rumble: {}", enabled ? "start" : "stop", SDL_GetError());
    }
    controller.m_standardRumbleDirty = false;
  }
}

Sint32 get_instance_for_player(uint32_t player) noexcept {
  for (const auto& [which, controller] : g_GameControllers) {
    if (player_index(which) == player) {
      return which;
    }
  }

  return {};
}

SDL_JoystickID add_controller(SDL_JoystickID which) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  if (g_GameControllers.contains(which)) {
    return which;
  }
  auto* ctrl = SDL_OpenGamepad(which);
  if (ctrl != nullptr) {
    GameController controller;
    controller.m_controller = ctrl;
    controller.m_index = which;
    controller.m_vid = SDL_GetGamepadVendor(ctrl);
    controller.m_pid = SDL_GetGamepadProduct(ctrl);
    if (controller.m_vid == 0x05ac /* USB_VENDOR_APPLE */ && controller.m_pid == 3) {
      // Ignore Apple TV remote
      SDL_CloseGamepad(ctrl);
      return -1;
    }
    controller.m_isGameCube = controller.m_vid == 0x057E && controller.m_pid == 0x0337;
    if (controller.m_isGameCube ||
        (SDL_GetGamepadType(ctrl) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && controller.m_pid == 0x2073)) {
      controller.m_deadZones.emulateTriggers = false;
    }
    const auto props = SDL_GetGamepadProperties(ctrl);
    controller.m_hasRumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, true);
    controller.m_hasRgbLed = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
    sample_standard_gamepad_state(controller);
    SDL_JoystickID instance = SDL_GetJoystickID(SDL_GetGamepadJoystick(ctrl));
    g_GameControllers[instance] = controller;
    ensure_player_index(g_GameControllers[instance]);
    apply_port_preferences();
    return instance;
  }

  return -1;
}

bool refresh_controller(SDL_JoystickID instance) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  const auto it = g_GameControllers.find(instance);
  if (it == g_GameControllers.end()) {
    return false;
  }
  // The SDL mapping changed underneath us; drop the cached PAD bindings so they
  // are rebuilt from the new one.
  it->second.m_mappingLoaded = false;
  sample_standard_gamepad_state(it->second);
  ensure_player_index(it->second);
  apply_port_preferences();
  return true;
}

void remove_controller(Uint32 instance) noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    if (it->second.m_controller != nullptr && it->second.m_hasRumble) {
      SDL_RumbleGamepad(it->second.m_controller, 0, 0, 0);
    }
    SDL_CloseGamepad(it->second.m_controller);
    g_GameControllers.erase(it);
    apply_port_preferences();
  }
}

bool is_gamecube(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_isGameCube;
  }
  return false;
}

int32_t player_index(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_playerIndex;
  }
  return -1;
}

void set_player_index(Uint32 instance, Sint32 index) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    SDL_SetGamepadPlayerIndex(it->second.m_controller, index);
    it->second.m_playerIndex = index;
  }
}

std::string controller_name(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    const auto* name = SDL_GetGamepadName(it->second.m_controller);
    if (name != nullptr) {
      return {name};
    }
  }
  return {};
}

bool controller_has_rumble(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_hasRumble;
  }
  return false;
}

void controller_rumble(uint32_t instance, uint16_t low_freq_intensity, uint16_t high_freq_intensity,
                       uint16_t duration_ms) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    SDL_RumbleGamepad(it->second.m_controller, low_freq_intensity, high_freq_intensity, duration_ms);
  }
}

uint32_t controller_count() noexcept { return g_GameControllers.size(); }

void persist_controller_for_player(uint32_t player, const GameController* controller) noexcept {
  if (player >= PAD_MAX_CONTROLLERS) {
    return;
  }

  ensure_port_preferences_loaded();
  if (controller != nullptr) {
    g_portPreferences[player].state = PortPreferenceState::Controller;
    g_portPreferences[player].identity = controller_identity(*controller);
  } else {
    g_portPreferences[player].state = PortPreferenceState::None;
    g_portPreferences[player].identity = {};
  }
  save_port_preferences();
}

void initialize() noexcept {
  /* Make sure we initialize everything input related now, this will automatically add all of the connected controllers
   * as expected */
  ASSERT(SDL_Init(SDL_INIT_HAPTIC | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD), "Failed to initialize SDL subsystems: {}",
         SDL_GetError());
}

struct MouseScrollStatus {
  float scrollX;
  float scrollY;
};

static MouseScrollStatus g_MouseStatus;

void set_mouse_scroll(const float scrollX, const float scrollY) noexcept {
  g_MouseStatus.scrollX = scrollX;
  g_MouseStatus.scrollY = scrollY;
}

void get_mouse_scroll(float* scrollX, float* scrollY) noexcept {
  *scrollX = g_MouseStatus.scrollX;
  *scrollY = g_MouseStatus.scrollY;
}

void shutdown() noexcept {
  std::scoped_lock lock(g_standardGamepadBridgeMutex);
  // Upon shutdown we want to ensure all controllers are in a default state, so force all rumble supporting controllers
  // to shut off their rumble motors.
  for (const auto& controller : g_GameControllers) {
    if (!controller.second.m_hasRumble) {
      continue;
    }
    controller_rumble(controller.first, 0, 0, 0);
  }
}
} // namespace aurora::input
