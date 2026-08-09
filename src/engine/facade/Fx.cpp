#include "../DjEngine.h"

#include "audio/DeckAudioPipeline.h"
#include "audio/DeckChannelProcessor.h"

#include <QHash>


void DjEngine::setFxEffectType(EffectType type)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxEffectType(type);
}


void DjEngine::setFxWetDry(float amount)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxAmount(amount);
}


void DjEngine::setFxExternalDelayTime(float seconds)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxExternalDelayTime(seconds);
}


void DjEngine::setFxPrimaryParam(float v)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxPrimaryParam(v);
}


void DjEngine::setFxSlotEffectType(int slot, EffectType type)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxSlotEffectType(slot, type);
}


void DjEngine::setFxSlotWetDry(int slot, float amount)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxSlotAmount(slot, amount);
}


void DjEngine::setFxSlotExternalDelayTime(int slot, float seconds)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxSlotExternalDelayTime(slot, seconds);
}


void DjEngine::setFxSlotPrimaryParam(int slot, float v)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxSlotPrimaryParam(slot, v);
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
    if (m_audioPipeline->mixerPtr()) {
        m_audioPipeline->mixer().setPadFxEffectType(type);
        m_audioPipeline->mixer().setPadFxAmount(wet);
    }
}


void DjEngine::clearPadFx()
{
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().clearPadFx();
}


void DjEngine::activateStopEffect(StopEffect effect)
{
    switch (effect) {
    case StopEffect::VinylBrake:
        if (m_vinylBrakeActive)
            return;
        m_vinylBrakeActive = true;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setVinylBrakeActive(true);
        emit vinylBrakeChanged();
        break;
    case StopEffect::EchoOut:
        if (m_echoOutActive)
            return;
        m_echoOutActive = true;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setEchoOutActive(true);
        emit echoOutChanged();
        break;
    case StopEffect::Backspin:
        if (m_backspinActive)
            return;
        m_backspinActive = true;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setBackspinActive(true);
        emit backspinChanged();
        break;
    case StopEffect::RollOut:
        if (m_rollOutActive)
            return;
        m_rollOutActive = true;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setRollOutActive(true);
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
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setVinylBrakeActive(false);
        emit vinylBrakeChanged();
        break;
    case StopEffect::EchoOut:
        if (!m_echoOutActive)
            return;
        m_echoOutActive = false;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setEchoOutActive(false);
        emit echoOutChanged();
        break;
    case StopEffect::Backspin:
        if (!m_backspinActive)
            return;
        m_backspinActive = false;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setBackspinActive(false);
        emit backspinChanged();
        break;
    case StopEffect::RollOut:
        if (!m_rollOutActive)
            return;
        m_rollOutActive = false;
        if (m_audioPipeline->mixerPtr())
            m_audioPipeline->mixer().setRollOutActive(false);
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
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxSCKnob(knob);
}


void DjEngine::setFxSCParam(float param)
{
    if (m_audioPipeline->mixerPtr()) m_audioPipeline->mixer().setFxSCParam(param);
}
