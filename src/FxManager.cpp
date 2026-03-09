#include "FxManager.h"
#include "DjEngine.h"

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
    if (name == "Reverb")     return EffectType::Reverb;
    if (name == "Bitcrusher") return EffectType::Bitcrusher;
    if (name == "Pitch Shifter" || name == "PitchShifter")
                              return EffectType::PitchShifter;
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

