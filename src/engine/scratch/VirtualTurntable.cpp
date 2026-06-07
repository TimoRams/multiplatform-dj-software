#include "VirtualTurntable.hpp"

#include <algorithm>

namespace engine::scratch {

void VirtualTurntable::reset(double startSamplePos, double trackSampleRate) noexcept
{
    m_trackSampleRate = std::max(1.0, trackSampleRate);
    m_targetSamplePos.store(startSamplePos, std::memory_order_relaxed);
    m_displayAngleRad = 0.0;
}

void VirtualTurntable::addAngleDeltaRadians(double deltaRadians) noexcept
{
    if (std::abs(deltaRadians) < 1e-12)
        return;

    m_displayAngleRad += deltaRadians;
    const double spr = samplesPerRadian(m_trackSampleRate);
    addTargetSampleDelta(deltaRadians * spr);
}

void VirtualTurntable::addAngleDeltaDegrees(double deltaDegrees) noexcept
{
    addAngleDeltaRadians(deltaDegrees * kPi / 180.0);
}

void VirtualTurntable::addTimeDeltaSeconds(double deltaSeconds) noexcept
{
    if (deltaSeconds == 0.0)
        return;
    addTargetSampleDelta(deltaSeconds * m_trackSampleRate);
}

void VirtualTurntable::addJogTicks(double ticks) noexcept
{
    if (ticks == 0.0 || m_samplesPerTick <= 0.0)
        return;
    addTargetSampleDelta(ticks * m_samplesPerTick);
}

void VirtualTurntable::setAbsoluteSamplePosition(double samplePos) noexcept
{
    m_targetSamplePos.store(samplePos, std::memory_order_relaxed);
}

void VirtualTurntable::setAbsoluteTimeSeconds(double seconds) noexcept
{
    setAbsoluteSamplePosition(seconds * m_trackSampleRate);
}

void VirtualTurntable::addTargetSampleDelta(double deltaSamples) noexcept
{
    const double next = m_targetSamplePos.load(std::memory_order_relaxed) + deltaSamples;
    m_targetSamplePos.store(next, std::memory_order_relaxed);
}

} // namespace engine::scratch
