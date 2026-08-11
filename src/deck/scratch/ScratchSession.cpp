#include "ScratchSession.h"

#include "audio/RenderModeRouter.h"

#include <algorithm>

namespace engine::scratch {

void ScratchSession::clear() noexcept
{
    m_phase = ScratchPhase::Idle;
    m_wasPlaying = false;
    m_loopLocked = false;
    m_savedReverse = false;
    m_lastRawSec = 0.0;
}

void ScratchSession::setScrubbing(bool value) noexcept
{
    if (value) {
        m_phase = ScratchPhase::TouchTracking;
    } else if (m_phase == ScratchPhase::TouchTracking) {
        m_phase = ScratchPhase::Idle;
    }
}

void ScratchSession::setReleaseGlide(bool value) noexcept
{
    if (value) {
        if (m_phase == ScratchPhase::Idle || m_phase == ScratchPhase::TouchTracking)
            m_phase = ScratchPhase::ReleasePending;
    } else if (releaseGlide()) {
        m_phase = ScratchPhase::Idle;
    }
}

double ScratchSession::wrapLoopPosition(double posSec,
                                        double trackLenSec,
                                        const ScratchLoopCtx& loop,
                                        bool& loopLocked) noexcept
{
    if (!loop.active || loop.outSec <= loop.inSec || trackLenSec <= 0.0)
        return posSec;

    const double lo = loop.inSec;
    const double hi = std::min(trackLenSec, loop.outSec);
    const double loopLen = hi - lo;
    if (loopLen <= 0.0)
        return posSec;

    if (!loopLocked && posSec >= lo)
        loopLocked = true;

    if (!loopLocked || (posSec >= lo && posSec < hi))
        return posSec;

    const double offset = posSec - lo;
    return lo + std::fmod(std::fmod(offset, loopLen) + loopLen, loopLen);
}

double ScratchSession::armGrab(double grabSec, double trackLenSec, const ScratchLoopCtx& loop) noexcept
{
    m_loopLocked = false;
    m_lastRawSec = wrapLoopPosition(grabSec, trackLenSec, loop, m_loopLocked);
    m_physicsClock.restart();
    m_lastMoveClock.restart();
    return m_lastRawSec;
}

bool ScratchSession::submitRelative(engine::audio::RenderModeRouter* bridge,
                                    double deltaSec,
                                    double sampleRate) noexcept
{
    // Preserve high-rate controller timing. Hardware-specific filtering happens
    // before this compatibility delta path, not by stretching fast events.
    const double dtSec = m_lastMoveClock.isValid()
        ? std::clamp(static_cast<double>(m_lastMoveClock.nsecsElapsed()) * 1e-9, 1e-6, 0.120)
        : 0.016;
    return submitRelativeAtInterval(bridge, deltaSec, sampleRate, dtSec);
}

bool ScratchSession::submitRelativeAtInterval(engine::audio::RenderModeRouter* bridge,
                                              double deltaSec,
                                              double sampleRate,
                                              double eventIntervalSeconds) noexcept
{
    if (!bridge || deltaSec == 0.0)
        return false;

    const double clamped = std::clamp(deltaSec, -kEventSpikeClampSec, kEventSpikeClampSec);
    const double dtSec = std::clamp(
        std::isfinite(eventIntervalSeconds) ? eventIntervalSeconds : 0.016,
        1e-6,
        0.120);
    m_lastMoveClock.restart();

    bridge->submitHandDeltaSeconds(clamped, dtSec);
    m_lastRawSec += clamped;
    // Audio position is owned by the tracker on the audio thread (driven by the
    // platter target updated in submitHandDeltaSeconds). Only publish the display.
    bridge->publishScratchDisplay(m_lastRawSec);
    (void) sampleRate;
    return true;
}

bool ScratchSession::submitReleaseRelative(engine::audio::RenderModeRouter* bridge,
                                           double deltaSec) noexcept
{
    if (!bridge || deltaSec == 0.0)
        return false;

    const double clamped = std::clamp(deltaSec, -kEventSpikeClampSec, kEventSpikeClampSec);
    const double dtSec = m_lastMoveClock.isValid()
        ? std::clamp(static_cast<double>(m_lastMoveClock.nsecsElapsed()) * 1e-9, 1e-6, 0.120)
        : 0.016;
    m_lastMoveClock.restart();
    bridge->submitReleaseDeltaSeconds(clamped, dtSec);
    return true;
}

bool ScratchSession::submitAbsolute(engine::audio::RenderModeRouter* bridge,
                                    double posSec,
                                    double sampleRate,
                                    double trackLenSec,
                                    double scratchPreRollSec,
                                    const ScratchLoopCtx& loop) noexcept
{
    if (!bridge || !scrubbing() || trackLenSec <= 0.0)
        return false;

    double target = std::clamp(posSec, -scratchPreRollSec, trackLenSec);
    target = wrapLoopPosition(target, trackLenSec, loop, m_loopLocked);

    const double virtualDelta = target - m_lastRawSec;
    if (std::abs(virtualDelta) <= 1e-9)
        return false;

    return submitRelative(bridge, virtualDelta, sampleRate);
}

double ScratchSession::tick(engine::audio::RenderModeRouter* bridge, double dtSec) noexcept
{
    (void) dtSec;
    return bridge ? bridge->scratchRate() : 0.0;
}

} // namespace engine::scratch
