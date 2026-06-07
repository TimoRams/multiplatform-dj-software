#include "DjEngine.h"
#include "audio/ReverseStreamAudioSource.h"

void DjEngine::setReverse(bool on)
{
    if (m_isReverse == on) return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_isReverse = on;
    if (reverseWrapSource) {
        reverseWrapSource->setReverse(on);
        if (m_loopActive)
            applyLoopRangeToAudioSource();
    }
    updateSpeedAndPitch();
    if (!on && wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit reverseChanged();
}


void DjEngine::setSlip(bool on)
{
    if (m_slipActive == on) return;
    m_slipActive = on;
    if (on)
        m_slipPosition = transportSource.getCurrentPosition();
    emit slipChanged();
}


void DjEngine::returnToSlipPosition()
{
    const double dur = std::max(0.001, static_cast<double>(getDuration()));
    const double pos = std::clamp(m_slipPosition, 0.0, dur);
    transportSource.setPosition(pos);
    m_snapPosition = pos;
    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(pos, std::memory_order_relaxed);
}
