#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Converts the lock-free audio peak snapshot into the one continuous 7-bit
// bar-height value exposed by the FLX10 channel-meter MIDI protocol. The
// controller firmware owns the physical segment colours and peak LED hold.
class LevelMeterBallistics final {
public:
    static constexpr float kPeakEpsilon = 1.0e-9f;
    static constexpr float kFloorDb = -54.0f;
    static constexpr float kCeilingDb = 12.0f;
    static constexpr float kReleaseDbPerSecond = 30.0f;

    [[nodiscard]] static float linearPeakToDb(float peak) noexcept
    {
        return 20.0f * std::log10(std::max(peak, kPeakEpsilon));
    }

    // Pioneer/AlphaTheta's meter payload is not a generic 0..127 dBFS bar.
    // The documented family calibration is approximately:
    // -24 dB -> 50, -18 -> 60, -12 -> 70, -6 -> 79, 0 -> 89,
    // +3 -> 94, +6 -> 99, +9 -> 104, +12/red -> 109.
    [[nodiscard]] static std::uint8_t dbToPioneerMidi(float db) noexcept
    {
        if (db <= kFloorDb)
            return 0;
        if (db < -24.0f) {
            const float value = (db - kFloorDb) * (50.0f / (-24.0f - kFloorDb));
            return static_cast<std::uint8_t>(std::lround(value));
        }
        if (db <= 0.0f) {
            const float value = 50.0f + (db + 24.0f) * (39.0f / 24.0f);
            return static_cast<std::uint8_t>(std::lround(value));
        }
        const float value = 89.0f + db * (20.0f / 12.0f);
        return static_cast<std::uint8_t>(std::lround(std::min(value, 109.0f)));
    }

    void reset() noexcept
    {
        m_displayDb = kFloorDb;
        m_lastMidiValue = 0;
    }

    [[nodiscard]] std::uint8_t update(float linearPeak, double deltaSeconds) noexcept
    {
        // The regular Pioneer scale deliberately leaves headroom at 0 dBFS,
        // but an actual floating-point over must still drive the red/clip LED.
        // Keep this decision on the unsmoothed peak so even a short over is not
        // hidden by the display release ballistics.
        constexpr float kClipThreshold = 1.001f;
        const bool clipping = linearPeak > kClipThreshold;
        const float signalDb = std::clamp(linearPeakToDb(linearPeak), kFloorDb, kCeilingDb);
        const float dt = static_cast<float>(
            std::clamp(deltaSeconds > 0.0 ? deltaSeconds : (1.0 / 30.0),
                       1.0 / 240.0, 0.100));

        // DJ-mixer response: peaks rise immediately, then decay visibly slower.
        if (signalDb >= m_displayDb)
            m_displayDb = signalDb;
        else
            m_displayDb = std::max(signalDb, m_displayDb - kReleaseDbPerSecond * dt);

        const auto candidate = clipping
            ? static_cast<std::uint8_t>(109)
            : dbToPioneerMidi(m_displayDb);

        // A one-step dead band prevents adjacent-height chatter without making
        // attacks sluggish. Zero and full-scale always propagate immediately.
        if (candidate != 0 && candidate != 109
            && std::abs(static_cast<int>(candidate) - static_cast<int>(m_lastMidiValue)) <= 1)
            return m_lastMidiValue;

        m_lastMidiValue = candidate;
        return candidate;
    }

    [[nodiscard]] float displayDb() const noexcept { return m_displayDb; }

private:
    float m_displayDb = kFloorDb;
    std::uint8_t m_lastMidiValue = 0;
};
