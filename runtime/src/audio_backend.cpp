#include "audio_backend.h"

#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <TargetConditionals.h>

AudioBackend& AudioBackend::Instance() {
    static AudioBackend instance;
    return instance;
}

float AudioBackend::EffectiveGainLocked() const {
    return m_muted ? 0.0f : m_masterVolume;
}

void AudioBackend::ApplyGainLocked() {
    if (m_stream && !SDL_SetAudioStreamGain(m_stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
    }
}

void AudioBackend::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

void AudioBackend::SetPausedForHost(bool paused) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream) return;
    if (paused) {
        SDL_PauseAudioStreamDevice(m_stream);
    } else {
        // Discard pre-menu queued audio before new guest frames start mixing.
        SDL_ClearAudioStream(m_stream);
        SDL_ResumeAudioStreamDevice(m_stream);
    }
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels) {
    if (m_initialized && m_sampleRate == sampleRate && m_channels == channels) {
        return true;
    }

    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }

#if TARGET_OS_IOS
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
#endif

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sampleRate);

    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    if (!SDL_SetAudioStreamGain(stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    m_stream = stream;
    m_spec = spec;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    RT_LOG(RT_TAG_AUDIO) << "host playback active: " << sampleRate << " Hz, "
                         << channels << " channels, gain=" << EffectiveGainLocked()
                         << std::endl;
    return true;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queueChecks != 0) {
        const int queued = m_stream ? SDL_GetAudioStreamQueued(m_stream) : -1;
        LogQueueTelemetryLocked(queued, true);
    }
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_reportedDroppedBlock = false;
    m_reportedAudibleBlock = false;
    m_queueChecks = 0;
    m_emptyQueueChecks = 0;
    m_droppedBlocks = 0;
    m_droppedBytes = 0;
    m_submittedBytes = 0;
    m_minQueuedBytes = -1;
    m_maxQueuedBytes = 0;
    m_convertBuffer.clear();
}

uint32_t AudioBackend::QueueLimitBytesLocked() const {
#if TARGET_OS_IOS
    constexpr uint32_t queueMs = 60;
#else
    constexpr uint32_t queueMs = 120;
#endif

    const uint64_t bytesPerSecond = static_cast<uint64_t>(m_spec.freq) *
                                    static_cast<uint64_t>(m_spec.channels) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * queueMs) / 1000u);
}

bool AudioBackend::QueueHasCapacityLocked(int incomingBytes) {
    if (!m_stream) {
        return false;
    }
    const int queued = SDL_GetAudioStreamQueued(m_stream);
    if (queued < 0) {
        return true;
    }
    ++m_queueChecks;
    if (m_queueChecks > 1 && queued == 0) {
        ++m_emptyQueueChecks;
    }
    m_minQueuedBytes = m_minQueuedBytes < 0 ? queued : std::min(m_minQueuedBytes, queued);
    m_maxQueuedBytes = std::max(m_maxQueuedBytes, queued);
    const uint32_t maxQueued = QueueLimitBytesLocked();
    if (static_cast<uint64_t>(queued) + static_cast<uint64_t>(std::max(incomingBytes, 0)) > maxQueued) {
        // Match Dolphin's FIFO overflow behavior: preserve the continuous audio
        // already queued and discard the new block.  Clearing SDL's entire
        // stream creates an audible discontinuity (the severe crackle seen when
        // VI-batched DMA briefly outran playback).
        // A drop is audible; report the first one so it is not invisible.
        if (!m_reportedDroppedBlock) {
            m_reportedDroppedBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "output queue full (" << queued << "/" << maxQueued
                      << " bytes); dropping blocks to preserve continuity" << std::endl;
        }
        ++m_droppedBlocks;
        m_droppedBytes += static_cast<uint64_t>(std::max(incomingBytes, 0));
        LogQueueTelemetryLocked(queued);
        return false;
    }
    LogQueueTelemetryLocked(queued);
    return true;
}

void AudioBackend::LogQueueTelemetryLocked(int queued, bool final) {
    constexpr uint64_t reportEveryChecks = 8192;
    if (!final && (m_queueChecks == 0 || (m_queueChecks % reportEveryChecks) != 0)) {
        return;
    }
    RT_LOG(RT_TAG_AUDIO) << (final ? "final " : "")
                         << "queue telemetry: checks=" << m_queueChecks
                         << ", empty-before-push=" << m_emptyQueueChecks
                         << ", dropped-blocks=" << m_droppedBlocks
                         << ", dropped-bytes=" << m_droppedBytes
                         << ", submitted-bytes=" << m_submittedBytes
                         << ", queued=" << queued
                         << ", observed-range=[" << m_minQueuedBytes << ","
                         << m_maxQueuedBytes << "] bytes, limit="
                         << QueueLimitBytesLocked() << " bytes" << std::endl;
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const size_t sampleCount = bytes / sizeof(int16_t);
    if (sampleCount == 0) {
        return false;
    }

    if (m_convertBuffer.size() < sampleCount) {
        m_convertBuffer.resize(sampleCount);
    }

    const size_t frameCount = sampleCount / 2;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t rightOffset = frame * 4;
        const size_t leftOffset = rightOffset + 2;
        const uint16_t right = static_cast<uint16_t>(data[rightOffset]) << 8 |
                               static_cast<uint16_t>(data[rightOffset + 1]);
        const uint16_t left = static_cast<uint16_t>(data[leftOffset]) << 8 |
                              static_cast<uint16_t>(data[leftOffset + 1]);
        m_convertBuffer[frame * 2] = static_cast<int16_t>(left);
        m_convertBuffer[frame * 2 + 1] = static_cast<int16_t>(right);
    }

    const int lenBytes = static_cast<int>(frameCount * 2 * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, m_convertBuffer.data(), lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }
    m_submittedBytes += static_cast<uint64_t>(lenBytes);
    if (!m_reportedAudibleBlock) {
        const auto peak = std::max_element(
            m_convertBuffer.begin(), m_convertBuffer.begin() + sampleCount,
            [](int16_t a, int16_t b) { return std::abs(static_cast<int>(a)) < std::abs(static_cast<int>(b)); });
        if (peak != m_convertBuffer.begin() + sampleCount && *peak != 0) {
            m_reportedAudibleBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "non-silent PCM reached host playback: peak="
                                 << std::abs(static_cast<int>(*peak))
                                 << ", queued=" << SDL_GetAudioStreamQueued(m_stream)
                                 << " bytes" << std::endl;
        }
    }

    return true;
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const int lenBytes = static_cast<int>(sampleCount * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, samples, lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }
    m_submittedBytes += static_cast<uint64_t>(lenBytes);
    if (!m_reportedAudibleBlock) {
        int peak = 0;
        for (size_t i = 0; i < sampleCount; ++i) {
            peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        }
        if (peak != 0) {
            m_reportedAudibleBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "non-silent PCM reached host playback: peak="
                                 << peak << ", queued="
                                 << SDL_GetAudioStreamQueued(m_stream) << " bytes"
                                 << std::endl;
        }
    }

    return true;
}
