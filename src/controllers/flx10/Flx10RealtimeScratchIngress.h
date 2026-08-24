#pragma once

#include "controllers/flx10/Flx10JogRouter.h"
#include "deck/scratch/RealtimeScratchInput.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>

namespace flx10 {

enum class RealtimeIngressResult {
    Ignored,
    Accepted,
    MirroredDuplicate
};

// Controller-thread front end for one physical FLX10 platter. It mirrors the
// same one-to-one tick calibration used by the UI route, but publishes directly
// into a lock-free audio snapshot so UI work cannot batch the hand trajectory.
class Flx10RealtimeScratchIngress final {
public:
    Flx10RealtimeScratchIngress()
        : m_stream(std::make_shared<engine::scratch::RealtimeScratchInput>())
    {
    }

    [[nodiscard]] std::shared_ptr<engine::scratch::RealtimeScratchInput> stream() const
    {
        return m_stream;
    }

    RealtimeIngressResult touchDown(double timestampSeconds,
                                    std::uint64_t sourceId = 0) noexcept
    {
        const std::lock_guard lock(m_mutex);
        if (mirroredDuplicate(EventKind::TouchDown, 0.0, timestampSeconds, sourceId))
            return RealtimeIngressResult::MirroredDuplicate;
        if (m_touching)
            return RealtimeIngressResult::Ignored;

        m_touching = true;
        m_released = false;
        m_speed.reset(timestampSeconds);
        m_stream->beginTouch(timestampSeconds);
        return RealtimeIngressResult::Accepted;
    }

    RealtimeIngressResult touchUp(double timestampSeconds,
                                  std::uint64_t sourceId = 0) noexcept
    {
        const std::lock_guard lock(m_mutex);
        if (mirroredDuplicate(EventKind::TouchUp, 0.0, timestampSeconds, sourceId))
            return RealtimeIngressResult::MirroredDuplicate;
        if (!m_touching)
            return RealtimeIngressResult::Ignored;

        const double releaseRate = m_speed.rate(timestampSeconds);
        m_touching = false;
        m_released = true;
        m_stream->endTouch(releaseRate, timestampSeconds);
        return RealtimeIngressResult::Accepted;
    }

    RealtimeIngressResult platter(double ticks,
                                  double timestampSeconds,
                                  std::uint64_t sourceId = 0) noexcept
    {
        if (!std::isfinite(ticks) || ticks == 0.0)
            return RealtimeIngressResult::Ignored;

        const std::lock_guard lock(m_mutex);
        if (mirroredDuplicate(EventKind::Platter, ticks, timestampSeconds, sourceId))
            return RealtimeIngressResult::MirroredDuplicate;
        if (!m_touching)
            return RealtimeIngressResult::Ignored;

        const double rate = m_speed.push(ticks, timestampSeconds);
        m_stream->publishTouchMotion(scratchDeltaSeconds(ticks),
                                     rate,
                                     m_speed.lastEventIntervalSeconds(),
                                     timestampSeconds);
        return RealtimeIngressResult::Accepted;
    }

    RealtimeIngressResult rim(double ticks,
                              double timestampSeconds,
                              std::uint64_t sourceId = 0) noexcept
    {
        if (!std::isfinite(ticks) || ticks == 0.0)
            return RealtimeIngressResult::Ignored;

        const std::lock_guard lock(m_mutex);
        if (mirroredDuplicate(EventKind::Rim, ticks, timestampSeconds, sourceId))
            return RealtimeIngressResult::MirroredDuplicate;
        if (m_touching || !m_released)
            return RealtimeIngressResult::Ignored;

        const double rate = m_speed.push(ticks, timestampSeconds);
        m_stream->publishReleaseVelocity(rate,
                                         m_speed.lastEventIntervalSeconds(),
                                         timestampSeconds);
        return RealtimeIngressResult::Accepted;
    }

    void reset(double timestampSeconds = 0.0) noexcept
    {
        const std::lock_guard lock(m_mutex);
        m_touching = false;
        m_released = false;
        m_speed.reset(timestampSeconds);
        m_lastEvents = {};
        m_stream->reset(timestampSeconds);
    }

private:
    enum class EventKind : std::size_t {
        TouchDown,
        TouchUp,
        Platter,
        Rim,
        Count
    };

    struct LastWireEvent {
        double value = 0.0;
        double timestampSeconds = 0.0;
        std::uint64_t sourceId = 0;
        bool valid = false;
    };

    [[nodiscard]] bool mirroredDuplicate(EventKind kind,
                                         double value,
                                         double timestampSeconds,
                                         std::uint64_t sourceId) noexcept
    {
        constexpr double kMirrorWindowSeconds = 0.004;
        auto& previous = m_lastEvents[static_cast<std::size_t>(kind)];
        if (previous.valid
            && sourceId != 0
            && previous.sourceId != 0
            && sourceId != previous.sourceId
            && previous.value == value
            && timestampSeconds >= previous.timestampSeconds
            && timestampSeconds - previous.timestampSeconds < kMirrorWindowSeconds) {
            return true;
        }

        previous = {value, timestampSeconds, sourceId, true};
        return false;
    }

    mutable std::mutex m_mutex;
    JogSpeedEstimator m_speed;
    std::shared_ptr<engine::scratch::RealtimeScratchInput> m_stream;
    std::array<LastWireEvent, static_cast<std::size_t>(EventKind::Count)> m_lastEvents {};
    bool m_touching = false;
    bool m_released = false;
};

} // namespace flx10
