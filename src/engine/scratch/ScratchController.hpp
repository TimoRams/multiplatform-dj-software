#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace engine::scratch {

struct ScratchControllerConfig {
    double throwThreshold = 0.50;
    double maxScratchSpeed = 8.0;
    double minScratchSpeed = 0.00005;
    double velocitySmoothingOld = 0.20;
    double velocitySmoothingNew = 0.80;
    double noMoveDecayMs = 30.0;
    double noMoveDecayFactor = 0.70;
    double inertiaFrictionPerBlock = 0.985;
    double inertiaStopThreshold = 0.02;
};

// Velocity-based virtual turntable: hand speed drives playback rate directly while
// touching. No PD follower and no friction while the platter is held.
class ScratchController {
public:
    ScratchController() = default;

    void setConfig(const ScratchControllerConfig& config) { m_config = config; }

    void setTrackSampleRate(double sampleRate) noexcept;

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    void setInertiaEnabled(bool enabled) noexcept { m_inertiaEnabled.store(enabled, std::memory_order_relaxed); }

    void startScratch(double audioSamplePos, bool wasPlayingBeforeScratch, double normalPlaybackSpeed) noexcept;
    void stopScratch() noexcept;
    void releaseScratch() noexcept;

    void setTouching(bool touching) noexcept { m_touching.store(touching, std::memory_order_relaxed); }

    // Control thread: deltaTrackSec / dtSec → normalized speed (1.0 = 1× track speed).
    void submitHandDelta(double deltaTrackSec, double dtSec) noexcept;

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
    std::atomic<double> m_rawSpeed { 0.0 };
    std::atomic<double> m_smoothedSpeed { 0.0 };
    std::atomic<double> m_inertiaSpeed { 0.0 };
    std::atomic<double> m_readPosition { 0.0 };
    std::atomic<double> m_trackSampleRate { 44100.0 };
    std::atomic<uint64_t> m_lastMoveNs { 0 };

    int m_debugBlockCounter = 0;
};

} // namespace engine::scratch
