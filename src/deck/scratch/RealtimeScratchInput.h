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

} // namespace engine::scratch
