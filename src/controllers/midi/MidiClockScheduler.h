#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Pure MIDI beat-clock logic shared by the clock output thread and the clock
// input follower. No I/O, no allocation, deterministic for tests.
namespace brockdj::midiclock {

inline constexpr int kPulsesPerBeat = 24;
inline constexpr int kBeatsPerBar = 4;
inline constexpr std::uint8_t kClock = 0xF8;
inline constexpr std::uint8_t kStart = 0xFA;
inline constexpr std::uint8_t kContinue = 0xFB;
inline constexpr std::uint8_t kStop = 0xFC;
inline constexpr std::uint8_t kSongPosition = 0xF2;

struct ClockSnapshot {
    bool valid = false;
    bool running = false;
    double bpm = 120.0;
    double beat = 0.0;
    double timestamp = 0.0;
    std::uint64_t generation = 0;
};

[[nodiscard]] inline bool usableTempo(double bpm) noexcept
{
    return std::isfinite(bpm) && bpm >= 20.0 && bpm <= 400.0;
}

// Turns a jittery master beat position into an evenly spaced 24 PPQN pulse
// stream. Small phase errors are corrected gradually; loops, track changes and
// source switches (new generation) resynchronise without pulse bursts. START is
// quantised to the next bar so external gear begins its pattern on the downbeat.
class MidiClockScheduler final {
public:
    static constexpr double kCorrectionGain = 0.08;
    static constexpr double kHardResyncBeats = 0.25;
    static constexpr double kMaximumSleepSeconds = 0.004;

    void setTransportEnabled(bool enabled) noexcept { m_transportEnabled = enabled; }
    [[nodiscard]] bool transportRunning() const noexcept { return m_transportRunning; }
    [[nodiscard]] bool startPending() const noexcept { return m_startPending; }
    [[nodiscard]] double modelBeat() const noexcept { return m_modelBeat; }

    void reset() noexcept { *this = MidiClockScheduler{}; }

    // Emits due messages through sink(std::uint8_t status) and returns the
    // time at which advance() should run next.
    template <class Sink>
    double advance(const ClockSnapshot& snapshot, double now, Sink&& sink)
    {
        if (!snapshot.valid || !usableTempo(snapshot.bpm) || !std::isfinite(snapshot.beat)) {
            stopTransport(sink);
            m_locked = false;
            return now + kMaximumSleepSeconds;
        }

        const double target = snapshot.beat + (now - snapshot.timestamp) * snapshot.bpm / 60.0;
        if (!m_locked || snapshot.generation != m_generation) {
            hardSync(snapshot, target, now);
        } else {
            m_modelBeat += (now - m_modelTime) * m_bpm / 60.0;
            m_modelTime = now;
            m_bpm = snapshot.bpm;
            if (snapshot.timestamp != m_lastSnapshotTime) {
                m_lastSnapshotTime = snapshot.timestamp;
                const double error = target - m_modelBeat;
                if (std::abs(error) > kHardResyncBeats)
                    hardSync(snapshot, target, now);
                else
                    m_modelBeat += error * kCorrectionGain;
            }
        }

        if (!m_transportEnabled || !snapshot.running)
            stopTransport(sink);
        else if (!m_transportRunning && !m_startPending)
            scheduleStart();

        const auto current = static_cast<std::int64_t>(std::floor(m_modelBeat * kPulsesPerBeat + 1e-9));
        if (current > m_lastPulse + kPulsesPerBeat)
            m_lastPulse = current - 1;
        for (std::int64_t pulse = m_lastPulse + 1; pulse <= current; ++pulse) {
            if (m_startPending && pulse >= m_startPulse) {
                sink(kStart);
                m_startPending = false;
                m_transportRunning = true;
            }
            sink(kClock);
            m_lastPulse = pulse;
        }

        const double nextBeat = static_cast<double>(m_lastPulse + 1) / kPulsesPerBeat;
        const double nextTime = m_modelTime + (nextBeat - m_modelBeat) * 60.0 / m_bpm;
        return std::clamp(nextTime, now, now + kMaximumSleepSeconds);
    }

private:
    template <class Sink>
    void stopTransport(Sink&& sink)
    {
        if (m_transportRunning)
            sink(kStop);
        m_transportRunning = false;
        m_startPending = false;
    }

    void hardSync(const ClockSnapshot& snapshot, double target, double now) noexcept
    {
        m_locked = true;
        m_generation = snapshot.generation;
        m_lastSnapshotTime = snapshot.timestamp;
        m_bpm = snapshot.bpm;
        m_modelBeat = target;
        m_modelTime = now;
        m_lastPulse = static_cast<std::int64_t>(std::floor(target * kPulsesPerBeat + 1e-9));
        if (m_startPending)
            scheduleStart();
    }

    void scheduleStart() noexcept
    {
        constexpr std::int64_t pulsesPerBar = kPulsesPerBeat * kBeatsPerBar;
        const std::int64_t next = m_lastPulse + 1;
        const std::int64_t bar = next >= 0 ? (next + pulsesPerBar - 1) / pulsesPerBar
                                           : next / pulsesPerBar;
        m_startPulse = bar * pulsesPerBar;
        m_startPending = true;
    }

    bool m_locked = false;
    bool m_transportEnabled = true;
    bool m_transportRunning = false;
    bool m_startPending = false;
    double m_bpm = 120.0;
    double m_modelBeat = 0.0;
    double m_modelTime = 0.0;
    double m_lastSnapshotTime = -1.0;
    std::uint64_t m_generation = 0;
    std::int64_t m_lastPulse = 0;
    std::int64_t m_startPulse = 0;
};

// Follows an external 24 PPQN clock. Tempo is a least-squares fit over the
// most recent pulses, which rejects USB/driver timestamp jitter far better than
// the last pulse interval. START/SPP define the beat origin.
class MidiClockFollower final {
public:
    static constexpr std::size_t kWindow = 96;
    static constexpr std::size_t kMinimumPulses = 12;
    static constexpr double kTimeoutSeconds = 0.5;

    void handle(std::uint8_t status, double timestamp, std::uint8_t data1 = 0, std::uint8_t data2 = 0) noexcept
    {
        switch (status) {
        case kClock:
            if (m_count > 0 && timestamp - m_times[(m_head + kWindow - 1) % kWindow] > kTimeoutSeconds)
                m_count = 0;
            m_times[m_head] = timestamp;
            m_head = (m_head + 1) % kWindow;
            m_count = std::min(m_count + 1, kWindow);
            ++m_pulseIndex;
            m_lastPulseTime = timestamp;
            break;
        case kStart:
            m_pulseIndex = -1;
            m_running = true;
            ++m_generation;
            break;
        case kContinue:
            m_running = true;
            break;
        case kStop:
            m_running = false;
            break;
        case kSongPosition:
            m_pulseIndex = static_cast<std::int64_t>((data1 & 0x7f) | ((data2 & 0x7f) << 7)) * 6 - 1;
            ++m_generation;
            break;
        default:
            break;
        }
    }

    [[nodiscard]] double tempo() const noexcept
    {
        if (m_count < kMinimumPulses)
            return 0.0;
        const std::size_t first = (m_head + kWindow - m_count) % kWindow;
        const double origin = m_times[first];
        const double n = static_cast<double>(m_count);
        double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
        for (std::size_t i = 0; i < m_count; ++i) {
            const double x = static_cast<double>(i);
            const double y = m_times[(first + i) % kWindow] - origin;
            sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
        }
        const double denominator = n * sumXX - sumX * sumX;
        if (denominator <= 0.0)
            return 0.0;
        const double secondsPerPulse = (n * sumXY - sumX * sumY) / denominator;
        if (!(secondsPerPulse > 0.0))
            return 0.0;
        const double bpm = 60.0 / (secondsPerPulse * kPulsesPerBeat);
        return usableTempo(bpm) ? bpm : 0.0;
    }

    [[nodiscard]] ClockSnapshot snapshot(double now) const noexcept
    {
        ClockSnapshot result;
        const double bpm = tempo();
        result.valid = bpm > 0.0 && now - m_lastPulseTime <= kTimeoutSeconds;
        result.running = result.valid && m_running;
        result.bpm = bpm > 0.0 ? bpm : 120.0;
        result.beat = static_cast<double>(std::max<std::int64_t>(m_pulseIndex, 0)) / kPulsesPerBeat;
        result.timestamp = m_lastPulseTime;
        result.generation = m_generation;
        return result;
    }

    void reset() noexcept { *this = MidiClockFollower{}; }

private:
    std::array<double, kWindow> m_times {};
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    std::int64_t m_pulseIndex = -1;
    double m_lastPulseTime = -1.0e9;
    bool m_running = false;
    std::uint64_t m_generation = 1;
};

} // namespace brockdj::midiclock
