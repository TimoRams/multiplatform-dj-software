#include "FxManager.h"
#include "domain/DeckId.h"
#include "deck/DjEngine.h"

#include <QDebug>

#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// FxManager implementation
// ─────────────────────────────────────────────────────────────────────────────

FxManager::FxManager(QObject* parent)
    : QObject(parent)
{
    qDebug() << "[FxManager] initialised";
}

void FxManager::registerEngines(DjEngine* deckA, DjEngine* deckB,
                              DjEngine* deckC, DjEngine* deckD)
{
    m_engineA = deckA;
    m_engineB = deckB;
    m_engineC = deckC;
    m_engineD = deckD;

    if (m_engineA) {
        connect(m_engineA, &DjEngine::tempoChanged, this, [this]() {
            const double bpm = m_engineA->getCurrentBpm();
            if (qFuzzyCompare(bpm, m_cachedBpmA)) return;
            m_cachedBpmA = bpm;
            emit displayBpm1Changed();
            emit displayBpm2Changed();
            pushSyncedDelay(1);
            pushSyncedDelay(2);
        });
    }
    if (m_engineB) {
        connect(m_engineB, &DjEngine::tempoChanged, this, [this]() {
            const double bpm = m_engineB->getCurrentBpm();
            if (qFuzzyCompare(bpm, m_cachedBpmB)) return;
            m_cachedBpmB = bpm;
            emit displayBpm1Changed();
            emit displayBpm2Changed();
            pushSyncedDelay(1);
            pushSyncedDelay(2);
        });
    }
    if (m_engineC) {
        connect(m_engineC, &DjEngine::tempoChanged, this, [this]() {
            const double bpm = m_engineC->getCurrentBpm();
            if (qFuzzyCompare(bpm, m_cachedBpmC)) return;
            m_cachedBpmC = bpm;
            emit displayBpm1Changed();
            emit displayBpm2Changed();
            pushSyncedDelay(1);
            pushSyncedDelay(2);
        });
    }
    if (m_engineD) {
        connect(m_engineD, &DjEngine::tempoChanged, this, [this]() {
            const double bpm = m_engineD->getCurrentBpm();
            if (qFuzzyCompare(bpm, m_cachedBpmD)) return;
            m_cachedBpmD = bpm;
            emit displayBpm1Changed();
            emit displayBpm2Changed();
            pushSyncedDelay(1);
            pushSyncedDelay(2);
        });
    }

    qDebug() << "[FxManager] engines registered";
}

// ── Helpers ───────────────────────────────────────────────────────────────────

EffectType FxManager::effectTypeFromString(const QString& name)
{
    if (name == "Reverb")        return EffectType::Reverb;
    if (name == "Bitcrusher")    return EffectType::Bitcrusher;
    if (name == "Pitch Shifter" || name == "PitchShifter")
                                 return EffectType::PitchShifter;
    if (name == "Echo")                          return EffectType::Echo;
    if (name == "Low Cut Echo")                  return EffectType::LowCutEcho;
    if (name == "MT Delay" ||
        name == "Multi-Tap Delay")               return EffectType::MtDelay;
    if (name == "Spiral")                        return EffectType::Spiral;
    if (name == "Flanger")                       return EffectType::Flanger;
    if (name == "Phaser")                        return EffectType::Phaser;
    if (name == "Trans" || name == "Tremolo")    return EffectType::Trans;
    if (name == "Enigma Jet")                    return EffectType::EnigmaJet;
    if (name == "Stretch")                       return EffectType::Stretch;
    if (name == "Slip Roll")                     return EffectType::SlipRoll;
    if (name == "Roll")                          return EffectType::Roll;
    if (name == "Roll Out")                      return EffectType::RollOut;
    // "Nobius"/"Mobius" are the previous names, kept so saved sessions load.
    if (name == "Mobius Saw" || name == "Nobius") return EffectType::MobiusSaw;
    if (name == "Mobius Tri" || name == "Mobius") return EffectType::MobiusTri;
    // ── SoundColor mode aliases ──────────────────────────────────────────────
    if (name == "Space")         return EffectType::SoundColorSpace;
    if (name == "D.Echo")        return EffectType::SoundColorDubEcho;
    if (name == "Crush")         return EffectType::SoundColorCrush;
    if (name == "Pitch")         return EffectType::SoundColorPitch;
    if (name == "Noise")         return EffectType::SoundColorNoise;
    if (name == "Sweep")         return EffectType::SoundColorSweep;
    if (name == "Filter")        return EffectType::SoundColorFilter;
    return EffectType::None;
}

void FxManager::routeToEngines(int unitId, EffectType type, float wetDry)
{
    // Unit 1 routes to DeckA when deck1A==true, optionally also DeckB when deck1B==true.
    // Unit 2 routes to DeckB when deck2B==true, optionally also DeckA when deck2A==true.
    if (unitId == 1) {
        if (m_deck1A && m_engineA) { m_engineA->setFxSlotEffectType(1, type); m_engineA->setFxSlotWetDry(1, wetDry); }
        if (m_deck1B && m_engineB) { m_engineB->setFxSlotEffectType(1, type); m_engineB->setFxSlotWetDry(1, wetDry); }
        if (m_deck1C && m_engineC) { m_engineC->setFxSlotEffectType(1, type); m_engineC->setFxSlotWetDry(1, wetDry); }
        if (m_deck1D && m_engineD) { m_engineD->setFxSlotEffectType(1, type); m_engineD->setFxSlotWetDry(1, wetDry); }
    } else {
        if (m_deck2A && m_engineA) { m_engineA->setFxSlotEffectType(2, type); m_engineA->setFxSlotWetDry(2, wetDry); }
        if (m_deck2B && m_engineB) { m_engineB->setFxSlotEffectType(2, type); m_engineB->setFxSlotWetDry(2, wetDry); }
        if (m_deck2C && m_engineC) { m_engineC->setFxSlotEffectType(2, type); m_engineC->setFxSlotWetDry(2, wetDry); }
        if (m_deck2D && m_engineD) { m_engineD->setFxSlotEffectType(2, type); m_engineD->setFxSlotWetDry(2, wetDry); }
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
    if (unitId < 1 || unitId > 2 || deck < 1 || deck > 4)
        return;

    if (unitId == 1) {
        switch (deck) {
        case 1: setDeck1A(active); break;
        case 2: setDeck1B(active); break;
        case 3: setDeck1C(active); break;
        case 4: setDeck1D(active); break;
        }
    } else {
        switch (deck) {
        case 1: setDeck2A(active); break;
        case 2: setDeck2B(active); break;
        case 3: setDeck2C(active); break;
        case 4: setDeck2D(active); break;
        }
    }

    // When un-assigning, immediately silence FX on that engine
    DjEngine* const targets[] = { m_engineA, m_engineB, m_engineC, m_engineD };
    DjEngine* target = targets[deck - 1];

    if (target) {
        if (!active) {
            target->setFxSlotEffectType(unitId, EffectType::None);
            target->setFxSlotWetDry(unitId, 0.0f);
            target->setFxSlotExternalDelayTime(unitId, -1.f);
        } else {
            const QString& et  = (unitId == 1) ? m_effectType1    : m_effectType2;
            const float    wd  = (unitId == 1) ? m_wetDry1        : m_wetDry2;
            const float    pp  = m_primaryParam[unitId - 1];
            target->setFxSlotEffectType(unitId, effectTypeFromString(et));
            target->setFxSlotWetDry(unitId, wd);
            target->setFxSlotPrimaryParam(unitId, pp);
        }
    }
    // Re-push synced delay so newly assigned/unassigned engines get the right timing.
    emit displayBpm1Changed();
    emit displayBpm2Changed();
    pushSyncedDelay(unitId);
}

// ── SoundColor ────────────────────────────────────────────────────────────────

void FxManager::setSoundColorMode(const QString& mode)
{
    if (m_soundColorMode == mode) return;

    // When switching AWAY from Filter mode, reset engine filters to neutral
    if (m_soundColorMode == "Filter" && mode != "Filter")
    {
        if (m_engineA) m_engineA->applyFilter(0.0);
        if (m_engineB) m_engineB->applyFilter(0.0);
    }

    m_soundColorMode = mode;
    emit soundColorModeChanged();
    // Re-apply current values with the new mode
    applySoundColorToEngine(m_engineA, mode, m_soundColorValueA);
    applySoundColorToEngine(m_engineB, mode, m_soundColorValueB);

    // When switching TO Filter mode, apply current knob values as filter
    if (mode == "Filter")
    {
        if (m_engineA) m_engineA->applyFilter(static_cast<double>(m_soundColorValueA));
        if (m_engineB) m_engineB->applyFilter(static_cast<double>(m_soundColorValueB));
    }

    qDebug() << "[FxManager] SoundColor mode ->" << mode;
}

void FxManager::setSoundColorParam(float param)
{
    const float clamped = std::clamp(param, 0.0f, 1.0f);
    if (qFuzzyCompare(m_soundColorParam, clamped))
        return;

    m_soundColorParam = clamped;
    emit soundColorParamChanged();

    if (m_engineA) m_engineA->setFxSCParam(m_soundColorParam);
    if (m_engineB) m_engineB->setFxSCParam(m_soundColorParam);
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
    value = std::clamp(value, -1.0f, 1.0f);
    if (deck == 1)
    {
        m_soundColorValueA = value;
        applySoundColorToEngine(m_engineA, m_soundColorMode, value);
    }
    else if (deck == 2)
    {
        m_soundColorValueB = value;
        applySoundColorToEngine(m_engineB, m_soundColorMode, value);
    }
}

void FxManager::setSoundColorChannel(const QString& channelId, float value)
{
    value = std::clamp(value, -1.0f, 1.0f);
    if (DjEngine* const engine = engineForChannelId(channelId))
        applySoundColorToEngine(engine, m_soundColorMode, value);

    if (channelId == QLatin1String("deckA")) m_soundColorValueA = value;
    else if (channelId == QLatin1String("deckB")) m_soundColorValueB = value;
}

DjEngine* FxManager::engineForChannelId(const QString& channelId) const
{
    const auto deck = domain::deckFromChannelId(channelId);
    return deck ? domain::selectDeck(*deck, m_engineA, m_engineB, m_engineC, m_engineD)
                : nullptr;
}

void FxManager::applySoundColorToEngine(DjEngine* engine, const QString& mode, float value)
{
    if (!engine) return;

    engine->setFxSCParam(m_soundColorParam);

    // "Filter" mode should use the deck's native DJ filter path only.
    // Running SC filter DSP on top of the mixer filter causes double-filtering
    // and unstable/unnatural behavior.
    if (mode == "Filter") {
        engine->setFxEffectType(EffectType::None);
        engine->setFxWetDry(0.0f);
        engine->setFxSCKnob(0.0f);
        engine->applyFilter(static_cast<double>(value));
        return;
    }

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

// ── BPM sync ──────────────────────────────────────────────────────────────────

double FxManager::bpmForUnit(int unitId) const
{
    if (unitId == 1) {
        if (m_deck1A && m_cachedBpmA > 0.0) return m_cachedBpmA;
        if (m_deck1B && m_cachedBpmB > 0.0) return m_cachedBpmB;
        if (m_deck1C && m_cachedBpmC > 0.0) return m_cachedBpmC;
        if (m_deck1D && m_cachedBpmD > 0.0) return m_cachedBpmD;
    } else {
        if (m_deck2B && m_cachedBpmB > 0.0) return m_cachedBpmB;
        if (m_deck2A && m_cachedBpmA > 0.0) return m_cachedBpmA;
        if (m_deck2C && m_cachedBpmC > 0.0) return m_cachedBpmC;
        if (m_deck2D && m_cachedBpmD > 0.0) return m_cachedBpmD;
    }
    return 0.0;
}

double FxManager::displayBpm1() const { return bpmForUnit(1); }
double FxManager::displayBpm2() const { return bpmForUnit(2); }

void FxManager::pushSyncedDelay(int unitId)
{
    const bool enabled = m_syncEnabled[unitId - 1];
    const float seconds = enabled
        ? static_cast<float>((60.0 / std::max(1.0, bpmForUnit(unitId))) * m_beatDiv[unitId - 1])
        : -1.f;

    if (unitId == 1) {
        if (m_deck1A && m_engineA) m_engineA->setFxSlotExternalDelayTime(1, seconds);
        if (m_deck1B && m_engineB) m_engineB->setFxSlotExternalDelayTime(1, seconds);
        if (m_deck1C && m_engineC) m_engineC->setFxSlotExternalDelayTime(1, seconds);
        if (m_deck1D && m_engineD) m_engineD->setFxSlotExternalDelayTime(1, seconds);
    } else {
        if (m_deck2A && m_engineA) m_engineA->setFxSlotExternalDelayTime(2, seconds);
        if (m_deck2B && m_engineB) m_engineB->setFxSlotExternalDelayTime(2, seconds);
        if (m_deck2C && m_engineC) m_engineC->setFxSlotExternalDelayTime(2, seconds);
        if (m_deck2D && m_engineD) m_engineD->setFxSlotExternalDelayTime(2, seconds);
    }
}

void FxManager::setSyncEnabled(int unitId, bool enabled)
{
    const int idx = unitId - 1;
    if (idx < 0 || idx > 1 || m_syncEnabled[idx] == enabled) return;
    m_syncEnabled[idx] = enabled;
    if (unitId == 1) emit syncEnabled1Changed();
    else             emit syncEnabled2Changed();
    pushSyncedDelay(unitId);
}

void FxManager::setBeatDivision(int unitId, float div)
{
    const int idx = unitId - 1;
    if (idx < 0 || idx > 1 || qFuzzyCompare(m_beatDiv[idx], div)) return;
    m_beatDiv[idx] = div;
    if (unitId == 1) emit beatDiv1Changed();
    else             emit beatDiv2Changed();
    pushSyncedDelay(unitId);
}

void FxManager::setPrimaryParam(int unitId, float v)
{
    const int idx = unitId - 1;
    if (idx < 0 || idx > 1) return;
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    if (qFuzzyCompare(m_primaryParam[idx], clamped)) return;
    m_primaryParam[idx] = clamped;
    if (unitId == 1) emit primaryParam1Changed();
    else             emit primaryParam2Changed();
    // Route to all assigned engines
    if (unitId == 1) {
        if (m_deck1A && m_engineA) m_engineA->setFxSlotPrimaryParam(1, clamped);
        if (m_deck1B && m_engineB) m_engineB->setFxSlotPrimaryParam(1, clamped);
        if (m_deck1C && m_engineC) m_engineC->setFxSlotPrimaryParam(1, clamped);
        if (m_deck1D && m_engineD) m_engineD->setFxSlotPrimaryParam(1, clamped);
    } else {
        if (m_deck2A && m_engineA) m_engineA->setFxSlotPrimaryParam(2, clamped);
        if (m_deck2B && m_engineB) m_engineB->setFxSlotPrimaryParam(2, clamped);
        if (m_deck2C && m_engineC) m_engineC->setFxSlotPrimaryParam(2, clamped);
        if (m_deck2D && m_engineD) m_engineD->setFxSlotPrimaryParam(2, clamped);
    }
}

// ── Unit 1 setters ────────────────────────────────────────────────────────────

void FxManager::setEffectType1(const QString& type)
{
    if (m_effectType1 == type) return;
    m_effectType1 = type;
    emit effectType1Changed();
    routeToEngines(1, effectTypeFromString(type), m_wetDry1);
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

void FxManager::setDeck1C(bool active)
{
    if (m_deck1C == active) return;
    m_deck1C = active;
    emit deck1CChanged();
}

void FxManager::setDeck1D(bool active)
{
    if (m_deck1D == active) return;
    m_deck1D = active;
    emit deck1DChanged();
}

// ── Unit 2 setters ────────────────────────────────────────────────────────────

void FxManager::setEffectType2(const QString& type)
{
    if (m_effectType2 == type) return;
    m_effectType2 = type;
    emit effectType2Changed();
    routeToEngines(2, effectTypeFromString(type), m_wetDry2);
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

void FxManager::setDeck2C(bool active)
{
    if (m_deck2C == active) return;
    m_deck2C = active;
    emit deck2CChanged();
}

void FxManager::setDeck2D(bool active)
{
    if (m_deck2D == active) return;
    m_deck2D = active;
    emit deck2DChanged();
}
