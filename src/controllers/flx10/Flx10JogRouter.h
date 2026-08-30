#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace flx10 {

// The FLX10 relative CC stream yields roughly 12,750 semantic ticks per
// physical revolution. Keep this conversion beside the jog state machine that
// owns it; it is not part of the display/HID wire protocol.
constexpr double kScratchIntervalsPerRevolution = 12750.0;
// Holding BEAT JUMP turns the platter into a silent, coarse transport search.
// Thirty seconds per revolution is deliberately much faster than vinyl motion,
// while retaining enough resolution for accurate positioning at slow speeds.
constexpr double kFastSearchSecondsPerRevolution = 30.0;
constexpr double kVinylRpm = 33.0 + 1.0 / 3.0;
// The rate window is primarily sized in ticks. A purely fixed-duration window
// resolves a fast platter finely and a slow one hardly at all: at a crawl only
// one or two ticks land in it, so the estimate becomes the quotient of one tick
// by a USB-frame-quantised gap and swings by tens of percent between events.
// Holding the tick count constant makes the averaging time scale as 1/speed;
// the small minimum time below then prevents the high-speed end from shrinking
// below the controller/USB timing resolution.
constexpr double kJogSpeedWindowTicks = 8.0;
// At high platter speeds eight ticks span less than one USB frame. A quotient
// over that interval mostly measures scheduler/USB jitter rather than the hand.
// Keep at least a few milliseconds of same-direction history once available.
// Direction changes reset the estimator before this rule is applied, so the
// first reverse frame still changes sign immediately.
constexpr double kJogSpeedMinimumWindowSeconds = 0.0035;
constexpr double kJogSpeedWindowSeconds = 0.060;   // hard bound on that window
constexpr double kJogSpeedStaleSeconds = 0.060;
constexpr double kJogTailSuppressionSeconds = 0.120;
// ALSA/JUCE may drain several USB packets in one scheduler wake-up. Those
// packets receive timestamps only a few microseconds apart even though the
// wheel did not physically accelerate by two orders of magnitude. Treat a
// sub-frame cluster as one observation and retain all of its ticks.
constexpr double kJogTimestampCoalesceSeconds = 0.00035;

constexpr double scratchDeltaSeconds(double ticks) noexcept
{
    return ticks * (60.0 / kVinylRpm) / kScratchIntervalsPerRevolution;
}

constexpr double fastSearchDeltaSeconds(double ticks) noexcept
{
    return ticks * kFastSearchSecondsPerRevolution / kScratchIntervalsPerRevolution;
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
        m_cumulativeAbsTicks = 0.0;
        m_baseTicks = 0.0;
        m_baseAbsTicks = 0.0;
        m_baseTimestampSeconds = timestampSeconds;
        m_hasBase = std::isfinite(timestampSeconds);
        m_lastEventIntervalSeconds = 0.0;
        m_lastDirection = 0;
    }

    double push(double ticks, double timestampSeconds) noexcept
    {
        if (!std::isfinite(ticks) || ticks == 0.0 || !std::isfinite(timestampSeconds))
            return rate(timestampSeconds);

        if (!m_hasBase)
            reset(timestampSeconds);

        if (m_count > 0 && timestampSeconds < m_samples[m_count - 1].timestampSeconds)
            reset(timestampSeconds);

        const double newestTimestamp = m_count > 0
            ? m_samples[m_count - 1].timestampSeconds
            : m_baseTimestampSeconds;
        const bool restartedAfterStall =
            timestampSeconds - newestTimestamp > kJogSpeedStaleSeconds;
        if (restartedAfterStall)
            reset(timestampSeconds);

        const int direction = ticks > 0.0 ? 1 : -1;
        if (m_lastDirection != 0 && direction != m_lastDirection) {
            // Old-direction ticks must not cancel the first samples after a
            // reversal. Restart the velocity window at the preceding event;
            // absolute position remains authoritative in the separate stream.
            const double turnTimestamp = m_count > 0
                ? m_samples[m_count - 1].timestampSeconds
                : timestampSeconds;
            reset(turnTimestamp);
        }

        const bool coalesced = m_count > 0
            && timestampSeconds - m_samples[m_count - 1].timestampSeconds
                <= kJogTimestampCoalesceSeconds;

        m_lastEventIntervalSeconds = m_count > 0 && !coalesced
            ? timestampSeconds - m_samples[m_count - 1].timestampSeconds
            : 0.0;

        m_cumulativeTicks += ticks;
        m_cumulativeAbsTicks += std::abs(ticks);
        if (coalesced) {
            auto& newest = m_samples[m_count - 1];
            newest.timestampSeconds = std::max(newest.timestampSeconds, timestampSeconds);
            newest.cumulativeTicks = m_cumulativeTicks;
            newest.cumulativeAbsTicks = m_cumulativeAbsTicks;
        } else {
            if (m_count == kCapacity)
                popOldest();
            m_samples[m_count++] = {
                timestampSeconds, m_cumulativeTicks, m_cumulativeAbsTicks};
        }
        m_lastDirection = direction;
        if (restartedAfterStall) {
            // The first tick says motion resumed but carries no trustworthy
            // duration. Make it the new window origin rather than counting it
            // again over the interval to the second tick.
            m_baseTimestampSeconds = timestampSeconds;
            m_baseTicks = m_cumulativeTicks;
            m_baseAbsTicks = m_cumulativeAbsTicks;
        }

        // There is no usable timebase inside one drained USB frame. This also
        // covers the first packet at touch-down and the first packet after a
        // reversal: position is still applied exactly, but velocity stays at
        // the physical turn point instead of becoming an artificial +/-8x
        // command from dividing by a few scheduler microseconds. The next
        // distinct frame measures the complete movement normally.
        const double windowDuration = timestampSeconds - m_baseTimestampSeconds;
        if (windowDuration >= 0.0
            && windowDuration <= kJogTimestampCoalesceSeconds) {
            m_baseTimestampSeconds = timestampSeconds;
            m_baseTicks = m_cumulativeTicks;
            m_baseAbsTicks = m_cumulativeAbsTicks;
        }
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
        double cumulativeAbsTicks = 0.0;
    };

    // Drop the oldest sample; the window base moves onto it.
    void popOldest() noexcept
    {
        if (m_count == 0)
            return;
        m_baseTimestampSeconds = m_samples[0].timestampSeconds;
        m_baseTicks = m_samples[0].cumulativeTicks;
        m_baseAbsTicks = m_samples[0].cumulativeAbsTicks;
        for (std::size_t i = 1; i < m_count; ++i)
            m_samples[i - 1] = m_samples[i];
        --m_count;
    }

    void trimToWindow(double newestTimestampSeconds) noexcept
    {
        if (m_count == 0)
            return;

        // Shrink to the shortest window that still carries enough ticks and a
        // trustworthy amount of time to resolve the rate. Direction changes
        // reset the history in push(), so this window never straddles a turn.
        while (m_count > 1
               && m_samples[m_count - 1].cumulativeAbsTicks
                          - m_samples[0].cumulativeAbsTicks
                      >= kJogSpeedWindowTicks
               && m_samples[m_count - 1].timestampSeconds
                          - m_samples[0].timestampSeconds
                      > kJogSpeedMinimumWindowSeconds)
            popOldest();

        // Hard time bound, so a hand that has nearly stopped cannot keep
        // averaging over movement that happened long ago.
        while (m_count > 1
               && newestTimestampSeconds - m_baseTimestampSeconds > kJogSpeedWindowSeconds)
            popOldest();

        if (m_count == 1
            && newestTimestampSeconds - m_baseTimestampSeconds > kJogSpeedWindowSeconds) {
            m_baseTimestampSeconds = m_samples[0].timestampSeconds;
            m_baseTicks = m_samples[0].cumulativeTicks;
            m_baseAbsTicks = m_samples[0].cumulativeAbsTicks;
        }
    }

    std::array<Sample, kCapacity> m_samples {};
    std::size_t m_count = 0;
    double m_cumulativeTicks = 0.0;
    double m_cumulativeAbsTicks = 0.0;
    double m_baseTimestampSeconds = 0.0;
    double m_baseTicks = 0.0;
    double m_baseAbsTicks = 0.0;
    double m_lastEventIntervalSeconds = 0.0;
    bool m_hasBase = false;
    int m_lastDirection = 0;
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
