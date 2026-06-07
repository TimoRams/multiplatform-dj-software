#include "DjEngine.h"
#include "DjMasterBus.h"
#include "audio/ReverseStreamAudioSource.h"
#include "audio/AudioDeviceUtils.h"
#include "audio/MetadataUtils.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "fx/FxProcessor.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "WaveformCache.h"
#include "WaveformAnalyzer.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QVariantMap>
#include <QImage>
#include <QBuffer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <ranges>
#include <vector>
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

namespace {

constexpr double kVolumeMin = 0.0;
constexpr double kVolumeMax = 1.0;
constexpr double kTrimMin = 0.0;
constexpr double kTrimMax = 2.0;
constexpr double kEqMin = -1.0;
constexpr double kEqMax = 1.0;
constexpr double kFilterMin = -1.0;
constexpr double kFilterMax = 1.0;

double playHistoryThresholdSeconds(double durationSec)
{
    if (durationSec <= 0.0)
        return 12.0;

    if (durationSec <= 45.0)
        return std::clamp(durationSec * 0.35, 5.0, 12.0);

    return std::clamp(durationSec * 0.12, 10.0, 20.0);
}

QString defaultHotCueColor(int index)
{
    static const char* kColors[] = {
        "#e04040", "#e08030", "#e0c030", "#40c040",
        "#3080e0", "#8040e0", "#e040a0", "#40c0c0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

QString defaultSavedLoopColor(int index)
{
    static const char* kColors[] = {
        "#30b050", "#3080e0", "#e08030", "#8040e0",
        "#e04040", "#40c0c0", "#e0c030", "#e040a0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

} // namespace

void DjEngine::setFxEffectType(EffectType type)
{
    if (mixerSource) mixerSource->setFxEffectType(type);
}


void DjEngine::setFxWetDry(float amount)
{
    if (mixerSource) mixerSource->setFxAmount(amount);
}


void DjEngine::setFxExternalDelayTime(float seconds)
{
    if (mixerSource) mixerSource->setFxExternalDelayTime(seconds);
}


void DjEngine::setFxPrimaryParam(float v)
{
    if (mixerSource) mixerSource->setFxPrimaryParam(v);
}


void DjEngine::setFxSlotEffectType(int slot, EffectType type)
{
    if (mixerSource) mixerSource->setFxSlotEffectType(slot, type);
}


void DjEngine::setFxSlotWetDry(int slot, float amount)
{
    if (mixerSource) mixerSource->setFxSlotAmount(slot, amount);
}


void DjEngine::setFxSlotExternalDelayTime(int slot, float seconds)
{
    if (mixerSource) mixerSource->setFxSlotExternalDelayTime(slot, seconds);
}


void DjEngine::setFxSlotPrimaryParam(int slot, float v)
{
    if (mixerSource) mixerSource->setFxSlotPrimaryParam(slot, v);
}


void DjEngine::setPadFx(const QString& effectName, float wet)
{
    static const QHash<QString, EffectType> kMap = {
        {"Echo",       EffectType::Echo},
        {"Reverb",     EffectType::Reverb},
        {"Roll",       EffectType::Roll},
        {"SlipRoll",   EffectType::SlipRoll},
        {"Flanger",    EffectType::Flanger},
        {"Phaser",     EffectType::Phaser},
        {"Bitcrusher", EffectType::Bitcrusher},
        {"Trans",      EffectType::Trans},
        {"Stretch",    EffectType::Stretch},
        {"Filter",     EffectType::SoundColorFilter},
        {"RollOut",    EffectType::RollOut},
    };
    const EffectType type = kMap.value(effectName, EffectType::None);
    if (mixerSource) {
        mixerSource->setPadFxEffectType(type);
        mixerSource->setPadFxAmount(wet);
    }
}


void DjEngine::clearPadFx()
{
    if (mixerSource)
        mixerSource->clearPadFx();
}


void DjEngine::activateStopEffect(StopEffect effect)
{
    switch (effect) {
    case StopEffect::VinylBrake:
        if (m_vinylBrakeActive)
            return;
        m_vinylBrakeActive = true;
        if (mixerSource)
            mixerSource->setVinylBrakeActive(true);
        emit vinylBrakeChanged();
        break;
    case StopEffect::EchoOut:
        if (m_echoOutActive)
            return;
        m_echoOutActive = true;
        if (mixerSource)
            mixerSource->setEchoOutActive(true);
        emit echoOutChanged();
        break;
    case StopEffect::Backspin:
        if (m_backspinActive)
            return;
        m_backspinActive = true;
        if (mixerSource)
            mixerSource->setBackspinActive(true);
        emit backspinChanged();
        break;
    case StopEffect::RollOut:
        if (m_rollOutActive)
            return;
        m_rollOutActive = true;
        if (mixerSource)
            mixerSource->setRollOutActive(true);
        emit rollOutChanged();
        break;
    }
}


void DjEngine::deactivateStopEffect(StopEffect effect)
{
    switch (effect) {
    case StopEffect::VinylBrake:
        if (!m_vinylBrakeActive)
            return;
        m_vinylBrakeActive = false;
        if (mixerSource)
            mixerSource->setVinylBrakeActive(false);
        emit vinylBrakeChanged();
        break;
    case StopEffect::EchoOut:
        if (!m_echoOutActive)
            return;
        m_echoOutActive = false;
        if (mixerSource)
            mixerSource->setEchoOutActive(false);
        emit echoOutChanged();
        break;
    case StopEffect::Backspin:
        if (!m_backspinActive)
            return;
        m_backspinActive = false;
        if (mixerSource)
            mixerSource->setBackspinActive(false);
        emit backspinChanged();
        break;
    case StopEffect::RollOut:
        if (!m_rollOutActive)
            return;
        m_rollOutActive = false;
        if (mixerSource)
            mixerSource->setRollOutActive(false);
        emit rollOutChanged();
        break;
    }
}


void DjEngine::startVinylBrake()
{
    activateStopEffect(StopEffect::VinylBrake);
}


void DjEngine::stopVinylBrake()
{
    deactivateStopEffect(StopEffect::VinylBrake);
}


void DjEngine::startEchoOut()
{
    activateStopEffect(StopEffect::EchoOut);
}


void DjEngine::stopEchoOut()
{
    deactivateStopEffect(StopEffect::EchoOut);
}


void DjEngine::startBackspin()
{
    activateStopEffect(StopEffect::Backspin);
}


void DjEngine::stopBackspin()
{
    deactivateStopEffect(StopEffect::Backspin);
}


void DjEngine::startRollOut()
{
    activateStopEffect(StopEffect::RollOut);
}


void DjEngine::stopRollOut()
{
    deactivateStopEffect(StopEffect::RollOut);
}


void DjEngine::setFxSCKnob(float knob)
{
    if (mixerSource) mixerSource->setFxSCKnob(knob);
}


void DjEngine::setFxSCParam(float param)
{
    if (mixerSource) mixerSource->setFxSCParam(param);
}

