#include "FxManager.h"

// ─────────────────────────────────────────────────────────────────────────────
// FxManager implementation
// ─────────────────────────────────────────────────────────────────────────────

FxManager::FxManager(QObject* parent)
    : QObject(parent)
{
    qDebug() << "[FxManager] initialised";
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
    qDebug() << "[FxManager] FX" << unitId << "→ deck" << deck
             << (active ? "ASSIGNED" : "REMOVED");

    if (unitId == 1) {
        if (deck == 1) setDeck1A(active);
        else           setDeck1B(active);
    } else {
        if (deck == 1) setDeck2A(active);
        else           setDeck2B(active);
    }

    // TODO: route / un-route DSP chain in JUCE AudioProcessorGraph
}

// ── Unit 1 setters ────────────────────────────────────────────────────────────

void FxManager::setEffectType1(const QString& type)
{
    if (m_effectType1 == type) return;
    m_effectType1 = type;
    emit effectType1Changed();
    applyToEngine(1, "effectType", 0.0f); // value unused for string param
    qDebug() << "[FxManager] FX1 effect →" << type;
}

void FxManager::setWetDry1(float amount)
{
    if (qFuzzyCompare(m_wetDry1, amount)) return;
    m_wetDry1 = amount;
    emit wetDry1Changed();
    applyToEngine(1, "wetDry", amount);
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
    applyToEngine(2, "effectType", 0.0f);
    qDebug() << "[FxManager] FX2 effect →" << type;
}

void FxManager::setWetDry2(float amount)
{
    if (qFuzzyCompare(m_wetDry2, amount)) return;
    m_wetDry2 = amount;
    emit wetDry2Changed();
    applyToEngine(2, "wetDry", amount);
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

// ── Internal ─────────────────────────────────────────────────────────────────

void FxManager::applyToEngine(int unitId, const QString& param, float value)
{
    // ── STUB ──────────────────────────────────────────────────────────────────
    // Replace with calls to your JUCE DSP nodes, e.g.:
    //
    //   if (param == "wetDry") {
    //       auto* fxNode = m_graph->getNodeForId(fxNodeId[unitId]);
    //       if (fxNode) fxNode->getProcessor()->setParameter(WET_DRY_IDX, value);
    //   }
    //
    qDebug() << "[FxManager] applyToEngine  unit=" << unitId
             << " param=" << param << " value=" << value;
}
