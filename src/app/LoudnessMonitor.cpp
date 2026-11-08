#include "app/LoudnessMonitor.h"

#include "app/SettingsManager.h"
#include "audio/AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr auto kTargetKey = "loudnessTargetLufs";
}

LoudnessMonitor::LoudnessMonitor(QObject* parent)
    : QObject(parent)
    , m_source([](const std::function<void(float)>& consumer) {
        return AudioEngine::drainLoudnessBlocks(consumer);
    })
{
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &LoudnessMonitor::poll);
    m_timer.start();
}

void LoudnessMonitor::attachSettingsManager(SettingsManager* settings)
{
    m_settings = settings;
    if (!m_settings)
        return;
    bool ok = false;
    const double stored = m_settings->getUiState(QString::fromLatin1(kTargetKey)).toDouble(&ok);
    if (ok)
        setTargetLufs(stored);
}

void LoudnessMonitor::poll()
{
    if (!m_source)
        return;
    const int blocks = m_source([this](float energy) { m_accumulator.addBlock(energy); });
    if (blocks > 0)
        publish();
}

void LoudnessMonitor::publish()
{
    const auto moved = [](double a, double b) { return std::abs(a - b) >= 0.05; };
    const double momentary = m_accumulator.momentaryLufs();
    const double shortTerm = m_accumulator.shortTermLufs();
    const double integrated = m_accumulator.integratedLufs();
    const double maxShortTerm = m_accumulator.maxShortTermLufs();
    if (!moved(momentary, m_momentary) && !moved(shortTerm, m_shortTerm)
        && !moved(integrated, m_integrated) && !moved(maxShortTerm, m_maxShortTerm))
        return;
    m_momentary = momentary;
    m_shortTerm = shortTerm;
    m_integrated = integrated;
    m_maxShortTerm = maxShortTerm;
    emit changed();
}

void LoudnessMonitor::setTargetLufs(double value)
{
    value = std::clamp(value, -30.0, 0.0);
    if (std::abs(value - m_targetLufs) < 1e-9)
        return;
    m_targetLufs = value;
    if (m_settings)
        m_settings->setUiState(QString::fromLatin1(kTargetKey), QString::number(value, 'f', 1));
    emit targetLufsChanged();
}

void LoudnessMonitor::reset()
{
    m_accumulator.reset();
    m_momentary = m_shortTerm = m_integrated = m_maxShortTerm = brockdj::audio::kLufsFloor;
    emit changed();
}
