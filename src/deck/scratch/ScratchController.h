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
    double commandVelocityHoldMs = 8.0;
    double commandVelocityDecayTauMs = 7.0;
    double releaseReturnTauSec = 0.220;
    double inertiaStopThreshold = 0.02;
    // When normal playback is waiting in the opposite direction, hand off
    // before the backspin becomes an audible near-zero hold. Pause releases
    // continue to use inertiaStopThreshold and therefore still coast to zero.
    double crossDirectionHandoffThreshold = 0.10;
};

enum class ScratchReleaseDisposition : std::uint8_t {
    HandoffNow,
    CoastToDeckRate,
    CoastToStop
};

enum class ScratchPhase : std::uint8_t {
    Idle,
    TouchTracking,
    ReleasePending,
    CoastToDeckRate,
    CoastToStop,
    HandoffPending
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
    // Control thread: publish touch-up without taking ownership away from the
    // audio callback. The callback resolves the release after its final
    // position-tracking block.
    [[nodiscard]] bool requestRelease(bool allowInertia) noexcept;
    [[nodiscard]] ScratchReleaseDisposition releaseScratch() noexcept;
    // Audio thread: resolve ReleasePending from the actually rendered speed.
    [[nodiscard]] ScratchReleaseDisposition releaseScratchWithSpeed(
        double measuredNormalizedSpeed,
        bool allowInertia) noexcept;
    [[nodiscard]] ScratchReleaseDisposition releaseScratchWithSpeed(
        double measuredNormalizedSpeed,
        bool allowInertia,
        double signedDeckSpeed,
        bool wasPlaying) noexcept;
    // Audio thread: retarget an already-running coast after Play/Pause changes.
    [[nodiscard]] ScratchReleaseDisposition retargetRelease(
        bool playbackIntent,
        double signedDeckSpeed) noexcept;
    // Audio thread: acknowledge only the release that still owns the phase.
    // A concurrent re-grab has already changed the phase and makes this fail.
    [[nodiscard]] bool completeHandoff() noexcept;

    void setTouching(bool touching) noexcept;

    // Control thread: deltaTrackSec / dtSec → normalized speed (1.0 = 1× track speed).
    void submitHandDelta(double deltaTrackSec, double dtSec) noexcept;
    // Control thread: physical top-platter motion that arrives after touch-up.
    // It may refine an existing coast but cannot restart a completed scratch.
    void submitReleaseDelta(double deltaTrackSec, double dtSec) noexcept;
    // Control thread: publish an already measured normalized wheel speed.
    // The audio callback consumes it and owns all phase transitions.
    void submitReleaseSpeed(double measuredNormalizedSpeed) noexcept;

    // Control thread: keep integrated read sample counter aligned with hand position.
    void syncReadPositionSamples(double audioSamplePos) noexcept {
        m_readPosition.store(audioSamplePos, std::memory_order_relaxed);
    }

    // Audio thread: report the velocity the position tracker is actually producing,
    // so release/inertia throws and scratch timbre use the true playback speed.
    void setMeasuredNormalizedSpeed(double normalized) noexcept {
        const double s = std::clamp(normalized, -m_config.maxScratchSpeed, m_config.maxScratchSpeed);
        m_smoothedSpeed.store(s, std::memory_order_relaxed);
        m_rawSpeed.store(s, std::memory_order_relaxed);
    }

    // Audio thread — once per output block. Returns resampler rate (track samples / output sample).
    double processAudioBlock(int bufferSize, double outputSampleRate, double trackSampleRate) noexcept;

    [[nodiscard]] bool isScratching() const noexcept {
        return phase() == ScratchPhase::TouchTracking;
    }
    [[nodiscard]] bool isInertiaActive() const noexcept {
        const auto current = phase();
        return current == ScratchPhase::CoastToDeckRate
            || current == ScratchPhase::CoastToStop;
    }
    [[nodiscard]] bool isActive() const noexcept { return phase() != ScratchPhase::Idle; }
    [[nodiscard]] bool touching() const noexcept {
        return phase() == ScratchPhase::TouchTracking;
    }
    [[nodiscard]] bool requiresPositionTracking() const noexcept {
        const auto current = phase();
        return current == ScratchPhase::TouchTracking
            || current == ScratchPhase::ReleasePending;
    }
    [[nodiscard]] bool handoffPending() const noexcept {
        return phase() == ScratchPhase::HandoffPending;
    }
    [[nodiscard]] ScratchPhase phase() const noexcept {
        return m_phase.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool releaseAllowsInertia() const noexcept {
        return m_releaseAllowsInertia.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool wasPlayingBeforeScratch() const noexcept {
        return m_wasPlayingBeforeScratch.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double normalizedRate() const noexcept {
        const auto current = phase();
        if (current == ScratchPhase::TouchTracking
            || current == ScratchPhase::ReleasePending)
            return m_smoothedSpeed.load(std::memory_order_relaxed);
        if (current == ScratchPhase::CoastToDeckRate
            || current == ScratchPhase::CoastToStop)
            return m_inertiaSpeed.load(std::memory_order_relaxed);
        if (current == ScratchPhase::HandoffPending)
            return m_releaseTargetSpeed.load(std::memory_order_relaxed);
        return 0.0;
    }

    [[nodiscard]] double rawSpeed() const noexcept { return m_rawSpeed.load(std::memory_order_relaxed); }
    [[nodiscard]] double smoothedSpeed() const noexcept {
        return m_smoothedSpeed.load(std::memory_order_relaxed);
    }
    // Latest hand-command velocity, decayed quickly when the controller stops
    // producing ticks. Unlike smoothedSpeed(), audio feedback never overwrites
    // this value, so the position tracker can interpolate sparse MIDI events.
    [[nodiscard]] double commandedHandSpeed() const noexcept;
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
    std::atomic<bool> m_wasPlayingBeforeScratch { false };
    std::atomic<bool> m_releaseAllowsInertia { true };
    std::atomic<ScratchPhase> m_phase { ScratchPhase::Idle };

    std::atomic<double> m_normalPlaybackSpeed { 1.0 };
    std::atomic<double> m_handPositionSec { 0.0 };
    std::atomic<double> m_rawSpeed { 0.0 };
    std::atomic<double> m_smoothedSpeed { 0.0 };
    std::atomic<double> m_commandedHandSpeed { 0.0 };
    std::atomic<double> m_inertiaSpeed { 0.0 };
    std::atomic<double> m_releaseTargetSpeed { 0.0 };
    std::atomic<double> m_releaseHandoffThreshold { 0.02 };
    std::atomic<bool> m_preserveMomentumAtHandoff { false };
    std::atomic<double> m_readPosition { 0.0 };
    std::atomic<double> m_trackSampleRate { 44100.0 };
    std::atomic<uint64_t> m_lastMoveNs { 0 };
    std::atomic<double> m_submittedReleaseSpeed { 0.0 };
    std::atomic<uint64_t> m_submittedReleaseEpoch { 0 };
    std::atomic<uint64_t> m_releaseSpeedGeneration { 0 };
    std::atomic<uint64_t> m_appliedReleaseSpeedGeneration { 0 };
    std::atomic<uint64_t> m_releaseEpoch { 0 };
};

} // namespace engine::scratch
