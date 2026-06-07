#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace engine::scratch {

struct ScratchControllerConfig {
    double sampleIntervalSeconds = 0.004;
    double moveDelayMaxSeconds = 0.040;
    double throwThreshold = 2.5;
    double maxVelocity = 100.0;
    double timeToStopSeconds = 1.0;
    double p = 0.38;
    double d = -0.12;
    double filterFactor = 0.35;
    double minScratchRate = 0.00005;
    double maxScratchRate = 48.0;
};

// Mixxx PositionScratchController-inspired PD follower: target platter motion drives
// playback rate; audio never hard-seeks per UI event.
class ScratchController {
public:
    ScratchController() = default;

    void setConfig(const ScratchControllerConfig& config) { m_config = config; }

    void setTrackSampleRate(double sampleRate) noexcept;

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    void setInertiaEnabled(bool enabled) noexcept { m_inertiaEnabled.store(enabled, std::memory_order_relaxed); }

    // UI / control thread
    void startScratch(double audioSamplePos, double targetSamplePos) noexcept;
    void stopScratch() noexcept;
    void releaseScratch() noexcept;

    void setTargetSamplePosition(double targetSamples) noexcept;
    void addTargetSampleDelta(double deltaSamples) noexcept;
    void notifyTargetMoved() noexcept;

    // Audio thread — once per output block
    double processAudioBlock(double currentAudioSamplePos,
                             int bufferSize,
                             double outputSampleRate,
                             double baseSampleRateRatio,
                             bool loopActive,
                             double loopInSample,
                             double loopOutSample) noexcept;

    void tickInertia(double dtSeconds) noexcept;

    [[nodiscard]] bool isScratching() const noexcept { return m_isScratching.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isInertiaActive() const noexcept { return m_inertiaActive.load(std::memory_order_relaxed); }
    [[nodiscard]] double rate() const noexcept { return m_rate.load(std::memory_order_relaxed); }
    [[nodiscard]] double readPositionSamples() const noexcept { return m_readPosition.load(std::memory_order_relaxed); }
    [[nodiscard]] double targetSamplePosition() const noexcept { return m_targetSamplePos.load(std::memory_order_relaxed); }

private:
    void resetControllerState(double audioSamplePos, double targetSamplePos) noexcept;
    [[nodiscard]] double correctSampleDeltaForLoop(double sampleDelta,
                                                   bool loopActive,
                                                   double loopInSample,
                                                   double loopOutSample) const noexcept;
    void runPdTick(double targetDeltaNormalized) noexcept;
    void clampRate(bool snapSmallToZero) noexcept;

    ScratchControllerConfig m_config;

    std::atomic<bool> m_enabled { true };
    std::atomic<bool> m_inertiaEnabled { true };
    std::atomic<bool> m_isScratching { false };
    std::atomic<bool> m_inertiaActive { false };

    std::atomic<double> m_targetSamplePos { 0.0 };
    std::atomic<double> m_rate { 0.0 };
    std::atomic<double> m_readPosition { 0.0 };
    std::atomic<double> m_trackSampleRate { 44100.0 };

    // PD state (audio thread only)
    double m_previousAudioSamplePos = 0.0;
    double m_scratchStartTargetPos = 0.0;
    double m_audioDeltaSum = 0.0;
    double m_targetDeltaLast = 0.0;
    double m_previousError = 0.0;
    double m_filteredError = 0.0;
    double m_moveDelay = 0.0;
    double m_scratchPosSampleTime = 0.0;
    int m_bufferSize = 0;
    double m_dt = 0.0;
    int m_callsPerDt = 1;
    int m_callsSinceInterval = 0;
    double m_timeSinceTargetMove = 0.0;
    uint64_t m_targetMoveGeneration = 0;
    uint64_t m_lastSeenTargetGeneration = 0;
};

} // namespace engine::scratch
