#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

struct AudioPresentationPosition {
    double seconds = 0.0;
    double timestamp = 0.0;
    double rate = 0.0;
    double length = 0.0;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    std::uint64_t trackGeneration = 0;
    std::uint64_t seekGeneration = 0;

    [[nodiscard]] double positionAt(double now, double latency = 0.0) const noexcept
    {
        // Never let a stalled/disconnected device turn into an unbounded
        // free-running display clock.
        const double elapsed = std::clamp(now - timestamp, -0.250, 0.050);
        double position = seconds + (elapsed - latency) * rate;
        if (loopEnd > loopStart
            && ((position >= loopEnd && (rate > 0.0 || seconds < loopEnd))
                || (position < loopStart && (rate < 0.0 || seconds >= loopStart)))) {
            const double span = loopEnd - loopStart;
            position = loopStart + std::fmod(std::fmod(position - loopStart, span) + span, span);
        }
        return std::clamp(position, 0.0, length);
    }
};

// One audio writer (or the control owner while the pipeline swap gate is
// closed), read-only GUI consumers. All payload fields are atomic as readers
// can overlap publication; the sequence makes the values one coherent block.
class AudioPresentationClock {
public:
    void publish(const AudioPresentationPosition& value) noexcept
    {
        m_sequence.fetch_add(1, std::memory_order_acq_rel);
        m_seconds.store(value.seconds, std::memory_order_relaxed);
        m_timestamp.store(value.timestamp, std::memory_order_relaxed);
        m_rate.store(value.rate, std::memory_order_relaxed);
        m_length.store(value.length, std::memory_order_relaxed);
        m_loopStart.store(value.loopStart, std::memory_order_relaxed);
        m_loopEnd.store(value.loopEnd, std::memory_order_relaxed);
        m_track.store(value.trackGeneration, std::memory_order_relaxed);
        m_seek.store(value.seekGeneration, std::memory_order_relaxed);
        m_sequence.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] AudioPresentationPosition snapshot() const noexcept
    {
        AudioPresentationPosition value;
        for (;;) {
            const auto before = m_sequence.load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            value.seconds = m_seconds.load(std::memory_order_relaxed);
            value.timestamp = m_timestamp.load(std::memory_order_relaxed);
            value.rate = m_rate.load(std::memory_order_relaxed);
            value.length = m_length.load(std::memory_order_relaxed);
            value.loopStart = m_loopStart.load(std::memory_order_relaxed);
            value.loopEnd = m_loopEnd.load(std::memory_order_relaxed);
            value.trackGeneration = m_track.load(std::memory_order_relaxed);
            value.seekGeneration = m_seek.load(std::memory_order_relaxed);
            if (before == m_sequence.load(std::memory_order_acquire))
                return value;
        }
    }

private:
    std::atomic<std::uint64_t> m_sequence {0}, m_track {0}, m_seek {0};
    std::atomic<double> m_seconds {0.0}, m_timestamp {0.0}, m_rate {0.0};
    std::atomic<double> m_length {0.0}, m_loopStart {0.0}, m_loopEnd {0.0};
};
