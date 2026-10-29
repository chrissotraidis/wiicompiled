#ifdef NDEBUG
#undef NDEBUG
#endif
#include <SDL3/SDL.h>
#include <array>
#include <cassert>
#include <string>

int main() {
  assert(SDL_Init(SDL_INIT_GAMEPAD));
  // The Wii HIDAPI driver identifies Classic extensions with GUID byte 15 =
  // 130 and emits raw D-pad buttons 11..14, with no joystick hats.
  for (const char* nativeGuid : {"050000007e0500000603000000006882",
                                 "050000007e0500003003000000006882"}) {
    char* native = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(nativeGuid));
    assert(native);
    const std::string mapping = native;
    SDL_free(native);
    SDL_JoystickID previousID = 0;
    // Reattach the same capabilities with a new SDL ID and check all buttons
    // again, as happens after the extension is unplugged and reconnected.
    for (int connection = 0; connection < 2; ++connection) {
      SDL_VirtualJoystickDesc description{};
      SDL_INIT_INTERFACE(&description);
      description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
      description.nbuttons = 32;
      description.naxes = SDL_GAMEPAD_AXIS_COUNT;
      description.name = "Wii Classic raw input fixture";
      const SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
      assert(id && id != previousID);
      previousID = id;
      char guid[33];
      SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
      // Use the production HIDAPI mapping with a virtual device supplying the
      // same raw capabilities. This leaves the mapping logic inside real SDL.
      assert(SDL_AddGamepadMapping((std::string(guid) + mapping.substr(32)).c_str()) >= 0);
      SDL_Gamepad* pad = SDL_OpenGamepad(id);
      assert(pad);
      SDL_Joystick* joystick = SDL_GetGamepadJoystick(pad);
      assert(SDL_GetNumJoystickHats(joystick) == 0);
      const std::array directions = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                     SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
      for (auto pressed : directions) {
        assert(SDL_SetJoystickVirtualButton(joystick, pressed, true));
        SDL_UpdateJoysticks();
        for (auto direction : directions) {
          assert(SDL_GetGamepadButton(pad, direction) == (direction == pressed));
        }
        // A second sample keeps the hold; releasing clears every direction.
        SDL_UpdateJoysticks();
        assert(SDL_GetGamepadButton(pad, pressed));
        assert(SDL_SetJoystickVirtualButton(joystick, pressed, false));
        SDL_UpdateJoysticks();
        for (auto direction : directions) assert(!SDL_GetGamepadButton(pad, direction));
      }
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_UP, true));
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true));
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, true));
      SDL_UpdateJoysticks();
      assert(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP));
      assert(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
      assert(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST));
      SDL_CloseGamepad(pad);
      // Reopening preserves the correct mapping when new input reports arrive.
      pad = SDL_OpenGamepad(id);
      assert(pad);
      joystick = SDL_GetGamepadJoystick(pad);
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_UP, true));
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true));
      SDL_UpdateJoysticks();
      assert(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP));
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_UP, false));
      assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false));
      SDL_UpdateJoysticks();
      for (auto direction : directions) assert(!SDL_GetGamepadButton(pad, direction));
      SDL_CloseGamepad(pad);
      assert(SDL_DetachVirtualJoystick(id));
    }
  }
  // Standalone remote orientation and Nunchuk bindings retain their own maps.
  for (const char* nativeGuid : {"050000007e0500000603000000006880",
                                 "050000007e0500000603000000006881"}) {
    char* native = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(nativeGuid));
    assert(native);
    const std::string mapping = native;
    assert(mapping.find(nativeGuid[31] == '0' ? "dpup:h0.1" : "dpup:b11") != std::string::npos);
    SDL_free(native);
  }
  SDL_Quit();
}
