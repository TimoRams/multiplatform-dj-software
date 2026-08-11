#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace flx10 {

// The FLX10 relative CC stream yields roughly 12,750 semantic ticks per
// physical revolution. Keep this conversion beside the jog state machine that
// owns it; it is not part of the display/HID wire protocol.
constexpr double kScratchIntervalsPerRevolution = 12750.0;
constexpr double kVinylRpm = 33.0 + 1.0 / 3.0;
constexpr double kJogSpeedWindowSeconds = 0.032;
constexpr double kJogSpeedStaleSeconds = 0.060;
constexpr double kJogTailSuppressionSeconds = 0.120;

constexpr double scratchDeltaSeconds(double ticks) noexcept
{
    return ticks * (60.0 / kVinylRpm) / kScratchIntervalsPerRevolution;
}

constexpr int relativeTicksFromRaw(int rawValue) noexcept
{
    const int raw = rawValue < 0 ? 0 : (rawValue > 127 ? 127 : rawValue);
    return raw - 0x40;
}

enum class JogPhase {
    Idle,
    TouchTracking,
    ReleaseOwned,
    TailSuppression
};

enum class JogEventType {
    TouchDown,
    TouchUp,
    Platter,
    Rim,
    Generic
};

enum class JogRouteAction {
    Ignore,
    BeginScratch,
    RequestRelease,
    ScratchDelta,
    ReleaseDelta,
    Nudge
};

struct JogInput {
    JogEventType type = JogEventType::Generic;
    double ticks = 0.0;
    double timestampSeconds = 0.0;
    bool engineReleaseActive = false;
};

struct JogRouteResult {
    JogRouteAction action = JogRouteAction::Ignore;
    JogPhase phase = JogPhase::Idle;
    double ticks = 0.0;
    double deltaSeconds = 0.0;
    double estimatedRate = 0.0;
    double eventIntervalSeconds = 0.0;
};

// Fixed-capacity estimator for the signed platter rate. Samples retain their
// original MIDI timestamps, including timestamps from a batched callback.
class JogSpeedEstimator {
public:
    static constexpr std::size_t kCapacity = 32;

    void reset(double timestampSeconds = 0.0) noexcept
    {
        m_count = 0;
        m_cumulativeTicks = 0.0;
        m_baseTicks = 0.0;
        m_baseTimestampSeconds = timestampSeconds;
        m_hasBase = std::isfinite(timestampSeconds);
        m_lastEventIntervalSeconds = 0.0;
    }

    double push(double ticks, double timestampSeconds) noexcept
    {
        if (!std::isfinite(ticks) || ticks == 0.0 || !std::isfinite(timestampSeconds))
            return rate(timestampSeconds);

        if (!m_hasBase)
            reset(timestampSeconds);

        if (m_count > 0 && timestampSeconds < m_samples[m_count - 1].timestampSeconds)
            reset(timestampSeconds);

        m_lastEventIntervalSeconds = m_count > 0
            ? timestampSeconds - m_samples[m_count - 1].timestampSeconds
            : 0.0;

        m_cumulativeTicks += ticks;
        if (m_count == kCapacity) {
            m_baseTimestampSeconds = m_samples[0].timestampSeconds;
            m_baseTicks = m_samples[0].cumulativeTicks;
            for (std::size_t i = 1; i < m_count; ++i)
                m_samples[i - 1] = m_samples[i];
            --m_count;
        }

        m_samples[m_count++] = {timestampSeconds, m_cumulativeTicks};
        trimToWindow(timestampSeconds);
        return rate(timestampSeconds);
    }

    [[nodiscard]] double rate(double nowSeconds) const noexcept
    {
        if (!m_hasBase || m_count == 0 || !std::isfinite(nowSeconds))
            return 0.0;

        const auto& newest = m_samples[m_count - 1];
        const double ageSeconds = nowSeconds - newest.timestampSeconds;
        if (ageSeconds < 0.0 || ageSeconds > kJogSpeedStaleSeconds)
            return 0.0;

        const double elapsedSeconds = newest.timestampSeconds - m_baseTimestampSeconds;
        if (!(elapsedSeconds > 0.0))
            return 0.0;

        const double ticks = newest.cumulativeTicks - m_baseTicks;
        return scratchDeltaSeconds(ticks) / elapsedSeconds;
    }

    [[nodiscard]] std::size_t sampleCount() const noexcept { return m_count; }
    [[nodiscard]] double lastEventIntervalSeconds() const noexcept
    {
        return m_lastEventIntervalSeconds;
    }

private:
    struct Sample {
        double timestampSeconds = 0.0;
        double cumulativeTicks = 0.0;
    };

    void trimToWindow(double newestTimestampSeconds) noexcept
    {
        if (m_count == 1
            && newestTimestampSeconds - m_baseTimestampSeconds > kJogSpeedWindowSeconds) {
            m_baseTimestampSeconds = m_samples[0].timestampSeconds;
            m_baseTicks = m_samples[0].cumulativeTicks;
            return;
        }

        while (m_count > 1
               && newestTimestampSeconds - m_baseTimestampSeconds > kJogSpeedWindowSeconds) {
            m_baseTimestampSeconds = m_samples[0].timestampSeconds;
            m_baseTicks = m_samples[0].cumulativeTicks;
            for (std::size_t i = 1; i < m_count; ++i)
                m_samples[i - 1] = m_samples[i];
            --m_count;
        }

        if (m_count == 1
            && newestTimestampSeconds - m_baseTimestampSeconds > kJogSpeedWindowSeconds) {
            m_baseTimestampSeconds = m_samples[0].timestampSeconds;
            m_baseTicks = m_samples[0].cumulativeTicks;
        }
    }

    std::array<Sample, kCapacity> m_samples {};
    std::size_t m_count = 0;
    double m_cumulativeTicks = 0.0;
    double m_baseTimestampSeconds = 0.0;
    double m_baseTicks = 0.0;
    double m_lastEventIntervalSeconds = 0.0;
    bool m_hasBase = false;
};

class Flx10JogRouter {
public:
    [[nodiscard]] JogRouteResult route(const JogInput& input) noexcept
    {
        observeReleaseCompletion(input);
        expireTailSuppression(input.timestampSeconds);

        switch (input.type) {
        case JogEventType::TouchDown:
            return handleTouchDown(input);
        case JogEventType::TouchUp:
            return handleTouchUp(input);
        case JogEventType::Platter:
            return handlePlatter(input);
        case JogEventType::Rim:
            return handleRim(input);
        case JogEventType::Generic:
            return handleGeneric(input);
        }

        return result(JogRouteAction::Ignore, input);
    }

    [[nodiscard]] JogPhase phase() const noexcept { return m_phase; }
    [[nodiscard]] double estimatedRate(double nowSeconds) const noexcept
    {
        return m_speed.rate(nowSeconds);
    }

    void reset() noexcept
    {
        m_phase = JogPhase::Idle;
        m_releaseRequestPublished = false;
        m_tailLastMotionSeconds = 0.0;
        m_speed.reset();
    }

private:
    [[nodiscard]] JogRouteResult result(JogRouteAction action,
                                        const JogInput& input,
                                        double rate,
                                        double eventIntervalSeconds = 0.0) const noexcept
    {
        return {action,
                m_phase,
                input.ticks,
                scratchDeltaSeconds(input.ticks),
                rate,
                eventIntervalSeconds};
    }

    [[nodiscard]] JogRouteResult result(JogRouteAction action,
                                        const JogInput& input) const noexcept
    {
        return result(action, input, m_speed.rate(input.timestampSeconds));
    }

    void observeReleaseCompletion(const JogInput& input) noexcept
    {
        if (m_phase != JogPhase::ReleaseOwned || !m_releaseRequestPublished)
            return;

        if (input.engineReleaseActive)
            return;

        m_phase = JogPhase::TailSuppression;
        m_releaseRequestPublished = false;
    }

    void expireTailSuppression(double timestampSeconds) noexcept
    {
        if (m_phase != JogPhase::TailSuppression || !std::isfinite(timestampSeconds))
            return;

        if (timestampSeconds - m_tailLastMotionSeconds > kJogTailSuppressionSeconds)
            m_phase = JogPhase::Idle;
    }

    void noteSuppressedMotion(const JogInput& input) noexcept
    {
        if (input.ticks != 0.0 && std::isfinite(input.timestampSeconds))
            m_tailLastMotionSeconds = input.timestampSeconds;
    }

    [[nodiscard]] JogRouteResult handleTouchDown(const JogInput& input) noexcept
    {
        if (m_phase == JogPhase::TouchTracking)
            return result(JogRouteAction::Ignore, input);

        m_phase = JogPhase::TouchTracking;
        m_releaseRequestPublished = false;
        m_tailLastMotionSeconds = input.timestampSeconds;
        m_speed.reset(input.timestampSeconds);
        return result(JogRouteAction::BeginScratch, input, 0.0);
    }

    [[nodiscard]] JogRouteResult handleTouchUp(const JogInput& input) noexcept
    {
        if (m_phase != JogPhase::TouchTracking)
            return result(JogRouteAction::Ignore, input);

        const double releaseRate = m_speed.rate(input.timestampSeconds);
        m_phase = JogPhase::ReleaseOwned;
        m_releaseRequestPublished = true;
        return result(JogRouteAction::RequestRelease, input, releaseRate);
    }

    [[nodiscard]] JogRouteResult handlePlatter(const JogInput& input) noexcept
    {
        if (m_phase == JogPhase::TailSuppression) {
            noteSuppressedMotion(input);
            return result(JogRouteAction::Ignore, input);
        }

        if (m_phase != JogPhase::TouchTracking || input.ticks == 0.0)
            return result(JogRouteAction::Ignore, input);

        const double rate = m_speed.push(input.ticks, input.timestampSeconds);
        noteSuppressedMotion(input);
        return result(JogRouteAction::ScratchDelta,
                      input,
                      rate,
                      m_speed.lastEventIntervalSeconds());
    }

    [[nodiscard]] JogRouteResult handleRim(const JogInput& input) noexcept
    {
        if (m_phase == JogPhase::TailSuppression) {
            noteSuppressedMotion(input);
            return result(JogRouteAction::Ignore, input);
        }

        if (input.ticks == 0.0 || m_phase == JogPhase::TouchTracking)
            return result(JogRouteAction::Ignore, input);

        if (m_phase == JogPhase::ReleaseOwned) {
            const double rate = m_speed.push(input.ticks, input.timestampSeconds);
            noteSuppressedMotion(input);
            return result(JogRouteAction::ReleaseDelta,
                          input,
                          rate,
                          m_speed.lastEventIntervalSeconds());
        }

        return result(JogRouteAction::Nudge, input);
    }

    [[nodiscard]] JogRouteResult handleGeneric(const JogInput& input) noexcept
    {
        if (m_phase == JogPhase::TailSuppression) {
            noteSuppressedMotion(input);
            return result(JogRouteAction::Ignore, input);
        }

        if (input.ticks == 0.0)
            return result(JogRouteAction::Ignore, input);

        if (m_phase == JogPhase::TouchTracking) {
            const double rate = m_speed.push(input.ticks, input.timestampSeconds);
            noteSuppressedMotion(input);
            return result(JogRouteAction::ScratchDelta,
                          input,
                          rate,
                          m_speed.lastEventIntervalSeconds());
        }

        if (m_phase == JogPhase::ReleaseOwned) {
            const double rate = m_speed.push(input.ticks, input.timestampSeconds);
            noteSuppressedMotion(input);
            return result(JogRouteAction::ReleaseDelta,
                          input,
                          rate,
                          m_speed.lastEventIntervalSeconds());
        }

        return result(JogRouteAction::Nudge, input);
    }

    JogSpeedEstimator m_speed;
    JogPhase m_phase = JogPhase::Idle;
    double m_tailLastMotionSeconds = 0.0;
    bool m_releaseRequestPublished = false;
};

} // namespace flx10
