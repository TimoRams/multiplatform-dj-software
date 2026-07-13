#include "DjEngineCommonIncludes.h"

void DjEngine::setCueEnabled(bool value)
{
    const bool prev = m_cueEnabled.exchange(value, std::memory_order_relaxed);
    if (prev != value)
        emit cueEnabledChanged();
}


bool DjEngine::masterCueEnabled() const
{
    return DjMasterBus::masterCueEnabled();
}


double DjEngine::headphoneMix() const
{
    return static_cast<double>(DjMasterBus::headphoneMix());
}


void DjEngine::setMasterCueEnabled(bool value)
{
    const bool prev = DjMasterBus::masterCueEnabled();
    DjMasterBus::setMasterCueEnabled(value);
    if (prev != value)
        emit masterCueEnabledChanged();
}


void DjEngine::setHeadphoneMix(double value)
{
    const float clamped = static_cast<float>(std::clamp(value, 0.0, 1.0));
    const float prev = static_cast<float>(DjMasterBus::headphoneMix());
    DjMasterBus::setHeadphoneMix(clamped);
    if (std::abs(prev - clamped) > 0.0001f)
        emit headphoneMixChanged();
}


void DjEngine::setQuantizeEnabled(bool enabled)
{
    if (m_quantizeEnabled == enabled)
        return;
    m_quantizeEnabled = enabled;
    emit quantizeEnabledChanged();
}


void DjEngine::setReverse(bool on)
{
    if (m_isReverse == on) return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_isReverse = on;
    if (m_audioGraph->playback()) {
        m_audioGraph->playback()->setReverse(on);
        if (m_cueLoopController.activeLoop().active)
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
        m_slipPosition = m_audioGraph->transport().getCurrentPosition();
    emit slipChanged();
}


void DjEngine::returnToSlipPosition()
{
    const double dur = std::max(0.001, static_cast<double>(getDuration()));
    const double pos = std::clamp(m_slipPosition, 0.0, dur);
    m_audioGraph->transport().setPosition(pos);
    m_snapPosition = pos;
    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(pos, std::memory_order_relaxed);
}
