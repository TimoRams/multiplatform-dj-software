#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace engine::scratch {

enum class RealtimeScratchPhase : std::uint8_t {
    Idle,
    TouchTracking,
    Released
};

struct RealtimeScratchSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t motionSequence = 0;
    double cumulativeDeltaSeconds = 0.0;
    double velocity = 0.0;
    double eventIntervalSeconds = 0.0;
    double lastEventTimestampSeconds = 0.0;
    RealtimeScratchPhase phase = RealtimeScratchPhase::Idle;

    [[nodiscard]] bool touching() const noexcept {
        return phase == RealtimeScratchPhase::TouchTracking;
    }
};

// A controller-thread producer publishes the physical platter trajectory here;
// the audio callback reads one coherent snapshot without taking a lock or
// spinning. All fields are atomic because a failed seqlock read must still be a
// legal concurrent read. Writers are serialized away from the audio thread.
class RealtimeScratchInput final {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "Scratch handoff requires lock-free sequence atomics");
    static_assert(std::atomic<double>::is_always_lock_free,
                  "Scratch handoff requires lock-free motion atomics");
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "Scratch handoff requires lock-free phase atomics");
    [[nodiscard]] static double clockSeconds() noexcept
    {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::uint64_t beginTouch(double timestampSeconds) noexcept
    {
        const std::lock_guard lock(m_writerMutex);
        beginWrite();
        const auto generation = m_generation.load(std::memory_order_relaxed) + 1;
        m_generation.store(generation, std::memory_order_relaxed);
        m_motionSequence.store(0, std::memory_order_relaxed);
        m_cumulativeDeltaSeconds.store(0.0, std::memory_order_relaxed);
        m_velocity.store(0.0, std::memory_order_relaxed);
        m_eventIntervalSeconds.store(0.0, std::memory_order_relaxed);
        m_lastEventTimestampSeconds.store(sanitizedTimestamp(timestampSeconds),
                                          std::memory_order_relaxed);
        m_phase.store(static_cast<std::uint8_t>(RealtimeScratchPhase::TouchTracking),
                      std::memory_order_relaxed);
        endWrite();
        return generation;
    }

    void publishTouchMotion(double deltaSeconds,
                            double velocity,
                            double eventIntervalSeconds,
                            double timestampSeconds) noexcept
    {
        if (!std::isfinite(deltaSeconds) || deltaSeconds == 0.0)
            return;

        const std::lock_guard lock(m_writerMutex);
        if (phaseRelaxed() != RealtimeScratchPhase::TouchTracking)
            return;

        beginWrite();
        const double previous = m_cumulativeDeltaSeconds.load(std::memory_order_relaxed);
        m_cumulativeDeltaSeconds.store(previous + deltaSeconds,
                                       std::memory_order_relaxed);
        m_velocity.store(sanitizedVelocity(velocity), std::memory_order_relaxed);
        if (std::isfinite(eventIntervalSeconds) && eventIntervalSeconds > 0.0) {
            m_eventIntervalSeconds.store(
                std::clamp(eventIntervalSeconds, 1.0e-6, 0.120),
                std::memory_order_relaxed);
        }
        m_lastEventTimestampSeconds.store(sanitizedTimestamp(timestampSeconds),
                                          std::memory_order_relaxed);
        m_motionSequence.fetch_add(1, std::memory_order_relaxed);
        endWrite();
    }

    void endTouch(double releaseVelocity, double timestampSeconds) noexcept
    {
        const std::lock_guard lock(m_writerMutex);
        if (phaseRelaxed() != RealtimeScratchPhase::TouchTracking)
            return;

        beginWrite();
        m_velocity.store(sanitizedVelocity(releaseVelocity),
                         std::memory_order_relaxed);
        m_lastEventTimestampSeconds.store(sanitizedTimestamp(timestampSeconds),
                                          std::memory_order_relaxed);
        m_phase.store(static_cast<std::uint8_t>(RealtimeScratchPhase::Released),
                      std::memory_order_relaxed);
        m_motionSequence.fetch_add(1, std::memory_order_relaxed);
        endWrite();
    }

    void publishReleaseVelocity(double velocity,
                                double eventIntervalSeconds,
                                double timestampSeconds) noexcept
    {
        const std::lock_guard lock(m_writerMutex);
        if (phaseRelaxed() != RealtimeScratchPhase::Released)
            return;

        beginWrite();
        m_velocity.store(sanitizedVelocity(velocity), std::memory_order_relaxed);
        if (std::isfinite(eventIntervalSeconds) && eventIntervalSeconds > 0.0) {
            m_eventIntervalSeconds.store(
                std::clamp(eventIntervalSeconds, 1.0e-6, 0.120),
                std::memory_order_relaxed);
        }
        m_lastEventTimestampSeconds.store(sanitizedTimestamp(timestampSeconds),
                                          std::memory_order_relaxed);
        m_motionSequence.fetch_add(1, std::memory_order_relaxed);
        endWrite();
    }

    void reset(double timestampSeconds = 0.0) noexcept
    {
        const std::lock_guard lock(m_writerMutex);
        beginWrite();
        m_generation.fetch_add(1, std::memory_order_relaxed);
        m_motionSequence.store(0, std::memory_order_relaxed);
        m_cumulativeDeltaSeconds.store(0.0, std::memory_order_relaxed);
        m_velocity.store(0.0, std::memory_order_relaxed);
        m_eventIntervalSeconds.store(0.0, std::memory_order_relaxed);
        m_lastEventTimestampSeconds.store(sanitizedTimestamp(timestampSeconds),
                                          std::memory_order_relaxed);
        m_phase.store(static_cast<std::uint8_t>(RealtimeScratchPhase::Idle),
                      std::memory_order_relaxed);
        endWrite();
    }

    // Audio thread: one attempt only. The caller retains its previous snapshot
    // if a producer happens to be publishing during this exact callback edge.
    [[nodiscard]] bool tryRead(RealtimeScratchSnapshot& snapshot) const noexcept
    {
        const auto before = m_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U)
            return false;

        RealtimeScratchSnapshot candidate;
        candidate.generation = m_generation.load(std::memory_order_relaxed);
        candidate.motionSequence = m_motionSequence.load(std::memory_order_relaxed);
        candidate.cumulativeDeltaSeconds =
            m_cumulativeDeltaSeconds.load(std::memory_order_relaxed);
        candidate.velocity = m_velocity.load(std::memory_order_relaxed);
        candidate.eventIntervalSeconds =
            m_eventIntervalSeconds.load(std::memory_order_relaxed);
        candidate.lastEventTimestampSeconds =
            m_lastEventTimestampSeconds.load(std::memory_order_relaxed);
        candidate.phase = static_cast<RealtimeScratchPhase>(
            m_phase.load(std::memory_order_relaxed));

        const auto after = m_sequence.load(std::memory_order_acquire);
        if (before != after || (after & 1U) != 0U)
            return false;

        snapshot = candidate;
        return true;
    }

    // Control/UI thread only. This may briefly wait for the MIDI producer and
    // is used solely to bind a touch generation to a new scratch session.
    [[nodiscard]] RealtimeScratchSnapshot readForControlThread() const noexcept
    {
        const std::lock_guard lock(m_writerMutex);
        RealtimeScratchSnapshot snapshot;
        snapshot.generation = m_generation.load(std::memory_order_relaxed);
        snapshot.motionSequence = m_motionSequence.load(std::memory_order_relaxed);
        snapshot.cumulativeDeltaSeconds =
            m_cumulativeDeltaSeconds.load(std::memory_order_relaxed);
        snapshot.velocity = m_velocity.load(std::memory_order_relaxed);
        snapshot.eventIntervalSeconds =
            m_eventIntervalSeconds.load(std::memory_order_relaxed);
        snapshot.lastEventTimestampSeconds =
            m_lastEventTimestampSeconds.load(std::memory_order_relaxed);
        snapshot.phase = phaseRelaxed();
        return snapshot;
    }

private:
    [[nodiscard]] static double sanitizedTimestamp(double timestampSeconds) noexcept
    {
        return std::isfinite(timestampSeconds) && timestampSeconds > 0.0
            ? timestampSeconds : clockSeconds();
    }

    [[nodiscard]] static double sanitizedVelocity(double velocity) noexcept
    {
        return std::clamp(std::isfinite(velocity) ? velocity : 0.0, -8.0, 8.0);
    }

    [[nodiscard]] RealtimeScratchPhase phaseRelaxed() const noexcept
    {
        return static_cast<RealtimeScratchPhase>(m_phase.load(std::memory_order_relaxed));
    }

    void beginWrite() noexcept
    {
        m_sequence.fetch_add(1, std::memory_order_acq_rel);
    }

    void endWrite() noexcept
    {
        m_sequence.fetch_add(1, std::memory_order_release);
    }

    mutable std::mutex m_writerMutex;
    std::atomic<std::uint64_t> m_sequence { 0 };
    std::atomic<std::uint64_t> m_generation { 0 };
    std::atomic<std::uint64_t> m_motionSequence { 0 };
    std::atomic<double> m_cumulativeDeltaSeconds { 0.0 };
    std::atomic<double> m_velocity { 0.0 };
    std::atomic<double> m_eventIntervalSeconds { 0.0 };
    std::atomic<double> m_lastEventTimestampSeconds { 0.0 };
    std::atomic<std::uint8_t> m_phase {
        static_cast<std::uint8_t>(RealtimeScratchPhase::Idle) };
};

class VirtualTurntable {
public:
    static_assert(std::atomic<double>::is_always_lock_free,
                  "Platter position must be lock-free on supported targets");
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kNominalRpm = 33.0 + 1.0 / 3.0;
    static constexpr double kNominalDegreesPerSecond = 200.0;

    void reset(double startSamplePos, double trackSampleRate) noexcept {
        m_trackSampleRate = std::max(1.0, trackSampleRate);
        m_targetSamplePos.store(startSamplePos, std::memory_order_relaxed);
        m_displayAngleRad = 0.0;
    }

    void setTrackSampleRate(double sampleRate) noexcept
    { m_trackSampleRate = std::max(1.0, sampleRate); }

    // UI platter: pointer angle delta in radians (wrap-aware caller).
    void addAngleDeltaRadians(double deltaRadians) noexcept {
        if (std::abs(deltaRadians) < 1e-12)
            return;

        m_displayAngleRad += deltaRadians;
        const double spr = samplesPerRadian(m_trackSampleRate);
        addTargetSampleDelta(deltaRadians * spr);
    }

    // UI platter: degrees delta (shortest-path wrapped).
    void addAngleDeltaDegrees(double deltaDegrees) noexcept {
        addAngleDeltaRadians(deltaDegrees * kPi / 180.0);
    }

    // Waveform / linear UI in seconds.
    void addTimeDeltaSeconds(double deltaSeconds) noexcept {
        if (deltaSeconds == 0.0)
            return;
        addTargetSampleDelta(deltaSeconds * m_trackSampleRate);
        m_displayAngleRad += (deltaSeconds * kNominalRpm / 60.0) * 2.0 * kPi;
    }

    void setAbsoluteSamplePosition(double samplePos) noexcept {
        m_targetSamplePos.store(samplePos, std::memory_order_relaxed);
    }

    void setAbsoluteTimeSeconds(double seconds) noexcept {
        setAbsoluteSamplePosition(seconds * m_trackSampleRate);
    }

    [[nodiscard]] double targetSamplePosition() const noexcept {
        return m_targetSamplePos.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double displayAngleRadians() const noexcept { return m_displayAngleRad; }
    [[nodiscard]] double displayAngleDegrees() const noexcept {
        double deg = m_displayAngleRad * 180.0 / kPi;
        if (deg < 0.0)
            deg += 360.0;
        return std::fmod(deg, 360.0);
    }

    [[nodiscard]] static double samplesPerRadian(double trackSampleRate) noexcept {
        return (trackSampleRate * 60.0) / (kNominalRpm * 2.0 * kPi);
    }

    void addTargetSampleDelta(double deltaSamples) noexcept {
        const double next = m_targetSamplePos.load(std::memory_order_relaxed) + deltaSamples;
        m_targetSamplePos.store(next, std::memory_order_relaxed);
    }

private:
    std::atomic<double> m_targetSamplePos { 0.0 };
    double m_trackSampleRate = 44100.0;
    double m_displayAngleRad = 0.0;
};

} // namespace engine::scratch
