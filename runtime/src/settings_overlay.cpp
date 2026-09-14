#include "settings_overlay.h"
#include "kartpad/macos_settings_shortcut.hpp"
extern "C" void KartPadOpenSettingsFromShortcut();
#include "wup028_adapter.h"
#include "audio_backend.h"
#include "controller_mapping_wizard.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

#if defined(__APPLE__) && !defined(KARTPAD_IOS_RUNTIME)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
extern "C" void KartPadControllersTick();
extern "C" bool KartPadControllersVisible();
#endif
#endif
namespace {
std::atomic_bool g_nativeSettingsReload{false};
std::atomic_bool g_nativeCompatibilityRequest{false};
}
extern "C" void KartPadRequestSettingsReload() { g_nativeSettingsReload.store(true); }
extern "C" void KartPadRequestControllerCompatibility() { g_nativeCompatibilityRequest.store(true); }
namespace settings_overlay {
namespace {

bool g_compatibilityVisible = false;
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

struct ControllerButtonItem {
    const char* configKey;
    const char* label;
    PADButton padButton;
};

constexpr std::array<ControllerButtonItem, PAD_BUTTON_COUNT> kControllerButtons = {{
    {"a", "A", PAD_BUTTON_A},
    {"b", "B", PAD_BUTTON_B},
    {"x", "X", PAD_BUTTON_X},
    {"y", "Y", PAD_BUTTON_Y},
    {"start", "Start", PAD_BUTTON_START},
    {"z", "Z", PAD_TRIGGER_Z},
    {"l", "L", PAD_TRIGGER_L},
    {"r", "R", PAD_TRIGGER_R},
    {"up", "D-pad Up", PAD_BUTTON_UP},
    {"down", "D-pad Down", PAD_BUTTON_DOWN},
    {"left", "D-pad Left", PAD_BUTTON_LEFT},
    {"right", "D-pad Right", PAD_BUTTON_RIGHT},
}};

struct NativeButtonItem {
    const char* configName;
    const char* label;
    uint32_t nativeButton;
};

constexpr std::array<NativeButtonItem, SDL_GAMEPAD_BUTTON_COUNT + 1> kNativeButtons = {{
    {"unmapped", "Unmapped / analog trigger", PAD_NATIVE_BUTTON_INVALID},
    {"south", "South (A / Cross)", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", "East (B / Circle)", SDL_GAMEPAD_BUTTON_EAST},
    {"west", "West (X / Square)", SDL_GAMEPAD_BUTTON_WEST},
    {"north", "North (Y / Triangle)", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", "Back / Select", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", "Guide / Home", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", "Start / Options", SDL_GAMEPAD_BUTTON_START},
    {"left_stick", "Left stick click", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"right_stick", "Right stick click", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"left_shoulder", "Left shoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"right_shoulder", "Right shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"dpad_up", "D-pad Up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpad_down", "D-pad Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpad_left", "D-pad Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpad_right", "D-pad Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"misc1", "Misc 1 / Share", SDL_GAMEPAD_BUTTON_MISC1},
    {"right_paddle1", "Right paddle 1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {"left_paddle1", "Left paddle 1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"right_paddle2", "Right paddle 2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"left_paddle2", "Left paddle 2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"touchpad", "Touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"misc2", "Misc 2", SDL_GAMEPAD_BUTTON_MISC2},
    {"misc3", "Misc 3 / GC L click", SDL_GAMEPAD_BUTTON_MISC3},
    {"misc4", "Misc 4 / GC R click", SDL_GAMEPAD_BUTTON_MISC4},
    {"misc5", "Misc 5", SDL_GAMEPAD_BUTTON_MISC5},
    {"misc6", "Misc 6", SDL_GAMEPAD_BUTTON_MISC6},
}};

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// SDL presents an attached Nunchuk as the left stick, C as left shoulder, and
// Z as the left-trigger axis. Leave Classic L without a digital binding so
// Aurora's normal analog-trigger emulation turns Nunchuk Z into the item
// button. Wii Remote B becomes Classic R (drift); button 1 provides Classic B
// for menu-back/brake without making every drift press brake simultaneously.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kWiimoteNunchukPreset = {
    "east",          // A: Wii Remote A
    "west",          // B: Wii Remote 1
    "left_shoulder", // X: Nunchuk C / look behind
    "north",         // Y: Wii Remote 2
    "start",         // Start: Plus
    "back",          // Z: Minus
    "unmapped",      // L: Nunchuk Z arrives on the left-trigger axis
    "south",         // R: Wii Remote B / drift
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

const NativeButtonItem* FindNativeButton(std::string value) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(), [&](const NativeButtonItem& item) {
        return value == item.configName;
    });
    return it == kNativeButtons.end() ? nullptr : &*it;
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};

std::string TrimBindingToken(const std::string& token) {
    const size_t begin = token.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = token.find_last_not_of(" \t");
    return token.substr(begin, end - begin + 1);
}

// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {TrimBindingToken(value), {}};
    }
    return {TrimBindingToken(value.substr(0, comma)), TrimBindingToken(value.substr(comma + 1))};
}

const NativeButtonItem& NativeButtonForValue(uint32_t nativeButton) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(), [&](const NativeButtonItem& item) {
        return nativeButton == item.nativeButton;
    });
    return it == kNativeButtons.end() ? kNativeButtons.front() : *it;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = native->nativeButton;
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

void DrawGameCubeAdapterInfo() {
    ImGui::Separator();
    if (!ImGui::BeginMenu("GameCube adapter info")) return;

    const auto adapter = Wup028Adapter::GetInfo();
    const char* state = adapter.state == Wup028Adapter::ConnectionState::Connected
                            ? "Connected"
                            : adapter.state == Wup028Adapter::ConnectionState::DriverError ? "Driver error"
                                                                                           : "Searching";
    ImGui::Text("Status: %s", state);
    if (!adapter.deviceName.empty()) {
        ImGui::Text("Device: %s", adapter.deviceName.c_str());
    }
    ImGui::TextWrapped("%s", adapter.detail.c_str());
    if (adapter.state == Wup028Adapter::ConnectionState::Connected) {
        ImGui::Text("Poll rate: %.1f reports/s", adapter.pollRateHz);
        ImGui::Text("Endpoints: IN 0x%02X, OUT 0x%02X", adapter.inputEndpoint, adapter.outputEndpoint);
        for (size_t port = 0; port < adapter.ports.size(); ++port) {
            const uint8_t type = adapter.portStatus[port] & 0x30;
            const char* typeName = type == 0x10 ? "wired" : type == 0x20 ? "wireless" : "none";
            ImGui::Text("Adapter port %u: %s (type %s, raw 0x%02X)", static_cast<unsigned>(port + 1),
                        adapter.ports[port] ? "Controller connected" : "Empty", typeName,
                        adapter.portStatus[port]);
        }
    }
    ImGui::EndMenu();
}

void DrawControllerSettings() {
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    const int adapterAssignment = Wup028Adapter::GetPortAssignment(selectedGamePort);
    if (adapterAssignment >= 0) {
        ImGui::Text("Assigned: GameCube adapter port %d", adapterAssignment + 1);
    } else {
        const char* currentName = PADGetName(selectedGamePort);
        ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    }
    if (ImGui::BeginMenu("Assign GameCube adapter port")) {
        if (ImGui::MenuItem("None", nullptr, adapterAssignment < 0)) {
            Wup028Adapter::SetPortAssignment(selectedGamePort, -1);
            RuntimeConfigFile::SetGameCubeAdapterPort(selectedGamePort, -1);
        }
        const auto adapter = Wup028Adapter::GetInfo();
        for (int physicalPort = 0; physicalPort < PAD_CHANMAX; ++physicalPort) {
            const std::string label = "Adapter port " + std::to_string(physicalPort + 1) +
                (adapter.ports[static_cast<size_t>(physicalPort)] ? " (connected)" : " (empty)");
            if (ImGui::MenuItem(label.c_str(), nullptr, adapterAssignment == physicalPort)) {
                for (uint32_t gamePort = 0; gamePort < PAD_CHANMAX; ++gamePort) {
                    if (gamePort != selectedGamePort && Wup028Adapter::GetPortAssignment(gamePort) == physicalPort) {
                        RuntimeConfigFile::SetGameCubeAdapterPort(gamePort, -1);
                    }
                }
                PADClearPort(selectedGamePort);
                Wup028Adapter::SetPortAssignment(selectedGamePort, physicalPort);
                RuntimeConfigFile::SetGameCubeAdapterPort(selectedGamePort, physicalPort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Unassign controller")) {
        PADClearPort(selectedGamePort);
        Wup028Adapter::SetPortAssignment(selectedGamePort, -1);
        RuntimeConfigFile::SetGameCubeAdapterPort(selectedGamePort, -1);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        DrawGameCubeAdapterInfo();
        return;
    }

    if (ImGui::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::MenuItem(name != nullptr ? name : "Unknown controller")) {
                Wup028Adapter::SetPortAssignment(selectedGamePort, -1);
                RuntimeConfigFile::SetGameCubeAdapterPort(selectedGamePort, -1);
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        DrawGameCubeAdapterInfo();
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeButtonForValue(primaryNative).configName;
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeButtonForValue(altNative).configName;
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(kClassicProPreset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, kClassicProPreset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Wii Remote + Nunchuk (Experimental)")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(kWiimoteNunchukPreset[i])) {
                PADSetButtonMapping(port,
                    PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                    PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, kWiimoteNunchukPreset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }

    ImGui::SeparatorText("Button mapping");
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo("##primary", current.label)) {
            for (const auto& candidate : kNativeButtons) {
                const bool selected = candidate.nativeButton == mappingIt->nativeButton;
                if (ImGui::Selectable(candidate.label, selected)) {
                    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
                    PADSetButtonMapping(port, PADButtonMapping{candidate.nativeButton, kControllerButtons[i].padButton});
                    writeBinding(i, candidate.nativeButton,
                                 altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                    PADSerializeMappings();
                    mappings = PADGetButtonMappings(port, &mappingCount);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(190.0f);
                if (ImGui::BeginCombo("##alt", altLabel)) {
                    for (const auto& candidate : kNativeButtons) {
                        const bool isNone = candidate.nativeButton == PAD_NATIVE_BUTTON_INVALID;
                        const bool selected = candidate.nativeButton == altIt->nativeButton;
                        if (ImGui::Selectable(isNone ? "None" : candidate.label, selected)) {
                            const uint32_t port = static_cast<uint32_t>(g_controllerPort);
                            PADSetAltButtonMapping(
                                port, PADButtonMapping{candidate.nativeButton, kControllerButtons[i].padButton});
                            writeBinding(i, mappingIt->nativeButton, candidate.nativeButton);
                            if (isNone) {
                                altRowExpanded[i] = false;
                            }
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawGameCubeAdapterInfo();
}

#if defined(__ANDROID__)
float g_androidFpsOverlayScale = 1.0f;
#endif

void DrawFpsOverlay() {
    static uint64_t lastTelemetryPresentCount = 0;
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    if (presentTiming.sampleCount > 0 &&
        presentTiming.totalPresentCount >= lastTelemetryPresentCount + 300) {
        const AuroraStats* stats = aurora_get_stats();
        RT_LOGF(RT_TAG_GX,
                "present telemetry: total=%llu samples=%u avg-ms=%.3f p50-ms=%.3f "
                "p95-ms=%.3f p99-ms=%.3f worst-ms=%.3f jitter-ms=%.3f fps=%.3f "
                "effective-fps=%.3f pipelines-queued=%u pipelines-created=%u\n",
                static_cast<unsigned long long>(presentTiming.totalPresentCount),
                presentTiming.sampleCount,
                presentTiming.averageFrameTimeMs,
                presentTiming.p50FrameTimeMs,
                presentTiming.p95FrameTimeMs,
                presentTiming.p99FrameTimeMs,
                presentTiming.worstFrameTimeMs,
                presentTiming.jitterMs,
                presentTiming.framesPerSecond,
                presentTiming.effectiveFramesPerSecond,
                stats != nullptr ? stats->queuedPipelines : 0,
                stats != nullptr ? stats->createdPipelines : 0);
        lastTelemetryPresentCount = presentTiming.totalPresentCount;
    }
    if (!g_showFps) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_compatibilityVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
#if defined(__ANDROID__)
        ImGui::SetWindowFontScale(g_androidFpsOverlayScale);
#endif
        if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
#if defined(__ANDROID__)
            ImGui::Text("Frame ms: p50 %.1f  p95 %.1f",
                        presentTiming.p50FrameTimeMs, presentTiming.p95FrameTimeMs);
            ImGui::Text("p99 %.1f  worst %.1f",
                        presentTiming.p99FrameTimeMs, presentTiming.worstFrameTimeMs);
#else
            ImGui::Text("Frame ms: p50 %.1f  p95 %.1f  p99 %.1f  worst %.1f",
                        presentTiming.p50FrameTimeMs, presentTiming.p95FrameTimeMs,
                        presentTiming.p99FrameTimeMs, presentTiming.worstFrameTimeMs);
#endif
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_compatibilityVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("KartPad Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        constexpr const char* kTitle = "KartPad";
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawControllerCompatibility() {
    if (g_nativeCompatibilityRequest.exchange(false)) g_compatibilityVisible = true;
    if (!g_compatibilityVisible) return;
    ImGui::SetNextWindowSize(ImVec2(760, 680), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Controller Compatibility Tools", &g_compatibilityVisible)) {
        ImGui::TextWrapped("Legacy setup for unmapped devices, adapter ports and presets. "
                           "Use KartPad Settings for standard controllers, graphics and audio.");
        ImGui::Separator();
        DrawControllerSettings();
        if (ImGui::Button("Close")) g_compatibilityVisible = false;
    }
    ImGui::End();
}

void ReloadNativeSettings() {
    if (!g_nativeSettingsReload.exchange(false)) return;
    const auto c = RuntimeConfigFile::LoadConfigFile();
    RuntimeConfigFile::Mutable() = c;
    const float resolution = c.resolutionMultiplier.value_or(1.0f);
    if (resolution != g_resolutionScale) {
        g_resolutionScale = resolution;
        VISetFrameBufferScale(resolution);
    }
    const uint32_t fps = c.frameInterpolationFps.value_or(0);
    const int frameMode = fps == 120 ? 1 : fps == 180 ? 2 : 0;
    const bool frameChanged = frameMode != g_frameInterpolationMode;
    g_frameInterpolationMode = frameMode;
    LimitResolutionForFrameRate();
    if (frameChanged) aurora_set_frame_interpolation_fps(kFrameInterpolationTargetFps[frameMode]);
    const auto mode = c.displayMode.value_or("windowed");
    const int display = mode == "borderless" ? 1 : mode == "exclusive" ? 2 : 0;
    if (display != g_displayMode || (frameChanged && display == AURORA_DISPLAY_MODE_EXCLUSIVE)) {
        aurora_set_display_mode(static_cast<AuroraDisplayMode>(display));
        g_displayMode = static_cast<int>(aurora_get_display_mode());
        if (g_displayMode != display)
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[g_displayMode]));
    }
    g_showFps = c.showFps.value_or(true);
    g_disableCopyFilter = c.disableCopyFilter.value_or(true);
    g_skipUnreadyPipelines = c.skipUnreadyPipelines.value_or(true);
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_disabledPostProcessingPaths = c.disabledPostProcessingPaths.value_or(0);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
    const auto percent = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 100)); };
    g_audioVolumePercent = percent(c.audioVolume.value_or(1));
    g_musicVolumePercent = percent(c.audioMusicVolume.value_or(1));
    g_soundEffectsVolumePercent = percent(c.audioSoundEffectsVolume.value_or(1));
    g_uiVolumePercent = percent(c.audioUiVolume.value_or(1));
    g_voicesVolumePercent = percent(c.audioVoicesVolume.value_or(1));
    g_audioMuted = c.audioMuted.value_or(false);
    AudioBackend::Instance().SetMasterVolume(g_audioVolumePercent / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(g_musicVolumePercent / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(g_soundEffectsVolumePercent / 100.0f);
    MusicAttenuation::SetUiVolume(g_uiVolumePercent / 100.0f);
    MusicAttenuation::SetVoicesVolume(g_voicesVolumePercent / 100.0f);
    if (g_audioMixWorker != c.audioMixWorker.value_or(true)) {
        g_audioMixWorker = c.audioMixWorker.value_or(true);
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
    }
}

// Persist display changes made by the platform fullscreen shortcut.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    PADBlockInput(g_compatibilityVisible);
    SDL_ShowCursor();
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        if (kartpad::IsMacSettingsShortcut(ev->sdl)) {
            KartPadOpenSettingsFromShortcut();
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
    }
}

void Draw() noexcept {
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    ReloadNativeSettings();
    ApplyConfiguredMappings();
#if defined(__APPLE__) && TARGET_OS_OSX
    KartPadControllersTick();
#endif
    PersistDisplayModeIfChanged();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    DrawControllerCompatibility();
    controller_mapping_wizard::Draw();
    // The wizard captures raw presses; keep them out of the game even when the
    // compatibility window is closed mid-setup.
    PADBlockInput(g_compatibilityVisible || controller_mapping_wizard::IsActive()
#if defined(__APPLE__) && TARGET_OS_OSX
                  || KartPadControllersVisible()
#endif
    );
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
