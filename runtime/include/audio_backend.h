#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL3/SDL_audio.h>

class AudioBackend {
public:
    static AudioBackend& Instance();

    bool Init(uint32_t sampleRate, uint32_t channels);
    void Shutdown();

    // Wii AI DMA frames are big-endian and ordered right, left. SDL expects
    // native-endian interleaved left, right samples.
    bool PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes);
    bool PushSamplesLE16(const int16_t* samples, size_t sampleCount);

    // Applied to the final host output, covering both AX and direct AI DMA.
    void SetMasterVolume(float volume);
    void SetMuted(bool muted);
    // Suspend this runtime output without changing the user's mute/volume.
    void SetPausedForHost(bool paused);

private:
    AudioBackend() = default;
    ~AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels);
    bool QueueHasCapacityLocked(int incomingBytes);
    void LogQueueTelemetryLocked(int queued, bool final = false);
    uint32_t QueueLimitBytesLocked() const;
    float EffectiveGainLocked() const;
    void ApplyGainLocked();

    mutable std::mutex m_mutex;
    SDL_AudioStream* m_stream = nullptr;
    SDL_AudioSpec m_spec{};
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    bool m_initialized = false;
    float m_masterVolume = 1.0f;
    bool m_muted = false;
    bool m_reportedDroppedBlock = false;
    bool m_reportedAudibleBlock = false;
    uint64_t m_queueChecks = 0;
    uint64_t m_emptyQueueChecks = 0;
    uint64_t m_droppedBlocks = 0;
    uint64_t m_droppedBytes = 0;
    uint64_t m_submittedBytes = 0;
    int m_minQueuedBytes = -1;
    int m_maxQueuedBytes = 0;
    std::vector<int16_t> m_convertBuffer;
};
