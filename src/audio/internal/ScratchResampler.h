#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "audio/cache/AudioPageCache.h"
#include <algorithm>
#include <cmath>

namespace engine::audio {

struct ScratchCacheStats {
    std::uint64_t pageHits = 0, pageMisses = 0, starvationBlocks = 0;
    std::uint64_t recoveryEvents = 0, droppedRequests = 0;
    std::uint64_t generationMismatches = 0, diskReadsFromAudioThread = 0;
};

// RT-safe Hermite scratch reader with true bidirectional playback.
// Window sizing follows the device buffer size from audio settings.
class ScratchResampler {
public:
    void prepare(int numChannels, int deviceBufferSize, double outputSampleRate);
    void reset(double readPositionSamples) noexcept;
    void setReadPositionSamples(double readPositionSamples) noexcept;
    void nudgeReadPositionSamples(double deltaSamples) noexcept;
    void snapSmoothedRate(double rate) noexcept;
    void primeTrackerVelocity(double ratePerOutputSample) noexcept;
    void invalidatePrefetch() noexcept { m_sourceSize = 0; }
    void setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle) noexcept;
    void prefetchAround(double readPositionSamples) noexcept;

    void setTrackLengthSamples(double lengthSamples) noexcept {
        m_trackLengthSamples = std::max(0.0, lengthSamples);
    }

    void setLoopRange(double loopInSample, double loopOutSample, bool active) noexcept
    {
        m_loopInSample = loopInSample;
        m_loopOutSample = loopOutSample;
        m_loopActive = active;
    }

    // rate: track samples advanced per output sample (negative = reverse)
    void processBlock(double rate,
                      const juce::AudioSourceChannelInfo& output) noexcept;

    // Position-authoritative scratch step. A critically-damped tracker glides the
    // read head toward the absolute hand target (track samples). Slow/precise moves
    // track exactly with no overshoot; momentum carries playback smoothly across
    // sparse UI events. Returns the rate used (track samples per output sample).
    double processScratchTracking(double targetPosSamples,
                                  double commandedRate,
                                  double maxAbsRate,
                                  const juce::AudioSourceChannelInfo& output) noexcept;

    [[nodiscard]] double readPosition() const noexcept { return m_readPos; }
    [[nodiscard]] int deviceBufferSize() const noexcept { return m_deviceBufferSize; }
    [[nodiscard]] ScratchCacheStats cacheStats() const noexcept;

private:
    void windowMargins(double rate, int outputBlockSize, int& lookBehind, int& lookAhead) const noexcept;
    [[nodiscard]] bool needsWindowReload(double minAbsPos, double maxAbsPos) const noexcept;
    bool ensureWindow(double rate, int outputBlockSize) noexcept;
    bool refillWindowFromCache(double rate, int outputBlockSize) noexcept;
    [[nodiscard]] bool positionInWindow(double position) const noexcept;
    void writeScratchOutput(float* out0, float* out1, int index, bool ready) noexcept;
    float readHermite(int channel, double position) const noexcept;
    double wrapPosition(double pos) const noexcept;

    AudioPageCache* m_cache = nullptr;
    AudioCacheHandle m_cacheHandle;
    juce::AudioBuffer<float> m_sourceBuffer;
    int m_channels = 2;
    int m_deviceBufferSize = 512;
    int m_blockSize = 512;
    double m_outputSampleRate = 44100.0;
    double m_trackLengthSamples = 0.0;

    int m_sourceSize = 0;
    double m_readPos = 0.0;
    double m_bufferOriginSample = 0.0;
    double m_lastRate = 0.0;
    double m_smoothedRate = 0.0;
    double m_trackVel = 0.0;   // tracker velocity, track samples / second
    float m_starvationGain = 0.0f;
    float m_lastOutputL = 0.0f;
    float m_lastOutputR = 0.0f;

    bool m_loopActive = false;
    double m_loopInSample = 0.0;
    double m_loopOutSample = 0.0;

    static constexpr int kHermiteRadius = 2;
    static constexpr int kMinWindowSamples = 12;
    static constexpr int kStarvationFadeSamples = 128;
    std::atomic<std::uint64_t> m_pageHits{0}, m_pageMisses{0}, m_starvationBlocks{0};
    std::atomic<std::uint64_t> m_recoveryEvents{0}, m_droppedRequests{0}, m_generationMismatches{0};
    std::atomic<std::uint64_t> m_diskReadsFromAudioThread{0};
};

} // namespace engine::audio
