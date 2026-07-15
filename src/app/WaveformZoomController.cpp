#include "WaveformZoomController.h"
#include "SettingsManager.h"

namespace {
constexpr auto kSettingKey = "waveformZoom";
}

WaveformZoomController::WaveformZoomController(SettingsManager* settings, QObject* parent)
    : QObject(parent), m_settings(settings)
{
    if (m_settings) {
        bool ok = false;
        const double stored = m_settings->getUiState(QString::fromLatin1(kSettingKey), QString::number(kDefault)).toDouble(&ok);
        m_zoom = ok ? validatedZoom(stored) : kDefault;
    }
}

void WaveformZoomController::setZoom(double value)
{
    const double next = validatedZoom(value);
    if (qFuzzyCompare(m_zoom, next))
        return;
    m_zoom = next;
    persist();
    emit zoomChanged();
}

void WaveformZoomController::zoomIn()
{
    setZoom(increasedZoom(m_zoom));
}

void WaveformZoomController::zoomOut()
{
    setZoom(decreasedZoom(m_zoom));
}

void WaveformZoomController::reset()
{
    setZoom(kDefault);
}

void WaveformZoomController::persist()
{
    if (m_settings)
        m_settings->setUiState(QString::fromLatin1(kSettingKey), QString::number(m_zoom, 'g', 12));
}
