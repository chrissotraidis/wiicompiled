#!/usr/bin/env python3
"""Run the production port matcher/reconciler against Wii extension hotplug cases.

Only SDL identity access and player-index writes are replaced. No controller,
game data, or graphics dependencies are required.
"""
from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / "lib/input.cpp").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
using Uint32 = uint32_t;
constexpr int PAD_MAX_CONTROLLERS = 4;
enum class PortPreferenceState : uint8_t { Unset, None, Controller };
struct ControllerIdentity {
  std::string guid, serial, path;
  int32_t slot = -1;
};
struct PortPreference {
  PortPreferenceState state = PortPreferenceState::Unset;
  ControllerIdentity identity;
};
struct GameController { ControllerIdentity identity; int player = -1; };
std::array<PortPreference, PAD_MAX_CONTROLLERS> g_portPreferences;
std::map<Uint32, GameController> g_GameControllers;
void ensure_port_preferences_loaded() {}
void save_port_preferences() {}
ControllerIdentity controller_identity(const GameController& controller) { return controller.identity; }
int effective_player_index(const GameController& controller) { return controller.player; }
void assign_player_index(GameController& controller, int32_t port) { controller.player = port; }
// SDL GUID text encodes USB vendor/product as little-endian words at bytes 4/8.
std::string SDL_StringToGUID(const char* value) { return value; }
void SDL_GetJoystickGUIDInfo(const std::string& guid, uint16_t* vendor, uint16_t* product,
                            void*, void*) {
  const auto word = [&](size_t offset) {
    return static_cast<uint16_t>(std::stoul(guid.substr(offset, 2), nullptr, 16) |
      (std::stoul(guid.substr(offset + 2, 2), nullptr, 16) << 8));
  };
  *vendor = word(8); *product = word(16);
}
'''
code += function("std::string normalize_serial(")
code += function("bool serial_matches(")
constants_start = source.index("constexpr int kScoreVidPid")
code += source[constants_start:source.index("int identity_score(", constants_start)]
code += function("int identity_score(")
code += function("void apply_port_preferences(")
code += r'''
ControllerIdentity remote(const char* path, int extension, const char* serial = "") {
  ControllerIdentity result{"050000007e0500000603000000006800", serial, path, 0};
  const char hex[] = "0123456789abcdef";
  result.guid[30] = hex[extension >> 4]; result.guid[31] = hex[extension & 15];
  return result;
}
int main() {
  // SDL's HIDAPI Wii driver changes only GUID byte 15 after an extension swap.
  const auto bare = remote("same-hid-device", 128);
  const auto nunchuk = remote("same-hid-device", 129);
  const auto classic = remote("same-hid-device", 130);
  assert(identity_score(bare, nunchuk) > 0);
  assert(identity_score(nunchuk, classic) > 0);
  assert(identity_score(classic, bare) > 0);
  assert(identity_score(bare, remote("different-remote", 129)) == 0);
  assert(identity_score(remote("", 128), remote("", 129)) == 0);
  assert(identity_score(remote("same-hid-device", 128, "aa"),
                        remote("same-hid-device", 129, "bb")) == 0);
  // A different product/driver must not inherit the port merely by path.
  auto otherProduct = nunchuk; otherProduct.guid[16] = '7';
  assert(identity_score(bare, otherProduct) == 0);
  auto otherDriver = nunchuk; otherDriver.guid[28] = '7';
  assert(identity_score(bare, otherDriver) == 0);
  // Existing serial fallback and exact GUID matching remain supported.
  assert(identity_score(remote("old-path", 128, "AA:BB"),
                        remote("new-path", 129, "aa-bb")) > 0);
  assert(identity_score(bare, bare) > 0);

  // Saved Player 1 -> disconnect -> new SDL instance with an extension.
  // A second, otherwise identical remote keeps Player 2 throughout.
  g_portPreferences[0] = {PortPreferenceState::Controller, bare};
  g_portPreferences[1] = {PortPreferenceState::Controller, remote("second-hid-device", 128)};
  g_portPreferences[2].state = PortPreferenceState::None;
  g_GameControllers[10] = {bare, 0};
  g_GameControllers[20] = {remote("second-hid-device", 128), 1};
  apply_port_preferences();
  g_GameControllers.erase(10); apply_port_preferences();
  assert(g_GameControllers.at(20).player == 1);
  g_GameControllers[30] = {nunchuk, 0}; apply_port_preferences();
  assert(g_GameControllers.at(30).player == 0);
  assert(g_GameControllers.at(20).player == 1);
  g_GameControllers.erase(30);
  g_GameControllers[40] = {classic, 0}; apply_port_preferences();
  assert(g_GameControllers.at(40).player == 0);
  assert(g_GameControllers.at(20).player == 1);
  // An explicitly unassigned port stays unassigned.
  g_portPreferences[0].state = PortPreferenceState::None;
  apply_port_preferences();
  assert(g_GameControllers.at(40).player == -1);
}
'''
with tempfile.TemporaryDirectory(prefix="wii-port-test-") as temporary:
    path = Path(temporary)
    test = path / "test.cpp"
    test.write_text(code)
    executable = path / "test"
    subprocess.run(["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(test), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: Wii extension reconnect retains its port; distinct devices and explicit None stay separate")
