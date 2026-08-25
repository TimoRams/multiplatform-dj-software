#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "audio/cache/AudioPageCache.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace engine::audio {

struct ScratchCacheStats {
    std::uint64_t pageHits = 0, pageMisses = 0, starvationBlocks = 0;
    std::uint64_t recoveryEvents = 0, droppedRequests = 0;
    std::uint64_t generationMismatches = 0, diskReadsFromAudioThread = 0;
};

// Motion telemetry for the position tracker. Everything here is accumulated in
// locals inside the audio callback and published once per block, so enabling it
// costs a handful of relaxed stores and never logs from the realtime thread.
struct ScratchMotionStats {
    // Per-sample resampler rate extremes over the most recent tracking block.
    double minRate = 0.0;
    double maxRate = 0.0;
    // Largest rate change between two adjacent output samples. A frozen or
    // snapped read head shows up here long before it is audible in a spectrum.
    double maxRateStep = 0.0;
    // Largest change of that step (discrete rate curvature). Callback-rate
    // zippering is primarily a discontinuity in acceleration and is visible
    // here even when the rate itself never jumps.
    double maxRateStepDelta = 0.0;
    // target - readPosition at block end, in track samples.
    double trackingErrorSamples = 0.0;
    // How far the read head was allowed to lead the last known hand target.
    double leadSamples = 0.0;
    // Blocks in which the lead limiter had to hold the read head back.
    std::uint64_t leadLimitedBlocks = 0;
};

// RT-safe band-limited scratch reader with true bidirectional playback.
// Window sizing follows the device buffer size from audio settings.
class ScratchResampler {
public:
    // Physical controller commands are clamped to 8x. The position tracker gets
    // a little private headroom so it can recover finite startup/event lag even
    // while the hand itself is already moving at that 8x boundary.
    static constexpr double kMaximumTrackingRate = 10.0;

    void prepare(int numChannels, int deviceBufferSize, double outputSampleRate);
    void reset(double readPositionSamples) noexcept;
    void setReadPositionSamples(double readPositionSamples) noexcept;
    void nudgeReadPositionSamples(double deltaSamples) noexcept;
    void snapSmoothedRate(double rate) noexcept;
    void primeTrackerVelocity(double ratePerOutputSample) noexcept;
    void invalidatePrefetch() noexcept { m_sourceSize = 0; }
    void setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle);
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

    // Position-authoritative scratch step. A critically-damped tracker glides
    // the read head toward the absolute hand target (track samples). Slow moves
    // retain exact long-term position; the C2 reference carries momentum across
    // sparse events without a hard snap. Returns the rate used (track samples
    // per output sample).
    // inputLeadSeconds is the measured age of the supplied target when the
    // block starts. It defaults to a typical hardware jog cadence.
    double processScratchTracking(double targetPosSamples,
                                  double commandedRate,
                                  double maxAbsRate,
                                  const juce::AudioSourceChannelInfo& output,
                                  double inputLeadSeconds = 0.002) noexcept;

    [[nodiscard]] double readPosition() const noexcept { return m_readPos; }
    [[nodiscard]] int deviceBufferSize() const noexcept { return m_deviceBufferSize; }
    [[nodiscard]] ScratchCacheStats cacheStats() const noexcept;
    [[nodiscard]] ScratchMotionStats motionStats() const noexcept;

private:
    [[nodiscard]] int requiredSourceBufferCapacity(double sourceSampleRate) const noexcept;
    void windowMargins(double rate, int outputBlockSize, int& lookBehind, int& lookAhead) const noexcept;
    [[nodiscard]] bool needsWindowReload(double minAbsPos, double maxAbsPos) const noexcept;
    bool ensureWindow(double rate, int outputBlockSize) noexcept;
    bool refillWindowFromCache(double rate, int outputBlockSize) noexcept;
    [[nodiscard]] bool positionInWindow(double position) const noexcept;
    void prepareSincTable();
    void writeScratchOutput(float* out0, float* out1, int index, bool ready,
                            double rate) noexcept;
    void readBandlimitedStereo(double position, double rate,
                               float& left, float& right) const noexcept;
    double wrapPosition(double pos) const noexcept;

    AudioPageCache* m_cache = nullptr;
    AudioCacheHandle m_cacheHandle;
    juce::AudioBuffer<float> m_sourceBuffer;
    int m_channels = 2;
    int m_deviceBufferSize = 512;
    int m_blockSize = 512;
    double m_outputSampleRate = 44100.0;
    double m_trackSampleRate = 44100.0;
    double m_trackLengthSamples = 0.0;

    int m_sourceSize = 0;
    double m_readPos = 0.0;
    double m_bufferOriginSample = 0.0;
    double m_lastRate = 0.0;
    double m_smoothedRate = 0.0;
    double m_previousRateStep = 0.0;
    double m_trackVel = 0.0;   // tracker velocity, track samples / second
    // Continuously advanced estimate of where the hand is now. Kept across
    // blocks so a newly arrived event corrects it rather than replaces it. The
    // velocity is retained as well: each block joins the old and new hand state
    // with a minimum-jerk quintic Hermite trajectory, keeping position, velocity
    // and acceleration continuous at callback boundaries.
    double m_referencePos = 0.0;
    double m_referenceVelocity = 0.0; // track samples / second
    bool m_referenceValid = false;
    // Decaying envelope of recent hand speed, track samples/second. The runaway
    // guard sizes itself from this rather than the instantaneous speed, which
    // passes through zero at every direction change.
    double m_leadSpeedEnvelope = 0.0;
    float m_starvationGain = 0.0f;
    float m_lastOutputL = 0.0f;
    float m_lastOutputR = 0.0f;

    bool m_loopActive = false;
    double m_loopInSample = 0.0;
    double m_loopOutSample = 0.0;

    static constexpr int kSincTaps = 64;
    static constexpr int kSincRadius = kSincTaps / 2;
    static constexpr int kSincPhaseCount = 256;
    // Covers 10x tracking for a 192 kHz source on a 44.1 kHz device without a
    // coefficient rebuild during track installation. Bands are uniform in
    // cutoff frequency rather than speed, retaining useful resolution at the
    // common 1x-10x ratios despite this wide safety range.
    static constexpr double kMaximumFilterRate = 64.0;
    static constexpr int kSincCutoffBands = 32;
    static std::array<float, kSincCutoffBands * (kSincPhaseCount + 1) * kSincTaps>
        s_sincTable;
    static std::once_flag s_sincTableOnce;
    static constexpr int kMinWindowSamples = kSincTaps + 2;
    static constexpr int kStarvationFadeSamples = 128;
    std::atomic<std::uint64_t> m_pageHits{0}, m_pageMisses{0}, m_starvationBlocks{0};
    std::atomic<std::uint64_t> m_recoveryEvents{0}, m_droppedRequests{0}, m_generationMismatches{0};
    std::atomic<std::uint64_t> m_diskReadsFromAudioThread{0};

    std::atomic<double> m_statMinRate{0.0}, m_statMaxRate{0.0}, m_statMaxRateStep{0.0};
    std::atomic<double> m_statMaxRateStepDelta{0.0};
    std::atomic<double> m_statTrackingError{0.0}, m_statLead{0.0};
    std::atomic<std::uint64_t> m_statLeadLimitedBlocks{0};
};

} // namespace engine::audio
