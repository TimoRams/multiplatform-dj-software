#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace engine::scratch {

struct ScratchControllerConfig {
    double throwThreshold = 0.35;
    double maxScratchSpeed = 6.0;
    double minScratchSpeed = 0.00005;
    double slowSpeedThreshold = 0.35;
    double slowVelocitySmoothingOld = 0.10;
    double fastVelocitySmoothingOld = 0.35;
    double noMoveDecayMs = 55.0;
    double noMoveDecayTauSec = 0.030;
    double releaseReturnTauSec = 0.220;
    double inertiaStopThreshold = 0.02;
};

// Virtual turntable controller. Hand movement drives playback velocity
// directly, with light adaptive smoothing for fast throws and clean slow drags.
// The release path ramps toward the current deck speed instead of decaying
// blindly to silence.
class ScratchController {
public:
    ScratchController() = default;

    void setConfig(const ScratchControllerConfig& config) { m_config = config; }

    void setTrackSampleRate(double sampleRate) noexcept;

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    void setInertiaEnabled(bool enabled) noexcept { m_inertiaEnabled.store(enabled, std::memory_order_relaxed); }
    void setNormalPlaybackSpeed(double speed) noexcept {
        m_normalPlaybackSpeed.store(std::clamp(speed, -m_config.maxScratchSpeed, m_config.maxScratchSpeed),
                                    std::memory_order_relaxed);
    }
    void setHandPositionSec(double seconds) noexcept {
        m_handPositionSec.store(seconds, std::memory_order_relaxed);
    }

    void startScratch(double audioSamplePos, bool wasPlayingBeforeScratch, double normalPlaybackSpeed) noexcept;
    void stopScratch() noexcept;
    void releaseScratch() noexcept;

    void setTouching(bool touching) noexcept { m_touching.store(touching, std::memory_order_relaxed); }

    // Control thread: deltaTrackSec / dtSec → normalized speed (1.0 = 1× track speed).
    void submitHandDelta(double deltaTrackSec, double dtSec) noexcept;

    // Control thread: keep integrated read sample counter aligned with hand position.
    void syncReadPositionSamples(double audioSamplePos) noexcept {
        m_readPosition.store(audioSamplePos, std::memory_order_relaxed);
    }

    // Audio thread — once per output block. Returns resampler rate (track samples / output sample).
    double processAudioBlock(int bufferSize, double outputSampleRate, double trackSampleRate) noexcept;

    [[nodiscard]] bool isScratching() const noexcept {
        return m_active.load(std::memory_order_relaxed)
            && m_touching.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isInertiaActive() const noexcept {
        return m_inertiaActive.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isActive() const noexcept { return m_active.load(std::memory_order_relaxed); }
    [[nodiscard]] bool touching() const noexcept { return m_touching.load(std::memory_order_relaxed); }
    [[nodiscard]] bool wasPlayingBeforeScratch() const noexcept {
        return m_wasPlayingBeforeScratch.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double normalizedRate() const noexcept {
        if (m_touching.load(std::memory_order_relaxed))
            return m_smoothedSpeed.load(std::memory_order_relaxed);
        if (m_inertiaActive.load(std::memory_order_relaxed))
            return m_inertiaSpeed.load(std::memory_order_relaxed);
        return 0.0;
    }

    [[nodiscard]] double rawSpeed() const noexcept { return m_rawSpeed.load(std::memory_order_relaxed); }
    [[nodiscard]] double smoothedSpeed() const noexcept {
        return m_smoothedSpeed.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double readPositionSamples() const noexcept {
        return m_readPosition.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double handPositionSec() const noexcept {
        return m_handPositionSec.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static uint64_t nowNs() noexcept;
    [[nodiscard]] double timeSinceLastMoveMs() const noexcept;
    [[nodiscard]] double oneXResamplerRate(double outputSampleRate, double trackSampleRate) const noexcept;

    ScratchControllerConfig m_config;

    std::atomic<bool> m_enabled { true };
    std::atomic<bool> m_inertiaEnabled { true };
    std::atomic<bool> m_active { false };
    std::atomic<bool> m_touching { false };
    std::atomic<bool> m_inertiaActive { false };
    std::atomic<bool> m_wasPlayingBeforeScratch { false };

    std::atomic<double> m_normalPlaybackSpeed { 1.0 };
    std::atomic<double> m_handPositionSec { 0.0 };
    std::atomic<double> m_rawSpeed { 0.0 };
    std::atomic<double> m_smoothedSpeed { 0.0 };
    std::atomic<double> m_inertiaSpeed { 0.0 };
    std::atomic<double> m_releaseTargetSpeed { 0.0 };
    std::atomic<double> m_readPosition { 0.0 };
    std::atomic<double> m_trackSampleRate { 44100.0 };
    std::atomic<uint64_t> m_lastMoveNs { 0 };
};

} // namespace engine::scratch
