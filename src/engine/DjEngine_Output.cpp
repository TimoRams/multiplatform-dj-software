#include "DjEngine.h"
#include "DjMasterBus.h"

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
