#pragma once

#include <cstddef>
#include <cstdint>

namespace aurora::input {
// SDL controllers assigned to Classic channels own those guest ports even
// while the bridge is suspended. This does not consume button presses.
uint32_t standard_gamepad_assigned_mask() noexcept;

// Connection probes must never consume a latched registration button press.
bool standard_gamepad_connected(uint32_t player,
                                bool allowSingleUnassigned) noexcept;

enum StandardGamepadButton : uint32_t {
  kStandardGamepadSouth = 1u << 0,
  kStandardGamepadEast = 1u << 1,
  kStandardGamepadWest = 1u << 2,
  kStandardGamepadNorth = 1u << 3,
  kStandardGamepadBack = 1u << 4,
  kStandardGamepadStart = 1u << 5,
  kStandardGamepadLeftShoulder = 1u << 6,
  kStandardGamepadRightShoulder = 1u << 7,
  kStandardGamepadDpadUp = 1u << 8,
  kStandardGamepadDpadDown = 1u << 9,
  kStandardGamepadDpadLeft = 1u << 10,
  kStandardGamepadDpadRight = 1u << 11,
};

struct StandardGamepadState {
  bool connected = false;
  uint32_t buttons = 0;
  int16_t leftX = 0;
  int16_t leftY = 0;
  int16_t leftTrigger = 0;
  int16_t rightTrigger = 0;
};

// Reads an explicitly assigned controller. Player zero may opt into a lone,
// unassigned controller so the first connected pad works before a settings UI
// exists; multiple unassigned controllers are never selected implicitly.
bool read_standard_gamepad_state(uint32_t player, bool allowSingleUnassigned,
                                 StandardGamepadState* state) noexcept;

// Queues a rumble change for the controller resolved by the same assignment
// rule as read_standard_gamepad_state. Returns false when no suitable
// rumble-capable controller exists. SDL applies the request during event polling.
bool set_standard_gamepad_rumble(uint32_t player, bool allowSingleUnassigned,
                                 bool enabled) noexcept;

// Enables or suspends the standard gamepad bridge. Suspension makes snapshots
// neutral immediately and stops any active rumble before Android releases its
// native surface. Rumble stop requests remain accepted while suspended.
void set_standard_gamepads_active(bool active) noexcept;

// Takes a stable, instance-id-sorted snapshot of currently open SDL gamepads.
// The return value is the number written, capped by capacity.
uint32_t list_standard_gamepads(uint32_t* instances, int32_t* players,
                                uint32_t capacity) noexcept;

// Copies a bounded display name for one currently connected instance.
bool copy_standard_gamepad_name(uint32_t instance, char* name,
                                size_t capacity) noexcept;

// Assigns one connected SDL instance to P1-P4, evicting any prior controller
// from that port and persisting the controller identity. The persisted old
// port is cleared when a controller moves.
bool assign_standard_gamepad(uint32_t instance, uint32_t player) noexcept;

// Persists an explicitly empty P1-P4 port and clears any connected controller
// currently assigned to it.
bool clear_standard_gamepad_player(uint32_t player) noexcept;

}  // namespace aurora::input
