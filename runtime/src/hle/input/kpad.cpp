#include "hle_stubs.h"
#include "memory.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "wii_remote_input.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int g_gxFrameCount;

namespace {
constexpr uint32_t kKpadStatusSize = 0x84;

// Exact sizes and offsets from Mario Kart's own byte-matching Revolution SDK
// headers (KPADStatus 0x84, KPADUnifiedWpadStatus 0x38).

constexpr uint32_t kKpadUnifiedStatusSize = 0x38;
constexpr uint64_t kSyntheticHoldNs = 500'000'000;
constexpr uint64_t kSyntheticMenuHoldNs = 20'000'000;
constexpr uint64_t kSyntheticStickHoldNs = 250'000'000;
constexpr uint8_t kWpadExtensionClassic = 0x02;
constexpr uint8_t kWpadFormatClassic = 0x06;
constexpr uint32_t kWpadButtonLeft = 0x0001;
constexpr uint32_t kWpadButtonRight = 0x0002;
constexpr uint32_t kWpadButtonDown = 0x0004;
constexpr uint32_t kWpadButtonUp = 0x0008;
constexpr uint32_t kWpadButtonPlus = 0x0010;
constexpr uint32_t kWpadButtonTwo = 0x0100;
constexpr uint32_t kWpadButtonOne = 0x0200;
constexpr uint32_t kWpadButtonB = 0x0400;
constexpr uint32_t kWpadButtonA = 0x0800;
constexpr uint32_t kWpadButtonMinus = 0x1000;

constexpr uint32_t kClassicButtonUp = 0x00000001;
constexpr uint32_t kClassicButtonLeft = 0x00000002;
constexpr uint32_t kClassicButtonZr = 0x00000004;
constexpr uint32_t kClassicButtonX = 0x00000008;
constexpr uint32_t kClassicButtonA = 0x00000010;
constexpr uint32_t kClassicButtonY = 0x00000020;
constexpr uint32_t kClassicButtonB = 0x00000040;
constexpr uint32_t kClassicButtonZl = 0x00000080;
constexpr uint32_t kClassicButtonR = 0x00000200;
constexpr uint32_t kClassicButtonPlus = 0x00000400;
constexpr uint32_t kClassicButtonMinus = 0x00001000;
constexpr uint32_t kClassicButtonL = 0x00002000;
constexpr uint32_t kClassicButtonDown = 0x00004000;
constexpr uint32_t kClassicButtonRight = 0x00008000;

std::array<uint32_t, 4> g_previousButtons{};
std::array<uint32_t, 4> g_previousClassicButtons{};
std::array<std::atomic<uint32_t>, 4> g_pendingPresses{};
std::array<std::atomic<uint32_t>, 4> g_pendingClassicPresses{};
std::array<std::atomic<bool>, 4> g_keyboardConnected{};
std::atomic<bool> g_fixtureArmRequested{false};
std::atomic<bool> g_preciseMenuPulseRequested{false};
std::array<std::atomic<uint64_t>, SDL_SCANCODE_COUNT> g_syntheticExpiryNs{};

bool PreciseMenuPulseEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("KARTPAD_PRECISE_MENU_PULSE_V2");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled || g_preciseMenuPulseRequested.load(std::memory_order_acquire);
}

bool FullSyntheticStickEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("KARTPAD_FULL_SYNTHETIC_STICK_V2");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct GhostSample {
    uint8_t face = 0;
    uint8_t direction = 0x77;
    uint8_t trick = 0;
};

class RkgInputFixture {
public:
    bool EnsureLoaded()
    {
        Load();
        return m_ready;
    }

    uint8_t VehicleId() const { return m_vehicleId; }
    uint8_t CharacterId() const { return m_characterId; }
    uint8_t CourseId() const { return m_courseId; }
    bool DriftIsAuto() const { return m_driftIsAuto; }

    bool Arm(bool resume = false)
    {
        Load();
        if (!m_ready) {
            return false;
        }
        m_frame = resume ? std::min(m_resumeFrame, m_face.size()) : 0;
        m_current = {};
        m_started = false;
        m_postStreamFrames = 0;
        m_finishDispatched = false;
        m_lastPresentationFrame = -1;
        m_active = true;
        std::fprintf(stderr,
                     "[input-fixture] armed RKG course=%u frames=%zu/%zu/%zu\n",
                     m_courseId, m_face.size(), m_direction.size(), m_trick.size());
        return true;
    }

    GhostSample Next()
    {
        if (!m_active) {
            return {};
        }
        m_current.face = m_frame < m_face.size() ? m_face[m_frame] : 0;
        m_current.direction =
            m_frame < m_direction.size() ? m_direction[m_frame] : 0x77;
        m_current.trick = m_frame < m_trick.size() ? m_trick[m_frame] : 0;
        if (m_frame < m_face.size()) {
            ++m_frame;
            if (m_frame == m_face.size()) {
                std::fprintf(stderr,
                             "[input-fixture] face stream complete at frame=%zu\n",
                             m_frame);
            }
        } else {
            ++m_postStreamFrames;
        }
        return m_current;
    }

    GhostSample NextOncePerPresentationFrame()
    {
        if (m_lastPresentationFrame == g_gxFrameCount) {
            return m_current;
        }
        m_lastPresentationFrame = g_gxFrameCount;
        return Next();
    }

    const GhostSample& Current() const { return m_current; }
    bool Active() const { return m_active; }
    bool ConsumeFinishRequest()
    {
        if (m_finishDispatched) return false;
        const char* forceFinish = std::getenv("KARTPAD_RKG_FORCE_FINISH_V2");
        if (forceFinish == nullptr || std::strcmp(forceFinish, "1") != 0) {
            return false;
        }
        const char* finishFrameValue =
            std::getenv("KARTPAD_RKG_FORCE_FINISH_FRAME_V2");
        const size_t finishFrame = finishFrameValue == nullptr ? 0 :
            static_cast<size_t>(std::strtoull(finishFrameValue, nullptr, 10));
        if (finishFrame != 0 ? m_frame < finishFrame : m_postStreamFrames < 300) {
            return false;
        }
        m_finishDispatched = true;
        return true;
    }
    size_t Frame() const { return m_frame; }
    size_t PostStreamFrames() const { return m_postStreamFrames; }
    bool AutoArmAtCountdown(uint32_t stage)
    {
        const char* autoStart = std::getenv("KARTPAD_RKG_AUTOSTART_V2");
        if (stage != 1 || autoStart == nullptr || std::strcmp(autoStart, "1") != 0) {
            return false;
        }
        return Arm();
    }
    bool BeginIfRaceActive(uint32_t stage)
    {
        if (!m_started && (stage == 1 || stage == 2)) {
            m_started = true;
            std::fprintf(stderr,
                         "[input-fixture] synchronized at RaceManager stage=%u\n",
                         stage);
        }
        return m_started;
    }
    void Disarm()
    {
        if (!m_active) return;
        m_resumeFrame = m_frame;
        m_active = false;
        m_current = {};
        std::fprintf(stderr, "[input-fixture] disarmed at frame=%zu\n", m_frame);
    }

private:
    static uint16_t ReadBe16(const std::vector<uint8_t>& data, size_t offset)
    {
        return static_cast<uint16_t>((static_cast<uint16_t>(data.at(offset)) << 8) |
                                     data.at(offset + 1));
    }

    static uint32_t ReadBe32(const std::vector<uint8_t>& data, size_t offset)
    {
        return (static_cast<uint32_t>(data.at(offset)) << 24) |
               (static_cast<uint32_t>(data.at(offset + 1)) << 16) |
               (static_cast<uint32_t>(data.at(offset + 2)) << 8) |
               data.at(offset + 3);
    }

    static bool DecodeYaz(const std::vector<uint8_t>& source, size_t offset,
                          size_t sourceSize, std::vector<uint8_t>& output)
    {
        if (sourceSize < 16 || offset + sourceSize > source.size() ||
            source[offset] != 'Y' || source[offset + 1] != 'a' ||
            source[offset + 2] != 'z') {
            return false;
        }
        const uint32_t expandedSize = ReadBe32(source, offset + 4);
        size_t sourceIndex = offset + 16;
        const size_t sourceEnd = offset + sourceSize;
        uint32_t flags = 0;
        uint32_t mask = 0;
        output.clear();
        output.reserve(expandedSize);

        while (output.size() < expandedSize) {
            if (mask == 0) {
                if (sourceIndex >= sourceEnd) return false;
                flags = source[sourceIndex++];
                mask = 0x80;
            }
            if ((flags & mask) != 0) {
                if (sourceIndex >= sourceEnd) return false;
                output.push_back(source[sourceIndex++]);
            } else {
                if (sourceIndex + 2 > sourceEnd) return false;
                const uint32_t repeat =
                    (static_cast<uint32_t>(source[sourceIndex]) << 8) |
                    source[sourceIndex + 1];
                sourceIndex += 2;
                const size_t distance = (repeat & 0x0fff) + 1;
                if (distance > output.size()) return false;
                size_t count = repeat >> 12;
                if (count != 0) {
                    count += 2;
                } else {
                    if (sourceIndex >= sourceEnd) return false;
                    count = static_cast<size_t>(source[sourceIndex++]) + 18;
                }
                if (output.size() + count > expandedSize) return false;
                for (size_t i = 0; i < count; ++i) {
                    output.push_back(output[output.size() - distance]);
                }
            }
            mask >>= 1;
        }
        return true;
    }

    static void ExpandByteStream(const std::vector<uint8_t>& inputs, size_t offset,
                                 size_t count, std::vector<uint8_t>& output)
    {
        output.clear();
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = inputs.at(offset + i * 2);
            const size_t frames = std::max<size_t>(
                1, inputs.at(offset + i * 2 + 1));
            output.insert(output.end(), frames, value);
        }
    }

    static void ExpandTrickStream(const std::vector<uint8_t>& inputs, size_t offset,
                                  size_t count, std::vector<uint8_t>& output)
    {
        output.clear();
        for (size_t i = 0; i < count; ++i) {
            const uint16_t sequence = ReadBe16(inputs, offset + i * 2);
            const uint8_t value = static_cast<uint8_t>(sequence >> 12);
            const size_t frames = std::max<size_t>(1, sequence & 0x0fff);
            output.insert(output.end(), frames, value);
        }
    }

    void Load()
    {
        if (m_loadAttempted) return;
        m_loadAttempted = true;
        const char* path = std::getenv("KARTPAD_RKG_INPUT_V2");
        if (path == nullptr || *path == '\0') return;

        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            std::fprintf(stderr, "[input-fixture] unable to open configured RKG\n");
            return;
        }
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)),
                                        std::istreambuf_iterator<char>());
        try {
            if (file.size() < 0x90 || file[0] != 'R' || file[1] != 'K' ||
                file[2] != 'G' || file[3] != 'D') {
                throw std::runtime_error("invalid RKG header");
            }
            const uint32_t race = ReadBe32(file, 4);
            const uint32_t identity = ReadBe32(file, 8);
            const uint16_t flags = ReadBe16(file, 0x0c);
            m_courseId = static_cast<uint8_t>((race >> 2) & 0x3f);
            m_vehicleId = static_cast<uint8_t>(identity >> 26);
            m_characterId = static_cast<uint8_t>((identity >> 20) & 0x3f);
            m_driftIsAuto = (flags & 0x0002) != 0;
            const size_t inputSize = ReadBe16(file, 0x0e);
            std::vector<uint8_t> inputs;
            if (file[0x8c] == 'Y' && file[0x8d] == 'a' && file[0x8e] == 'z') {
                const size_t compressedSize = ReadBe32(file, 0x88);
                if (!DecodeYaz(file, 0x8c, compressedSize, inputs)) {
                    throw std::runtime_error("invalid Yaz input stream");
                }
            } else {
                if (0x88 + inputSize > file.size()) {
                    throw std::runtime_error("truncated uncompressed input stream");
                }
                inputs.assign(file.begin() + 0x88, file.begin() + 0x88 + inputSize);
            }
            if (inputs.size() != inputSize || inputs.size() < 8) {
                throw std::runtime_error("RKG input size mismatch");
            }

            const size_t faceCount = ReadBe16(inputs, 0);
            const size_t directionCount = ReadBe16(inputs, 2);
            const size_t trickCount = ReadBe16(inputs, 4);
            if (8 + 2 * (faceCount + directionCount + trickCount) != inputs.size()) {
                throw std::runtime_error("RKG stream table mismatch");
            }
            const size_t faceOffset = 8;
            const size_t directionOffset = faceOffset + 2 * faceCount;
            const size_t trickOffset = directionOffset + 2 * directionCount;
            ExpandByteStream(inputs, faceOffset, faceCount, m_face);
            ExpandByteStream(inputs, directionOffset, directionCount, m_direction);
            ExpandTrickStream(inputs, trickOffset, trickCount, m_trick);
            m_ready = true;
            std::fprintf(stderr,
                         "[input-fixture] loaded configured RKG course=%u vehicle=%u character=%u drift=%s sequences=%zu/%zu/%zu\n",
                         m_courseId, m_vehicleId, m_characterId,
                         m_driftIsAuto ? "automatic" : "manual",
                         faceCount, directionCount, trickCount);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[input-fixture] rejected configured RKG: %s\n",
                         error.what());
        }
    }

    bool m_loadAttempted = false;
    bool m_ready = false;
    bool m_active = false;
    bool m_started = false;
    uint8_t m_courseId = 0;
    uint8_t m_vehicleId = 0;
    uint8_t m_characterId = 0;
    bool m_driftIsAuto = false;
    size_t m_frame = 0;
    size_t m_resumeFrame = 0;
    size_t m_postStreamFrames = 0;
    bool m_finishDispatched = false;
    int m_lastPresentationFrame = -1;
    GhostSample m_current{};
    std::vector<uint8_t> m_face;
    std::vector<uint8_t> m_direction;
    std::vector<uint8_t> m_trick;
};

RkgInputFixture& Fixture()
{
    static RkgInputFixture fixture;
    return fixture;
}

bool ForceFixtureMetadataEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("KARTPAD_RKG_FORCE_METADATA_V2");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled && Fixture().EnsureLoaded();
}

uint32_t ButtonForScancode(uint32_t chan, SDL_Scancode code)
{
    if (chan == 1) {
        switch (code) {
        case SDL_SCANCODE_R: return kWpadButtonA;
        case SDL_SCANCODE_F: return kWpadButtonB;
        case SDL_SCANCODE_2: return kWpadButtonPlus;
        case SDL_SCANCODE_T: return kWpadButtonLeft;
        case SDL_SCANCODE_Y: return kWpadButtonRight;
        case SDL_SCANCODE_H: return kWpadButtonDown;
        case SDL_SCANCODE_G: return kWpadButtonUp;
        default: return 0;
        }
    }
    if (chan == 2) {
        switch (code) {
        case SDL_SCANCODE_O: return kWpadButtonA;
        case SDL_SCANCODE_P: return kWpadButtonB;
        case SDL_SCANCODE_3: return kWpadButtonPlus;
        case SDL_SCANCODE_J: return kWpadButtonLeft;
        case SDL_SCANCODE_L: return kWpadButtonRight;
        case SDL_SCANCODE_K: return kWpadButtonDown;
        case SDL_SCANCODE_I: return kWpadButtonUp;
        default: return 0;
        }
    }
    if (chan == 3) {
        switch (code) {
        case SDL_SCANCODE_X: return kWpadButtonA;
        case SDL_SCANCODE_C: return kWpadButtonB;
        case SDL_SCANCODE_4: return kWpadButtonPlus;
        case SDL_SCANCODE_Z: return kWpadButtonLeft;
        case SDL_SCANCODE_V: return kWpadButtonRight;
        case SDL_SCANCODE_N: return kWpadButtonDown;
        case SDL_SCANCODE_B: return kWpadButtonUp;
        default: return 0;
        }
    }
    switch (code) {
    case SDL_SCANCODE_U: return kWpadButtonA;
    case SDL_SCANCODE_M: return kWpadButtonB;
    case SDL_SCANCODE_RETURN: return kWpadButtonA;
    case SDL_SCANCODE_BACKSPACE: return kWpadButtonB;
    case SDL_SCANCODE_SPACE: return kWpadButtonPlus;
    case SDL_SCANCODE_Q: return kWpadButtonOne;
    case SDL_SCANCODE_E: return kWpadButtonTwo;
    case SDL_SCANCODE_ESCAPE: return kWpadButtonA;
    case SDL_SCANCODE_LEFT: return kWpadButtonLeft;
    case SDL_SCANCODE_RIGHT: return kWpadButtonRight;
    case SDL_SCANCODE_DOWN: return kWpadButtonDown;
    case SDL_SCANCODE_UP: return kWpadButtonUp;
    default: return 0;
    }
}

uint32_t ClassicButtonForScancode(uint32_t chan, SDL_Scancode code)
{
    const uint32_t core = ButtonForScancode(chan, code);
    uint32_t buttons = 0;
    if ((core & kWpadButtonA) != 0) buttons |= kClassicButtonA;
    if ((core & kWpadButtonB) != 0) buttons |= kClassicButtonB;
    if ((core & kWpadButtonPlus) != 0) buttons |= kClassicButtonPlus;
    if ((core & kWpadButtonLeft) != 0) buttons |= kClassicButtonLeft;
    if ((core & kWpadButtonRight) != 0) buttons |= kClassicButtonRight;
    if ((core & kWpadButtonDown) != 0) buttons |= kClassicButtonDown;
    if ((core & kWpadButtonUp) != 0) buttons |= kClassicButtonUp;
    if (chan != 0) {
        if (chan == 1 && code == SDL_SCANCODE_V) buttons |= kClassicButtonZl;
        if (chan == 1 && code == SDL_SCANCODE_B) buttons |= kClassicButtonZr;
        if (chan == 2 && code == SDL_SCANCODE_N) buttons |= kClassicButtonZl;
        if (chan == 2 && code == SDL_SCANCODE_M) buttons |= kClassicButtonZr;
        return buttons;
    }
    switch (code) {
    case SDL_SCANCODE_U: return kClassicButtonA;
    case SDL_SCANCODE_M: return kClassicButtonB;
    case SDL_SCANCODE_RETURN: return kClassicButtonA;
    case SDL_SCANCODE_BACKSPACE: return kClassicButtonB;
    case SDL_SCANCODE_SPACE: return kClassicButtonPlus;
    case SDL_SCANCODE_Q: return kClassicButtonB;
    case SDL_SCANCODE_E: return kClassicButtonR;
    case SDL_SCANCODE_LSHIFT: return kClassicButtonL;
    case SDL_SCANCODE_TAB: return kClassicButtonMinus;
    case SDL_SCANCODE_ESCAPE: return kClassicButtonA;
    case SDL_SCANCODE_LEFT: return kClassicButtonLeft;
    case SDL_SCANCODE_RIGHT: return kClassicButtonRight;
    case SDL_SCANCODE_DOWN: return kClassicButtonDown;
    case SDL_SCANCODE_UP: return kClassicButtonUp;
    case SDL_SCANCODE_X: return kClassicButtonX;
    case SDL_SCANCODE_C: return kClassicButtonY;
    case SDL_SCANCODE_Z: return kClassicButtonZl;
    case SDL_SCANCODE_V: return kClassicButtonZr;
    default: return 0;
    }
}

bool SDLCALL WatchKeyboardEvent(void*, SDL_Event* event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        if (event->key.scancode == SDL_SCANCODE_F6) {
            const bool enabled = !g_preciseMenuPulseRequested.load(std::memory_order_acquire);
            g_preciseMenuPulseRequested.store(enabled, std::memory_order_release);
            std::fprintf(stderr, "[input] precise menu pulse %s\n",
                         enabled ? "enabled" : "disabled");
            return true;
        }
        if (event->key.scancode == SDL_SCANCODE_E) {
            g_fixtureArmRequested.store(true, std::memory_order_release);
        }
        if (event->key.scancode == SDL_SCANCODE_7 ||
            event->key.scancode == SDL_SCANCODE_8 ||
            event->key.scancode == SDL_SCANCODE_9) {
            const uint32_t chan = static_cast<uint32_t>(event->key.scancode - SDL_SCANCODE_7) + 1;
            g_keyboardConnected[chan].store(false, std::memory_order_release);
            g_pendingPresses[chan].store(0, std::memory_order_release);
            g_pendingClassicPresses[chan].store(0, std::memory_order_release);
            g_previousButtons[chan] = 0;
            g_previousClassicButtons[chan] = 0;
            std::fprintf(stderr, "[input] keyboard channel %u disconnected\n", chan);
        }
        for (uint32_t chan = 0; chan < g_pendingPresses.size(); ++chan) {
            const uint32_t core = ButtonForScancode(chan, event->key.scancode);
            const uint32_t classic = ClassicButtonForScancode(chan, event->key.scancode);
            if (chan != 0 && (core & kWpadButtonPlus) != 0 &&
                !g_keyboardConnected[chan].exchange(true, std::memory_order_acq_rel)) {
                std::fprintf(stderr, "[input] keyboard channel %u connected\n", chan);
            }
            if (chan == 0 || g_keyboardConnected[chan].load(std::memory_order_acquire)) {
                g_pendingPresses[chan].fetch_or(core, std::memory_order_release);
                g_pendingClassicPresses[chan].fetch_or(classic, std::memory_order_release);
            }
        }
        const auto scancode = static_cast<size_t>(event->key.scancode);
        if (scancode < g_syntheticExpiryNs.size()) {
            const bool menuKey = event->key.scancode == SDL_SCANCODE_RETURN ||
                event->key.scancode == SDL_SCANCODE_BACKSPACE ||
                event->key.scancode == SDL_SCANCODE_SPACE ||
                event->key.scancode == SDL_SCANCODE_Q ||
                event->key.scancode == SDL_SCANCODE_E ||
                event->key.scancode == SDL_SCANCODE_LEFT ||
                event->key.scancode == SDL_SCANCODE_RIGHT ||
                event->key.scancode == SDL_SCANCODE_DOWN ||
                event->key.scancode == SDL_SCANCODE_UP;
            const bool stickKey = event->key.scancode == SDL_SCANCODE_A ||
                event->key.scancode == SDL_SCANCODE_D ||
                event->key.scancode == SDL_SCANCODE_W ||
                event->key.scancode == SDL_SCANCODE_S ||
                event->key.scancode == SDL_SCANCODE_T ||
                event->key.scancode == SDL_SCANCODE_Y ||
                event->key.scancode == SDL_SCANCODE_G ||
                event->key.scancode == SDL_SCANCODE_H ||
                event->key.scancode == SDL_SCANCODE_I ||
                event->key.scancode == SDL_SCANCODE_J ||
                event->key.scancode == SDL_SCANCODE_K ||
                event->key.scancode == SDL_SCANCODE_L ||
                event->key.scancode == SDL_SCANCODE_Z ||
                event->key.scancode == SDL_SCANCODE_V ||
                event->key.scancode == SDL_SCANCODE_B ||
                event->key.scancode == SDL_SCANCODE_N;
            const uint64_t holdNs = menuKey && PreciseMenuPulseEnabled() ? 0 :
                menuKey ? kSyntheticMenuHoldNs :
                stickKey ? kSyntheticStickHoldNs : kSyntheticHoldNs;
            g_syntheticExpiryNs[scancode].store(SDL_GetTicksNS() + holdNs,
                                                std::memory_order_release);
        }
    }
    return true;
}

void EnsureKeyboardWatch()
{
    static const bool installed = SDL_AddEventWatch(WatchKeyboardEvent, nullptr);
    (void)installed;
}

bool IsKeyDown(const bool* keys, int keyCount, SDL_Scancode code)
{
    const auto scancode = static_cast<size_t>(code);
    const bool physicallyDown = static_cast<int>(code) < keyCount && keys[code];
    if (physicallyDown) return true;
    const uint64_t expiry = scancode < g_syntheticExpiryNs.size() ?
        g_syntheticExpiryNs[scancode].load(std::memory_order_acquire) : 0;
    // Most scancodes have never received a synthetic event. Their zero expiry
    // cannot be active, so avoid a clock read for each of them on every poll.
    return expiry != 0 && expiry > SDL_GetTicksNS();
}

float ReadKeyboardAxisLevel(const bool* keys, int keyCount, SDL_Scancode code)
{
    if (static_cast<int>(code) < keyCount && keys[code]) return 1.0f;
    const auto scancode = static_cast<size_t>(code);
    const uint64_t expiry = scancode < g_syntheticExpiryNs.size() ?
        g_syntheticExpiryNs[scancode].load(std::memory_order_acquire) : 0;
    const bool syntheticallyDown = expiry != 0 && expiry > SDL_GetTicksNS();
    return syntheticallyDown ? (FullSyntheticStickEnabled() ? 1.0f : 0.35f) : 0.0f;
}

uint32_t ReadKeyboardButtons(uint32_t chan)
{
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const auto down = [&](SDL_Scancode code) { return IsKeyDown(keys, keyCount, code); };

    uint32_t buttons = 0;
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
        const auto scancode = static_cast<SDL_Scancode>(code);
        if (down(scancode)) buttons |= ButtonForScancode(chan, scancode);
    }
    return buttons;
}

uint32_t ReadClassicButtons(uint32_t chan)
{
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    uint32_t buttons = 0;
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
        const auto scancode = static_cast<SDL_Scancode>(code);
        if (IsKeyDown(keys, keyCount, scancode)) {
            buttons |= ClassicButtonForScancode(chan, scancode);
        }
    }
    return buttons;
}

std::array<float, 2> ReadClassicLeftStick(uint32_t chan)
{
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const std::array<SDL_Scancode, 4> profile = chan == 0 ?
        std::array<SDL_Scancode, 4>{SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_S, SDL_SCANCODE_W} :
        chan == 1 ? std::array<SDL_Scancode, 4>{SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_H, SDL_SCANCODE_G} :
        chan == 2 ? std::array<SDL_Scancode, 4>{SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_K, SDL_SCANCODE_I} :
                    std::array<SDL_Scancode, 4>{SDL_SCANCODE_Z, SDL_SCANCODE_V, SDL_SCANCODE_N, SDL_SCANCODE_B};
    const float x = ReadKeyboardAxisLevel(keys, keyCount, profile[1]) -
                    ReadKeyboardAxisLevel(keys, keyCount, profile[0]);
    const float y = ReadKeyboardAxisLevel(keys, keyCount, profile[3]) -
                    ReadKeyboardAxisLevel(keys, keyCount, profile[2]);
    return {x, y};
}

uint32_t ClassicButtonsForGhost(const GhostSample& sample)
{
    uint32_t buttons = 0;
    if ((sample.face & 0x01) != 0) buttons |= kClassicButtonA;
    if ((sample.face & 0x02) != 0) buttons |= kClassicButtonB;
    if ((sample.face & 0x04) != 0) buttons |= kClassicButtonL;
    if ((sample.face & 0x08) != 0) buttons |= kClassicButtonR;
    if ((sample.face & 0x20) != 0) buttons |= kClassicButtonX;
    switch (sample.trick) {
    case 1: buttons |= kClassicButtonUp; break;
    case 2: buttons |= kClassicButtonDown; break;
    case 3: buttons |= kClassicButtonLeft; break;
    case 4: buttons |= kClassicButtonRight; break;
    default: break;
    }
    return buttons;
}

std::array<float, 2> ClassicStickForGhost(const GhostSample& sample)
{
    const auto axis = [](uint8_t value) {
        return std::clamp((static_cast<float>(value) - 7.0f) / 7.0f,
                          -1.0f, 1.0f);
    };
    return {axis(static_cast<uint8_t>(sample.direction >> 4)),
            axis(static_cast<uint8_t>(sample.direction & 0x0f))};
}

void WriteClassicStatus(uint32_t address, uint32_t hold, uint32_t trigger,
                        uint32_t release, uint32_t classicHold,
                        uint32_t classicTrigger, uint32_t classicRelease,
                        const std::array<float, 2>& stick)
{
    auto* status = Memory::GetPointer(address, kKpadStatusSize);
    std::memset(status, 0, kKpadStatusSize);
    Memory::Write32(address + 0x00, hold);
    Memory::Write32(address + 0x04, trigger);
    Memory::Write32(address + 0x08, release);
    Memory::Write8(address + 0x5C, kWpadExtensionClassic);
    Memory::Write8(address + 0x5F, kWpadFormatClassic);
    Memory::Write32(address + 0x60, classicHold);
    Memory::Write32(address + 0x64, classicTrigger);
    Memory::Write32(address + 0x68, classicRelease);
    Memory::WriteFloat32(address + 0x6C, stick[0]);
    Memory::WriteFloat32(address + 0x70, stick[1]);
}

} // namespace

extern "C" void KPad_RkgFixture_ForceRaceMetadata(uint32_t racedata)
{
    if (racedata == 0 || !ForceFixtureMetadataEnabled()) return;
    try {
        // Course choice is not exposed by the online character/vehicle
        // callbacks. Keep the three Racedata scenarios aligned, but never
        // rewrite online player records after the network has constructed
        // their resources.
        for (const uint32_t courseOffset : {2920u, 5976u, 9032u}) {
            Memory::Write32(racedata + courseOffset, Fixture().CourseId());
        }
        static bool logged = false;
        if (!logged) {
            std::fprintf(stderr,
                         "[input-fixture] forced race metadata course=%u vehicle=%u character=%u drift=%s\n",
                         Fixture().CourseId(), Fixture().VehicleId(),
                         Fixture().CharacterId(),
                         Fixture().DriftIsAuto() ? "automatic" : "manual");
            logged = true;
        }
    } catch (const Memory::AccessViolation&) {
        std::fprintf(stderr, "[input-fixture] unable to force race metadata\n");
    }
}

extern "C" uint32_t KPad_RkgFixture_ForceCharacterId(uint32_t requested)
{
    if (!ForceFixtureMetadataEnabled()) return requested;
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[input-fixture] selection forced character=%u\n",
                     Fixture().CharacterId());
        logged = true;
    }
    return Fixture().CharacterId();
}

extern "C" uint32_t KPad_RkgFixture_ForceVehicleId(uint32_t requested)
{
    if (!ForceFixtureMetadataEnabled()) return requested;
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[input-fixture] selection forced vehicle=%u\n",
                     Fixture().VehicleId());
        logged = true;
    }
    return Fixture().VehicleId();
}

extern "C" uint32_t KPad_RkgFixture_ForceCourseId(uint32_t requested)
{
    if (!ForceFixtureMetadataEnabled()) return requested;
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[input-fixture] vote forced course=%u\n",
                     Fixture().CourseId());
        logged = true;
    }
    return Fixture().CourseId();
}

extern "C" uint32_t KPad_RkgFixture_ForceDriftButtonId(uint32_t requested)
{
    if (!ForceFixtureMetadataEnabled()) return requested;
    return Fixture().DriftIsAuto() ? 1u : 2u;
}

extern "C" bool KPAD_IsKeyboardChannelConnected(uint32_t chan)
{
    return chan < g_keyboardConnected.size() &&
           (chan == 0 || g_keyboardConnected[chan].load(std::memory_order_acquire));
}

// KPAD HLE fed by a real Bluetooth Wii Remote. The game calls KPADRead once per
// frame with room for 16 KPADStatus entries and only looks at entry 0; with a
// Classic Controller it also calls KPADGetUnifiedWpadStatus for the raw
// WPADCLStatus (buttons, sticks and triggers of the extension).
namespace {



// KPADStatus field offsets (RVL SDK).
constexpr uint32_t kHold = 0x00, kTrig = 0x04, kRelease = 0x08, kAcc = 0x0C, kAccValue = 0x18,
                   kAccSpeed = 0x1C, kPos = 0x20, kAccVertical = 0x54, kDevType = 0x5C, kWpadErr = 0x5D,
                   kDpdValidFg = 0x5E, kDataFormat = 0x5F, kFsStick = 0x60, kFsAcc = 0x68, kFsAccValue = 0x74,
                   kFsAccSpeed = 0x78;
// KPADStatus.ex_status.cl (KPADEXStatus, Classic Controller view).
constexpr uint32_t kClHold = 0x60, kClTrig = 0x64, kClRelease = 0x68, kClLStick = 0x6C, kClRStick = 0x74,
                   kClLTrigger = 0x7C, kClRTrigger = 0x80;

// KPADUnifiedWpadStatus: WPADStatus / WPADFSStatus / WPADCLStatus union, then fmt.
constexpr uint32_t kUnifiedSize = 0x38;
constexpr uint32_t kUButton = 0x00, kUAccX = 0x02, kUAccY = 0x04, kUAccZ = 0x06, kUObj = 0x08, kUDev = 0x28,
                   kUErr = 0x29, kUFsStickX = 0x2A, kUFsStickY = 0x2B, kUFsAccX = 0x2C, kUFsAccY = 0x2E,
                   kUFsAccZ = 0x30, kUClButton = 0x2A, kUClLStickX = 0x2C, kUClLStickY = 0x2E, kUClRStickX = 0x30,
                   kUClRStickY = 0x32, kUClTriggerL = 0x34, kUClTriggerR = 0x35, kUFmt = 0x36;

// WPAD device types (WPAD_DEV_*) and the data formats KPAD runs each of them
// in (WPAD_FMT_*_ACC_DPD): the values KPADStatus.dev_type / data_format and
// KPADUnifiedWpadStatus.dev / fmt carry on the console.
constexpr uint8_t kDevCore = 0;
constexpr uint8_t kDevFreestyle = 1;
constexpr uint8_t kDevClassic = 2;
constexpr uint8_t kFmtCoreAccDpd = 2;
constexpr uint8_t kFmtFreestyleAccDpd = 5;
constexpr uint8_t kFmtClassicAccDpd = 8;
constexpr int8_t kWpadErrNone = 0;
constexpr int8_t kWpadErrNoController = -1;

// Raw accelerometer as WPADStatus carries it: 10 bits, 0x200 at 0 g, 100 per g.
constexpr float kRawAccZero = 512.0f;
constexpr float kRawAccPerG = 100.0f;

struct ChannelState {
    uint32_t prevHold = 0;
    uint32_t prevClHold = 0;
    float prevAcc[3] = {0.0f, -1.0f, 0.0f};
    float prevFsAcc[3] = {0.0f, -1.0f, 0.0f};
};

std::array<ChannelState, 4> g_channels{};

// Euclidean length of a 3-vector.
float Length(const float* v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Euclidean distance between two 3-vectors.
float Distance(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Length(d);
}

// Writes three big-endian floats to guest memory.
void WriteVec3(uint32_t addr, const float* v) {
    Memory::WriteFloat32(addr, v[0]);
    Memory::WriteFloat32(addr + 4, v[1]);
    Memory::WriteFloat32(addr + 8, v[2]);
}

// Zeroes `count` consecutive floats in guest memory.
void WriteZeroFloats(uint32_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        Memory::WriteFloat32(addr + i * 4, 0.0f);
    }
}

// Fills one KPADStatus at `addr` and returns the number of valid entries (1),
// or writes an "unplugged" status and returns 0.
int32_t WriteStatus(uint32_t chan, uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    ChannelState& state = g_channels[chan];
    if (sample == nullptr) {
        state = {};
        Memory::Write32(addr + kHold, 0);
        Memory::Write32(addr + kTrig, 0);
        Memory::Write32(addr + kRelease, 0);
        Memory::Write8(addr + kDevType, kDevCore);
        Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kDpdValidFg, 0);
        return 0;
    }

    const uint32_t hold = sample->hold;
    Memory::Write32(addr + kHold, hold);
    Memory::Write32(addr + kTrig, hold & ~state.prevHold);
    Memory::Write32(addr + kRelease, state.prevHold & ~hold);
    state.prevHold = hold;

    WriteVec3(addr + kAcc, sample->acc);
    Memory::WriteFloat32(addr + kAccValue, Length(sample->acc));
    Memory::WriteFloat32(addr + kAccSpeed, Distance(sample->acc, state.prevAcc));
    for (int i = 0; i < 3; ++i) state.prevAcc[i] = sample->acc[i];

    // No IR pointer: pos .. acc_vertical zeroed and dpd_valid_fg clear, which
    // the game treats as "pointing away from the screen".
    WriteZeroFloats(addr + kPos, (kAccVertical + 8 - kPos) / 4);
    Memory::Write8(addr + kDpdValidFg, 0);

    const uint8_t devType = sample->hasClassic ? kDevClassic : sample->hasNunchuk ? kDevFreestyle : kDevCore;
    const uint8_t dataFormat =
        sample->hasClassic ? kFmtClassicAccDpd : sample->hasNunchuk ? kFmtFreestyleAccDpd : kFmtCoreAccDpd;
    Memory::Write8(addr + kDevType, devType);
    Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNone));
    Memory::Write8(addr + kDataFormat, dataFormat);

    if (sample->hasClassic) {
        const uint32_t clHold = sample->clHold;
        Memory::Write32(addr + kClHold, clHold);
        Memory::Write32(addr + kClTrig, clHold & ~state.prevClHold);
        Memory::Write32(addr + kClRelease, state.prevClHold & ~clHold);
        state.prevClHold = clHold;
        Memory::WriteFloat32(addr + kClLStick, sample->clLStick[0]);
        Memory::WriteFloat32(addr + kClLStick + 4, sample->clLStick[1]);
        Memory::WriteFloat32(addr + kClRStick, sample->clRStick[0]);
        Memory::WriteFloat32(addr + kClRStick + 4, sample->clRStick[1]);
        Memory::WriteFloat32(addr + kClLTrigger, sample->clTriggerL / 255.0f);
        Memory::WriteFloat32(addr + kClRTrigger, sample->clTriggerR / 255.0f);
    } else if (sample->hasNunchuk) {
        state.prevClHold = 0;
        Memory::WriteFloat32(addr + kFsStick, sample->stick[0]);
        Memory::WriteFloat32(addr + kFsStick + 4, sample->stick[1]);
        WriteVec3(addr + kFsAcc, sample->nunchukAcc);
        Memory::WriteFloat32(addr + kFsAccValue, Length(sample->nunchukAcc));
        Memory::WriteFloat32(addr + kFsAccSpeed, Distance(sample->nunchukAcc, state.prevFsAcc));
        for (int i = 0; i < 3; ++i) state.prevFsAcc[i] = sample->nunchukAcc[i];
    } else {
        state.prevClHold = 0;
        WriteZeroFloats(addr + kFsStick, (kKpadStatusSize - kFsStick) / 4);
    }
    return 1;
}

// One accelerometer axis of KPAD's g vector back to the 10-bit raw WPAD value.
uint16_t RawAcc(float g) {
    const float raw = kRawAccZero + g * kRawAccPerG;
    return static_cast<uint16_t>(std::clamp(raw, 0.0f, 1023.0f));
}

// Fills one KPADUnifiedWpadStatus at `addr` from the sample: the WPADStatus core
// (remote buttons, raw accelerometer, no IR objects), then the Nunchuk or Classic
// Controller tail, then the data format.
void WriteUnifiedStatus(uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    for (uint32_t offset = 0; offset < kUnifiedSize; offset += 4) {
        Memory::Write32(addr + offset, 0);
    }
    if (sample == nullptr) {
        Memory::Write8(addr + kUDev, kDevCore);
        Memory::Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kUFmt, kFmtCoreAccDpd);
        return;
    }
    Memory::Write16(addr + kUButton, static_cast<uint16_t>(sample->hold & 0xFFFF));
    // KPAD's acc is (-wiiX, -wiiZ, wiiY); WPADStatus keeps the remote's own axes.
    Memory::Write16(addr + kUAccX, RawAcc(-sample->acc[0]));
    Memory::Write16(addr + kUAccY, RawAcc(sample->acc[2]));
    Memory::Write16(addr + kUAccZ, RawAcc(-sample->acc[1]));
    // No IR: every DPDObject invalid (x/y at the sensor's out-of-range value).
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::Write16(addr + kUObj + i * 8, 0x3FF);
        Memory::Write16(addr + kUObj + i * 8 + 2, 0x3FF);
    }
    Memory::Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNone));
    if (sample->hasClassic) {
        Memory::Write8(addr + kUDev, kDevClassic);
        Memory::Write16(addr + kUClButton, static_cast<uint16_t>(sample->clHold & 0xFFFF));
        Memory::Write16(addr + kUClLStickX, static_cast<uint16_t>(sample->clLStickRaw[0]));
        Memory::Write16(addr + kUClLStickY, static_cast<uint16_t>(sample->clLStickRaw[1]));
        Memory::Write16(addr + kUClRStickX, static_cast<uint16_t>(sample->clRStickRaw[0]));
        Memory::Write16(addr + kUClRStickY, static_cast<uint16_t>(sample->clRStickRaw[1]));
        Memory::Write8(addr + kUClTriggerL, sample->clTriggerL);
        Memory::Write8(addr + kUClTriggerR, sample->clTriggerR);
        Memory::Write8(addr + kUFmt, kFmtClassicAccDpd);
    } else if (sample->hasNunchuk) {
        Memory::Write8(addr + kUDev, kDevFreestyle);
        // WPADFSStatus: 8-bit stick (centre 128) and 10-bit Nunchuk accelerometer.
        Memory::Write8(addr + kUFsStickX,
                       static_cast<uint8_t>(std::clamp(128.0f + sample->stick[0] * 100.0f, 0.0f, 255.0f)));
        Memory::Write8(addr + kUFsStickY,
                       static_cast<uint8_t>(std::clamp(128.0f + sample->stick[1] * 100.0f, 0.0f, 255.0f)));
        Memory::Write16(addr + kUFsAccX, RawAcc(-sample->nunchukAcc[0]));
        Memory::Write16(addr + kUFsAccY, RawAcc(sample->nunchukAcc[2]));
        Memory::Write16(addr + kUFsAccZ, RawAcc(-sample->nunchukAcc[1]));
        Memory::Write8(addr + kUFmt, kFmtFreestyleAccDpd);
    } else {
        Memory::Write8(addr + kUDev, kDevCore);
        Memory::Write8(addr + kUFmt, kFmtCoreAccDpd);
    }
}

} // namespace

// KPADRead: fills KPADStatus[0] for `chan` from the Bluetooth remote, returns the entry count.
extern "C" int32_t KPAD__Read_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[input] KPADRead active: chan=%u count=%u\n", chan, count);
        logged = true;
    }
    if (chan >= g_previousButtons.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    if (WiiRemoteInput::IsRemoteChannel(chan)) {
        WiiRemoteInput::KpadSample sample;
        const bool have = WiiRemoteInput::ReadKpadSample(chan, sample);
        try {
            return WriteStatus(chan, statusPtr, have ? &sample : nullptr);
        } catch (const Memory::AccessViolation&) { return 0; }
    }
    EnsureKeyboardWatch();

    if (chan == 0 && ForceFixtureMetadataEnabled()) {
        try {
            KPad_RkgFixture_ForceRaceMetadata(Memory::Read32(0x809bd70c));
        } catch (const Memory::AccessViolation&) {
            // Racedata is not constructed during the earliest boot reads.
        }
    }

    const bool connected = chan == 0 || g_keyboardConnected[chan].load(std::memory_order_acquire);
    if (!connected) return 0;
    const uint32_t pending = g_pendingPresses[chan].exchange(0, std::memory_order_acq_rel);
    const uint32_t pendingClassic = g_pendingClassicPresses[chan].exchange(0, std::memory_order_acq_rel);
    if (chan == 0 && g_fixtureArmRequested.exchange(false, std::memory_order_acq_rel)) {
        if (Fixture().Active()) {
            Fixture().Disarm();
        } else {
            Fixture().Arm(true);
        }
    }
    const bool fixtureActive = chan == 0 && Fixture().Active();
    // The opt-in race-state hook consumes one RKG frame per guest simulation
    // tick. KPAD sampling must only expose its current state; consuming here
    // ties ghost cadence to host presentation and drifts when the two differ.
    const GhostSample ghost = fixtureActive ? Fixture().Current() : GhostSample{};
    const uint32_t hold = !fixtureActive ?
        (ReadKeyboardButtons(chan) | pending) : 0;
    const uint32_t classicHold = fixtureActive ? ClassicButtonsForGhost(ghost) :
        (ReadClassicButtons(chan) | pendingClassic);
    const auto stick = fixtureActive ? ClassicStickForGhost(ghost) :
                                       ReadClassicLeftStick(chan);
    const uint32_t previous = g_previousButtons[chan];
    const uint32_t previousClassic = g_previousClassicButtons[chan];
    const uint32_t trigger = (fixtureActive ? 0 : pending) | (hold & ~previous);
    const uint32_t release = previous & ~hold;
    const uint32_t classicTrigger = (fixtureActive ? 0 : pendingClassic) |
                                    (classicHold & ~previousClassic);
    const uint32_t classicRelease = previousClassic & ~classicHold;
    g_previousButtons[chan] = hold;
    g_previousClassicButtons[chan] = classicHold;

    if (trigger != 0 || classicTrigger != 0) {
        std::fprintf(stderr,
                     "[input] sample core=%08x classic=%08x stick=(%.1f,%.1f)\n",
                     trigger, classicTrigger, stick[0], stick[1]);
    }

    try {
        WriteClassicStatus(statusPtr, hold, trigger, release, classicHold,
                           classicTrigger, classicRelease, stick);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
    return 1;
}
PPC_NATIVE_OVERRIDE(80197380, KPAD__Read_HLE, int32_t, (uint32_t chan, uint32_t statusPtr, uint32_t count),
         (chan, statusPtr, count));

// KPADGetUnifiedWpadStatus: the raw WPAD status behind KPADStatus. The game
// reads the Classic Controller's buttons, sticks and triggers from here. The
// SDK fills `count` entries with the channel's recent samples (the game asks for
// as many as it asked KPADRead for and looks at entry 0); with one sample per
// frame here, every entry gets the current one.
extern "C" int32_t KPAD__GetUnifiedWpadStatus_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[input] KPADGetUnifiedWpadStatus active: chan=%u count=%u\n", chan, count);
        logged = true;
    }
    if (chan >= g_previousButtons.size() || statusPtr == 0 || count == 0 ||
        (chan != 0 && !g_keyboardConnected[chan].load(std::memory_order_acquire))) {
        return 0;
    }
    if (WiiRemoteInput::IsRemoteChannel(chan)) {
        WiiRemoteInput::KpadSample sample;
        const bool have = WiiRemoteInput::ReadKpadSample(chan, sample);
        try {
            for (uint32_t i = 0; i < std::min(count, 16u); ++i)
                WriteUnifiedStatus(statusPtr + i * kUnifiedSize, have ? &sample : nullptr);
        } catch (const Memory::AccessViolation&) { return 0; }
        return have ? 1 : 0;
    }
    EnsureKeyboardWatch();
    const uint32_t pending = g_pendingPresses[chan].exchange(0, std::memory_order_acq_rel);
    const uint32_t pendingClassic =
        g_pendingClassicPresses[chan].exchange(0, std::memory_order_acq_rel);
    const bool fixtureActive = Fixture().Active();
    try {
        auto* status = Memory::GetPointer(statusPtr, kKpadUnifiedStatusSize);
        std::memset(status, 0, kKpadUnifiedStatusSize);
        Memory::Write16(statusPtr + 0x00,
                        static_cast<uint16_t>(fixtureActive ? 0 :
                                              (ReadKeyboardButtons(chan) | pending)));
        Memory::Write8(statusPtr + 0x28, kWpadExtensionClassic);
        const auto& ghost = Fixture().Current();
        const uint32_t classicButtons = fixtureActive ?
            ClassicButtonsForGhost(ghost) :
            (ReadClassicButtons(chan) | pendingClassic);
        Memory::Write16(statusPtr + 0x2A,
                        static_cast<uint16_t>(classicButtons));
        const auto stick = fixtureActive ? ClassicStickForGhost(ghost) :
                                                ReadClassicLeftStick(chan);
        const auto stickX = static_cast<int16_t>(stick[0] * 511.0f);
        const auto stickY = static_cast<int16_t>(stick[1] * 511.0f);
        Memory::Write16(statusPtr + 0x2C, static_cast<uint16_t>(stickX));
        Memory::Write16(statusPtr + 0x2E, static_cast<uint16_t>(stickY));
        // In Mario Kart's 2008 SDK this byte follows the 0x36-byte WPADCLStatus.
        Memory::Write8(statusPtr + 0x36, kWpadFormatClassic);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
    return 0;
}
PPC_NATIVE_OVERRIDE(8019812C, KPAD__GetUnifiedWpadStatus_HLE, int32_t,
         (uint32_t chan, uint32_t statusPtr, uint32_t count), (chan, statusPtr, count));

// Called by a reproducibly injected guard at the top of the pinned PAL
// KPadWiiController::calcInner translation. Returning false preserves the
// complete original function. Returning true supplies the same race-state
// fields as KPadGhostController and consumes one frame per guest simulation
// tick instead of per host KPAD/presentation sample.
extern "C" bool KPad_RkgFixture_CalcInner(CpuContext* ctx)
{
    uint32_t stage = 0xffffffffu;
    try {
        const uint32_t raceManager = Memory::Read32(0x809bd730);
        stage = raceManager == 0 ? 0xffffffffu :
                                   Memory::Read8(raceManager + 0x2b);
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    if (!Fixture().Active() && !Fixture().AutoArmAtCountdown(stage)) return false;

    const uint32_t controller = ctx->gpr[3];
    try {
        if (controller == 0) {
            return false;
        }
        if (ForceFixtureMetadataEnabled()) {
            Memory::Write8(controller + 0x51,
                           Fixture().DriftIsAuto() ? 1 : 0);
        }
        // The fixture replaces calcInner for the whole race tick. Mirror the
        // successful Classic Controller bookkeeping that the retail function
        // would normally refresh after WPADProbe/KPADRead. In particular,
        // +0x50 is the live-controller flag; letting it go stale eventually
        // opens the retail "controller interrupted" modal.
        Memory::Write8(controller + 0x50, 1);
        Memory::Write32(controller + 0x8d0, 1);
        Memory::Write32(controller + 0x8d8, 2);
        Memory::Write32(controller + 0x8dc, 2);
        Memory::Write16(controller + 0x8f8, 0);
        Memory::Write16(controller + 0x8fc, 0);
        Memory::Write16(controller + 0x8fe, 0);
        Memory::Write32(controller + 0x900, 2);
    } catch (const Memory::AccessViolation&) {
        Fixture().Disarm();
        return false;
    }

    const uint32_t raceState = ctx->gpr[4];
    const uint32_t uiState = ctx->gpr[5];
    if (raceState == 0 || uiState == 0) {
        return false;
    }

    GhostSample sample;
    try {
        const char* singleStep = std::getenv("KARTPAD_RKG_SINGLE_STEP_V2");
        const bool oncePerPresentation = singleStep != nullptr &&
                                         std::strcmp(singleStep, "1") == 0;
        sample = Fixture().BeginIfRaceActive(stage) ?
            (oncePerPresentation ? Fixture().NextOncePerPresentationFrame() :
                                   Fixture().Next()) : Fixture().Current();
    } catch (const Memory::AccessViolation&) {
        Fixture().Disarm();
        return false;
    }
    const uint8_t xRaw = static_cast<uint8_t>(sample.direction >> 4);
    const uint8_t yRaw = static_cast<uint8_t>(sample.direction & 0x0f);
    const auto axis = [](uint8_t value) {
        return (static_cast<float>(value) - 7.0f) / 7.0f;
    };

    try {
        Memory::Write8(raceState + 0x14,
                       static_cast<uint8_t>(Memory::Read8(raceState + 0x14) & 0x7f));
        Memory::Write8(uiState + 0x30,
                       static_cast<uint8_t>(Memory::Read8(uiState + 0x30) & 0x7f));
        float stickX = axis(xRaw);
        const uint32_t raceConfig = Memory::Read32(0x809bd70c);
        const bool isMirror = Memory::Read8(raceConfig + 0x4155) != 0;
        if (isMirror) {
            stickX = -stickX;
        }
        uint8_t trick = sample.trick;
        if (isMirror) {
            if (trick == 3) {
                trick = 4;
            } else if (trick == 4) {
                trick = 3;
            }
        }
        Memory::Write16(raceState + 0x04, sample.face);
        Memory::WriteFloat32(raceState + 0x08, stickX);
        Memory::WriteFloat32(raceState + 0x0C, axis(yRaw));
        Memory::Write8(raceState + 0x10, xRaw);
        Memory::Write8(raceState + 0x11, yRaw);
        Memory::Write8(raceState + 0x12, trick);
        Memory::Write8(raceState + 0x13, sample.trick);
        Memory::Write8(raceState + 0x14,
                       static_cast<uint8_t>(Memory::Read8(raceState + 0x14) | 0x80));

        // A time-trial RKG cannot physically finish an online VS race because
        // the multiplayer starting grid uses a different pose. The opt-in
        // endurance harness normally lets both real peers exchange the full
        // 5001-frame trace plus five seconds of packets, then asks the retail
        // RaceinfoPlayer path to finish the local racer. A separate explicit
        // frame threshold supports shorter protocol acceptance runs before an
        // open-loop trace diverges from the VS starting grid. This exercises the
        // native results and return-to-lobby protocol without shipping any
        // gameplay behavior: the hook is unreachable unless the explicit
        // KARTPAD_RKG_FORCE_FINISH_V2 test environment variable is set.
        if (stage == 2) {
            const uint32_t raceinfo = Memory::Read32(0x809bd730);
            const uint32_t racedata = Memory::Read32(0x809bd728);
            const uint32_t players = raceinfo == 0 ? 0 : Memory::Read32(raceinfo + 0x0c);
            const uint32_t playerCount = racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
            uint32_t localPlayer = 0;
            uint32_t localPlayerId = 0xffffffffu;
            if (players != 0 && playerCount > 0 && playerCount <= 12) {
                for (uint32_t id = 0; id < playerCount; ++id) {
                    const uint32_t player = Memory::Read32(players + id * 4);
                    const uint32_t holder = player == 0 ? 0 : Memory::Read32(player + 0x48);
                    const uint32_t activeController = holder == 0 ? 0 : Memory::Read32(holder + 0x04);
                    if (activeController == controller) {
                        localPlayer = player;
                        localPlayerId = id;
                        break;
                    }
                }
            }
            if (localPlayer != 0 &&
                (Memory::Read32(localPlayer + 0x38) & 0x02u) == 0 &&
                Fixture().ConsumeFinishRequest()) {
                const CpuContext saved = *ctx;
                ctx->gpr[3] = localPlayer;
                ctx->gpr[4] = 2;  // normal online race completion status
                ctx->gpr[5] = 1;  // use the normal no-cameras online path
                InvokeDirectCpu<0x805342E8u>(ctx);
                *ctx = saved;
                std::fprintf(stderr,
                             "[input-fixture] forced native finish player=%u frame=%zu post-stream=%zu\n",
                             localPlayerId, Fixture().Frame(),
                             Fixture().PostStreamFrames());
            }
        }
    } catch (const Memory::AccessViolation&) {
        Fixture().Disarm();
    }
    return true;
}
