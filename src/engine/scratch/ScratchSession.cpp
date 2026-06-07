#include "ScratchSession.hpp"

#include "audio/ScratchDeckBridge.hpp"

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
    return m_lastRawSec;
}

bool ScratchSession::submitRelative(engine::audio::ScratchDeckBridge* bridge,
                                    double deltaSec,
                                    double sampleRate) noexcept
{
    if (!bridge || deltaSec == 0.0)
        return false;

    const double clamped = std::clamp(deltaSec, -kEventSpikeClampSec, kEventSpikeClampSec);
    bridge->addTargetDeltaSeconds(clamped, sampleRate);
    m_lastRawSec += clamped;
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

    const double virtualDelta = posSec - m_lastRawSec;
    if (std::abs(virtualDelta) <= 1e-9)
        return false;

    double target = std::clamp(posSec, -scratchPreRollSec, trackLenSec);
    target = wrapLoopPosition(target, trackLenSec, loop, m_loopLocked);

    m_lastRawSec = target;
    bridge->addTargetDeltaSeconds(virtualDelta, sampleRate);
    return true;
}

double ScratchSession::tick(engine::audio::ScratchDeckBridge* bridge, double dtSec) noexcept
{
    if (bridge)
        bridge->tickControlThread(dtSec);
    return bridge ? bridge->scratchRate() : 0.0;
}

} // namespace engine::scratch
