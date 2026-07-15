#include "UiScaleController.h"
#include "SettingsManager.h"

#include <cmath>

namespace {
constexpr auto kSettingKey = "uiScale";
}

UiScaleController::UiScaleController(SettingsManager* settings, QObject* parent)
    : QObject(parent), m_settings(settings)
{
    if (m_settings) {
        bool ok = false;
        const double stored = m_settings->getUiState(QString::fromLatin1(kSettingKey), QStringLiteral("1.0")).toDouble(&ok);
        m_scale = ok ? validatedScale(stored) : 1.0;
    }
}

int UiScaleController::percent() const noexcept
{
    return static_cast<int>(std::lround(m_scale * 100.0));
}

void UiScaleController::setScale(double value)
{
    const double next = validatedScale(value);
    if (qFuzzyCompare(m_scale, next))
        return;
    m_scale = next;
    persist();
    emit scaleChanged();
}

void UiScaleController::increase()
{
    setScale(increasedScale(m_scale));
}

void UiScaleController::decrease()
{
    setScale(decreasedScale(m_scale));
}

void UiScaleController::reset()
{
    setScale(1.0);
}

void UiScaleController::persist()
{
    if (m_settings)
        m_settings->setUiState(QString::fromLatin1(kSettingKey), QString::number(m_scale, 'f', 2));
}
