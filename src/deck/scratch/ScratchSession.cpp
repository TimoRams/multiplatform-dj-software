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
    // Use the actual interval between pointer events to estimate hand speed.
    const double dtSec = m_lastMoveClock.isValid()
        ? std::clamp(static_cast<double>(m_lastMoveClock.nsecsElapsed()) * 1e-9, 1e-6, 0.120)
        : 0.016;
    // Mouse movement can be coalesced by the window system. Its delta is the
    // complete distance since the previous event, so clipping one event loses
    // real drag distance (especially on Linux with sparse pointer events).
    return submitRelativeAtInterval(bridge, deltaSec, sampleRate, dtSec,
                                    std::numeric_limits<double>::quiet_NaN(), false);
}

bool ScratchSession::submitRelativeAtInterval(engine::audio::RenderModeRouter* bridge,
                                              double deltaSec,
                                              double sampleRate,
                                              double eventIntervalSeconds,
                                              double measuredRate,
                                              bool clampEventSpike) noexcept
{
    if (!bridge || deltaSec == 0.0)
        return false;

    // MIDI deltas still need spike protection; pointer deltas are measured
    // positions and must remain lossless even when events are coalesced.
    const double acceptedDelta = clampEventSpike
        ? std::clamp(deltaSec, -kEventSpikeClampSec, kEventSpikeClampSec)
        : deltaSec;
    const double dtSec = std::clamp(
        std::isfinite(eventIntervalSeconds) ? eventIntervalSeconds : 0.016,
        1e-6,
        0.120);
    m_lastMoveClock.restart();

    bridge->submitHandDeltaSeconds(acceptedDelta, dtSec, measuredRate);
    m_lastRawSec += acceptedDelta;
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
