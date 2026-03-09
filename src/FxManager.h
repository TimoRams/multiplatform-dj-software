#pragma once

#include <QObject>
#include <QString>
#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────
// FxManager  –  C++ stub that bridges the QML FX-UI to the JUCE audio engine.
//
// Currently implemented as a thin stub that:
//   • Logs all UI changes to qDebug() so you can confirm signal flow.
//   • Stores the current state (effect type, wet/dry, deck assignment) for both
//     FX units so the QML can always read back the current state.
//   • Exposes Q_INVOKABLE methods that QML calls directly.
//
// To wire this up to JUCE later, replace the qDebug() bodies with calls to
// your DSP graph / AudioProcessorGraph nodes.
// ─────────────────────────────────────────────────────────────────────────────
class FxManager : public QObject
{
    Q_OBJECT

    // ── Per-unit properties (unit 1) ─────────────────────────────────────────
    Q_PROPERTY(QString effectType1 READ effectType1 WRITE setEffectType1 NOTIFY effectType1Changed)
    Q_PROPERTY(float   wetDry1     READ wetDry1     WRITE setWetDry1     NOTIFY wetDry1Changed)
    Q_PROPERTY(bool    deck1A      READ deck1A      WRITE setDeck1A      NOTIFY deck1AChanged)
    Q_PROPERTY(bool    deck1B      READ deck1B      WRITE setDeck1B      NOTIFY deck1BChanged)

    // ── Per-unit properties (unit 2) ─────────────────────────────────────────
    Q_PROPERTY(QString effectType2 READ effectType2 WRITE setEffectType2 NOTIFY effectType2Changed)
    Q_PROPERTY(float   wetDry2     READ wetDry2     WRITE setWetDry2     NOTIFY wetDry2Changed)
    Q_PROPERTY(bool    deck2A      READ deck2A      WRITE setDeck2A      NOTIFY deck2AChanged)
    Q_PROPERTY(bool    deck2B      READ deck2B      WRITE setDeck2B      NOTIFY deck2BChanged)

public:
    explicit FxManager(QObject* parent = nullptr);

    // ── Accessors – unit 1 ───────────────────────────────────────────────────
    QString effectType1() const { return m_effectType1; }
    float   wetDry1()     const { return m_wetDry1;     }
    bool    deck1A()      const { return m_deck1A;      }
    bool    deck1B()      const { return m_deck1B;      }

    void setEffectType1(const QString& type);
    void setWetDry1(float amount);
    void setDeck1A(bool active);
    void setDeck1B(bool active);

    // ── Accessors – unit 2 ───────────────────────────────────────────────────
    QString effectType2() const { return m_effectType2; }
    float   wetDry2()     const { return m_wetDry2;     }
    bool    deck2A()      const { return m_deck2A;      }
    bool    deck2B()      const { return m_deck2B;      }

    void setEffectType2(const QString& type);
    void setWetDry2(float amount);
    void setDeck2A(bool active);
    void setDeck2B(bool active);

    // ── QML-callable API ─────────────────────────────────────────────────────
    /// Called by FxUnit whenever the user picks a new effect.
    /// @param unitId  1 or 2
    /// @param type    Effect name string (e.g. "Echo", "Reverb", …)
    Q_INVOKABLE void setEffectType(int unitId, const QString& type);

    /// Called by FxUnit whenever the Wet/Dry knob changes.
    /// @param unitId  1 or 2
    /// @param amount  0.0 … 1.0
    Q_INVOKABLE void setWetDry(int unitId, float amount);

    /// Called by FxUnit whenever a deck-assign button is toggled.
    /// @param unitId  1 or 2
    /// @param deck    1 (Deck A) or 2 (Deck B)
    /// @param active  true = assigned, false = removed
    Q_INVOKABLE void setDeckAssignment(int unitId, int deck, bool active);

signals:
    void effectType1Changed();
    void wetDry1Changed();
    void deck1AChanged();
    void deck1BChanged();

    void effectType2Changed();
    void wetDry2Changed();
    void deck2AChanged();
    void deck2BChanged();

private:
    // ── Unit 1 state ─────────────────────────────────────────────────────────
    QString m_effectType1 { "---" };
    float   m_wetDry1     { 0.0f  };
    bool    m_deck1A      { false };
    bool    m_deck1B      { false };

    // ── Unit 2 state ─────────────────────────────────────────────────────────
    QString m_effectType2 { "---" };
    float   m_wetDry2     { 0.0f  };
    bool    m_deck2A      { false };
    bool    m_deck2B      { false };

    // ── Internal helper ──────────────────────────────────────────────────────
    // Forwards a parameter change to the JUCE DSP graph (stub: just logs).
    void applyToEngine(int unitId, const QString& param, float value);
};
