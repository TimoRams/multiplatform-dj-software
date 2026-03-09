#include "FxManager.h"
#include "DjEngine.h"
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// FxManager implementation
// ─────────────────────────────────────────────────────────────────────────────

FxManager::FxManager(QObject* parent)
    : QObject(parent)
{
    qDebug() << "[FxManager] initialised";
}

void FxManager::registerEngines(DjEngine* deckA, DjEngine* deckB)
{
    m_engineA = deckA;
    m_engineB = deckB;
    qDebug() << "[FxManager] engines registered";
}

// ── Helpers ───────────────────────────────────────────────────────────────────

EffectType FxManager::effectTypeFromString(const QString& name)
{
    if (name == "Reverb")        return EffectType::Reverb;
    if (name == "Bitcrusher")    return EffectType::Bitcrusher;
    if (name == "Pitch Shifter" || name == "PitchShifter")
                                 return EffectType::PitchShifter;
    if (name == "Echo")          return EffectType::Echo;
    if (name == "Low Cut Echo")  return EffectType::LowCutEcho;
    if (name == "MT Delay")      return EffectType::MtDelay;
    if (name == "Spiral")        return EffectType::Spiral;
    if (name == "Flanger")       return EffectType::Flanger;
    if (name == "Phaser")        return EffectType::Phaser;
    if (name == "Trans")         return EffectType::Trans;
    if (name == "Enigma Jet")    return EffectType::EnigmaJet;
    if (name == "Stretch")       return EffectType::Stretch;
    if (name == "Slip Roll")     return EffectType::SlipRoll;
    if (name == "Roll")          return EffectType::Roll;
    if (name == "Nobius")        return EffectType::Nobius;
    if (name == "Mobius")        return EffectType::Mobius;
    // ── SoundColor mode aliases ──────────────────────────────────────────────
    if (name == "Space")         return EffectType::SoundColorSpace;
    if (name == "D.Echo")        return EffectType::SoundColorDubEcho;
    if (name == "Crush")         return EffectType::SoundColorCrush;
    if (name == "Pitch")         return EffectType::SoundColorPitch;
    if (name == "Noise")         return EffectType::SoundColorNoise;
    if (name == "Filter")        return EffectType::SoundColorFilter;
    return EffectType::None;
}

void FxManager::routeToEngines(int unitId, EffectType type, float wetDry)
{
    // Unit 1 routes to DeckA when deck1A==true, optionally also DeckB when deck1B==true.
    // Unit 2 routes to DeckB when deck2B==true, optionally also DeckA when deck2A==true.
    if (unitId == 1) {
        if (m_deck1A && m_engineA) { m_engineA->setFxEffectType(type); m_engineA->setFxWetDry(wetDry); }
        if (m_deck1B && m_engineB) { m_engineB->setFxEffectType(type); m_engineB->setFxWetDry(wetDry); }
    } else {
        if (m_deck2A && m_engineA) { m_engineA->setFxEffectType(type); m_engineA->setFxWetDry(wetDry); }
        if (m_deck2B && m_engineB) { m_engineB->setFxEffectType(type); m_engineB->setFxWetDry(wetDry); }
    }
}

// ── QML-callable dispatch ─────────────────────────────────────────────────────

void FxManager::setEffectType(int unitId, const QString& type)
{
    if (unitId == 1) setEffectType1(type);
    else             setEffectType2(type);
}

void FxManager::setWetDry(int unitId, float amount)
{
    if (unitId == 1) setWetDry1(amount);
    else             setWetDry2(amount);
}

void FxManager::setDeckAssignment(int unitId, int deck, bool active)
{
    qDebug() << "[FxManager] FX" << unitId << "deck" << deck
             << (active ? "ASSIGNED" : "REMOVED");

    if (unitId == 1) {
        if (deck == 1) setDeck1A(active);
        else           setDeck1B(active);
    } else {
        if (deck == 1) setDeck2A(active);
        else           setDeck2B(active);
    }

    // When un-assigning, immediately silence FX on that engine
    DjEngine* target = nullptr;
    if      (unitId == 1 && deck == 1) target = m_engineA;
    else if (unitId == 1 && deck == 2) target = m_engineB;
    else if (unitId == 2 && deck == 1) target = m_engineA;
    else                               target = m_engineB; // unitId==2, deck==2

    if (target) {
        if (!active) {
            target->setFxEffectType(EffectType::None);
            target->setFxWetDry(0.0f);
        } else {
            const QString& et = (unitId == 1) ? m_effectType1 : m_effectType2;
            const float    wd = (unitId == 1) ? m_wetDry1     : m_wetDry2;
            target->setFxEffectType(effectTypeFromString(et));
            target->setFxWetDry(wd);
        }
    }
}

// ── SoundColor ────────────────────────────────────────────────────────────────

void FxManager::setSoundColorMode(const QString& mode)
{
    if (m_soundColorMode == mode) return;

    // When switching AWAY from Filter mode, reset engine filters to neutral
    if (m_soundColorMode == "Filter" && mode != "Filter")
    {
        if (m_engineA) m_engineA->setFilter(0.0);
        if (m_engineB) m_engineB->setFilter(0.0);
    }

    m_soundColorMode = mode;
    emit soundColorModeChanged();
    // Re-apply current values with the new mode
    applySoundColorToEngine(m_engineA, mode, m_soundColorValueA);
    applySoundColorToEngine(m_engineB, mode, m_soundColorValueB);

    // When switching TO Filter mode, apply current knob values as filter
    if (mode == "Filter")
    {
        if (m_engineA) m_engineA->setFilter(static_cast<double>(m_soundColorValueA));
        if (m_engineB) m_engineB->setFilter(static_cast<double>(m_soundColorValueB));
    }

    qDebug() << "[FxManager] SoundColor mode ->" << mode;
}

void FxManager::setSoundColor(const QString& mode, float value)
{
    // Centre knob: left half → deck A gets wetter, right half → deck B gets wetter
    // value 0.0 = full left (wet A, dry B)
    // value 0.5 = centre   (both dry)
    // value 1.0 = full right (dry A, wet B)
    if (m_soundColorMode != mode)
    {
        m_soundColorMode = mode;
        emit soundColorModeChanged();
    }

    const float wetA = (value < 0.5f) ? (0.5f - value) * 2.0f : 0.0f;
    const float wetB = (value > 0.5f) ? (value - 0.5f) * 2.0f : 0.0f;

    m_soundColorValueA = 0.5f + wetA * 0.5f;  // remap back to 0-1 for applySoundColor
    m_soundColorValueB = 0.5f + wetB * 0.5f;

    applySoundColorToEngine(m_engineA, mode, m_soundColorValueA);
    applySoundColorToEngine(m_engineB, mode, m_soundColorValueB);
}

void FxManager::setSoundColorDeck(int deck, float value)
{
    // value is bipolar -1..+1
    if (deck == 1)
    {
        m_soundColorValueA = value;
        applySoundColorToEngine(m_engineA, m_soundColorMode, value);
        // For Filter mode: also drive the engine's built-in filter
        if (m_soundColorMode == "Filter" && m_engineA)
            m_engineA->setFilter(static_cast<double>(value));
    }
    else
    {
        m_soundColorValueB = value;
        applySoundColorToEngine(m_engineB, m_soundColorMode, value);
        if (m_soundColorMode == "Filter" && m_engineB)
            m_engineB->setFilter(static_cast<double>(value));
    }
}

void FxManager::applySoundColorToEngine(DjEngine* engine, const QString& mode, float value)
{
    if (!engine) return;

    // value is bipolar -1..+1.  At 0.0 (centre) = bypass.
    if (std::abs(value) < 0.01f)
    {
        engine->setFxEffectType(EffectType::None);
        engine->setFxWetDry(0.0f);
        engine->setFxSCKnob(0.0f);
        return;
    }

    EffectType type = effectTypeFromString(mode);
    engine->setFxEffectType(type);
    // SC FX handle their own wet/dry internally based on the knob.
    // setFxWetDry(1.0) ensures the SmoothedValue wrapper doesn't attenuate us.
    engine->setFxWetDry(1.0f);
    engine->setFxSCKnob(value);
}

// ── Unit 1 setters ────────────────────────────────────────────────────────────

void FxManager::setEffectType1(const QString& type)
{
    if (m_effectType1 == type) return;
    m_effectType1 = type;
    emit effectType1Changed();
    routeToEngines(1, effectTypeFromString(type), m_wetDry1);
    qDebug() << "[FxManager] FX1 effect ->" << type;
}

void FxManager::setWetDry1(float amount)
{
    if (qFuzzyCompare(m_wetDry1, amount)) return;
    m_wetDry1 = amount;
    emit wetDry1Changed();
    routeToEngines(1, effectTypeFromString(m_effectType1), amount);
}

void FxManager::setDeck1A(bool active)
{
    if (m_deck1A == active) return;
    m_deck1A = active;
    emit deck1AChanged();
}

void FxManager::setDeck1B(bool active)
{
    if (m_deck1B == active) return;
    m_deck1B = active;
    emit deck1BChanged();
}

// ── Unit 2 setters ────────────────────────────────────────────────────────────

void FxManager::setEffectType2(const QString& type)
{
    if (m_effectType2 == type) return;
    m_effectType2 = type;
    emit effectType2Changed();
    routeToEngines(2, effectTypeFromString(type), m_wetDry2);
    qDebug() << "[FxManager] FX2 effect ->" << type;
}

void FxManager::setWetDry2(float amount)
{
    if (qFuzzyCompare(m_wetDry2, amount)) return;
    m_wetDry2 = amount;
    emit wetDry2Changed();
    routeToEngines(2, effectTypeFromString(m_effectType2), amount);
}

void FxManager::setDeck2A(bool active)
{
    if (m_deck2A == active) return;
    m_deck2A = active;
    emit deck2AChanged();
}

void FxManager::setDeck2B(bool active)
{
    if (m_deck2B == active) return;
    m_deck2B = active;
    emit deck2BChanged();
}

