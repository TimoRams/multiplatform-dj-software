#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace engine::scratch {

// UI/controller ingress: accumulates target scratch sample position only.
// Never seeks audio directly.
class VirtualTurntable {
public:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kNominalRpm = 33.0 + 1.0 / 3.0;
    static constexpr double kNominalDegreesPerSecond = 200.0;

    void reset(double startSamplePos, double trackSampleRate) noexcept;

    void setTrackSampleRate(double sampleRate) noexcept { m_trackSampleRate = std::max(1.0, sampleRate); }

    // UI platter: pointer angle delta in radians (wrap-aware caller).
    void addAngleDeltaRadians(double deltaRadians) noexcept;

    // UI platter: degrees delta (shortest-path wrapped).
    void addAngleDeltaDegrees(double deltaDegrees) noexcept;

    // Waveform / linear UI in seconds.
    void addTimeDeltaSeconds(double deltaSeconds) noexcept;

    void setAbsoluteSamplePosition(double samplePos) noexcept;
    void setAbsoluteTimeSeconds(double seconds) noexcept;

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

    void addTargetSampleDelta(double deltaSamples) noexcept;

private:
    std::atomic<double> m_targetSamplePos { 0.0 };
    double m_trackSampleRate = 44100.0;
    double m_displayAngleRad = 0.0;
};

} // namespace engine::scratch
