#include "ScratchSession.hpp"

#include "dsp/ScratchDeckBridge.hpp"

#include <algorithm>

namespace engine::scratch {

void ScratchSession::clear() noexcept
{
    m_scrubbing = false;
    m_releaseGlide = false;
    m_wasPlaying = false;
    m_loopLocked = false;
    m_savedReverse = false;
    m_lastRawSec = 0.0;
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

bool ScratchSession::submitRelative(engine::audio::ScratchDeckBridge* bridge,
                                    double deltaSec,
                                    double sampleRate) noexcept
{
    if (!bridge || deltaSec == 0.0)
        return false;

    const double clamped = std::clamp(deltaSec, -kEventSpikeClampSec, kEventSpikeClampSec);

    // Floor dt at ~one UI frame to avoid velocity spikes from sub-millisecond events.
    const double dtSec = m_lastMoveClock.isValid()
        ? std::clamp(static_cast<double>(m_lastMoveClock.nsecsElapsed()) * 1e-9, 0.008, 0.120)
        : 0.016;
    m_lastMoveClock.restart();

    bridge->submitHandDeltaSeconds(clamped, dtSec);
    m_lastRawSec += clamped;
    // Audio position is owned by the tracker on the audio thread (driven by the
    // platter target updated in submitHandDeltaSeconds). Only publish the display.
    bridge->publishScratchDisplay(m_lastRawSec);
    (void) sampleRate;
    return true;
}

bool ScratchSession::submitAbsolute(engine::audio::ScratchDeckBridge* bridge,
                                    double posSec,
                                    double sampleRate,
                                    double trackLenSec,
                                    double scratchPreRollSec,
                                    const ScratchLoopCtx& loop) noexcept
{
    if (!bridge || !m_scrubbing || trackLenSec <= 0.0)
        return false;

    double target = std::clamp(posSec, -scratchPreRollSec, trackLenSec);
    target = wrapLoopPosition(target, trackLenSec, loop, m_loopLocked);

    const double virtualDelta = target - m_lastRawSec;
    if (std::abs(virtualDelta) <= 1e-9)
        return false;

    return submitRelative(bridge, virtualDelta, sampleRate);
}

double ScratchSession::tick(engine::audio::ScratchDeckBridge* bridge, double dtSec) noexcept
{
    (void) dtSec;
    return bridge ? bridge->scratchRate() : 0.0;
}

} // namespace engine::scratch
