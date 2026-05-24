#pragma once

#include <QObject>
#include <QString>
#include <QDebug>
#include "FxProcessor.h"

class DjEngine;

// ─────────────────────────────────────────────────────────────────────────────
// FxManager  –  bridges the QML FX-UI to the JUCE audio engines.
//
// Call registerEngines(deckA, deckB) from main() after both DjEngines are
// constructed.  All QML invokable methods forward directly to the matching
// DjEngine::setFxEffectType() / setFxWetDry().
// ─────────────────────────────────────────────────────────────────────────────
class FxManager : public QObject
{
    Q_OBJECT

    // ── Per-unit properties (unit 1) ─────────────────────────────────────────
    Q_PROPERTY(QString effectType1  READ effectType1  WRITE setEffectType1  NOTIFY effectType1Changed)
    Q_PROPERTY(float   wetDry1      READ wetDry1      WRITE setWetDry1      NOTIFY wetDry1Changed)
    Q_PROPERTY(bool    deck1A       READ deck1A       WRITE setDeck1A       NOTIFY deck1AChanged)
    Q_PROPERTY(bool    deck1B       READ deck1B       WRITE setDeck1B       NOTIFY deck1BChanged)
    Q_PROPERTY(bool    syncEnabled1 READ syncEnabled1                       NOTIFY syncEnabled1Changed)
    Q_PROPERTY(float   beatDiv1     READ beatDiv1                           NOTIFY beatDiv1Changed)
    Q_PROPERTY(double  displayBpm1  READ displayBpm1                        NOTIFY displayBpm1Changed)

    // ── Per-unit properties (unit 2) ─────────────────────────────────────────
    Q_PROPERTY(QString effectType2  READ effectType2  WRITE setEffectType2  NOTIFY effectType2Changed)
    Q_PROPERTY(float   wetDry2      READ wetDry2      WRITE setWetDry2      NOTIFY wetDry2Changed)
    Q_PROPERTY(bool    deck2A       READ deck2A       WRITE setDeck2A       NOTIFY deck2AChanged)
    Q_PROPERTY(bool    deck2B       READ deck2B       WRITE setDeck2B       NOTIFY deck2BChanged)
    Q_PROPERTY(bool    syncEnabled2 READ syncEnabled2                       NOTIFY syncEnabled2Changed)
    Q_PROPERTY(float   beatDiv2     READ beatDiv2                           NOTIFY beatDiv2Changed)
    Q_PROPERTY(double  displayBpm2  READ displayBpm2                        NOTIFY displayBpm2Changed)

    // ── SoundColor (centre knob) ──────────────────────────────────────────────
    Q_PROPERTY(QString soundColorMode READ soundColorMode WRITE setSoundColorMode NOTIFY soundColorModeChanged)
    Q_PROPERTY(float soundColorParam READ soundColorParam WRITE setSoundColorParam NOTIFY soundColorParamChanged)

public:
    explicit FxManager(QObject* parent = nullptr);

    /// Register both deck engines — must be called before the QML engine loads.
    void registerEngines(DjEngine* deckA, DjEngine* deckB);

    // ── SoundColor ───────────────────────────────────────────────────────────
    QString soundColorMode() const { return m_soundColorMode; }
    float soundColorParam() const { return m_soundColorParam; }
    Q_INVOKABLE void setSoundColorMode(const QString& mode);
    Q_INVOKABLE void setSoundColorParam(float param);

    /// Called from FxBar centre knob — applies SoundColor to BOTH decks.
    /// @param mode   "Space"|"D.Echo"|"Crush"|"Pitch"|"Noise"|"Filter"
    /// @param value  0.0 = full left (wet A), 0.5 = dry, 1.0 = full right (wet B)
    Q_INVOKABLE void setSoundColor(const QString& mode, float value);

    /// Called from per-deck SC knob in the Mixer strip.
    /// @param deck   1 = Deck A, 2 = Deck B
    /// @param value  -1.0 (max left) … 0.0 (centre/bypass) … +1.0 (max right)
    Q_INVOKABLE void setSoundColorDeck(int deck, float value);

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

    // ── BPM sync accessors ────────────────────────────────────────────────────
    bool   syncEnabled1() const { return m_syncEnabled[0]; }
    bool   syncEnabled2() const { return m_syncEnabled[1]; }
    float  beatDiv1()     const { return m_beatDiv[0]; }
    float  beatDiv2()     const { return m_beatDiv[1]; }
    double displayBpm1()  const;
    double displayBpm2()  const;

    // ── QML-callable API ─────────────────────────────────────────────────────
    Q_INVOKABLE void setEffectType(int unitId, const QString& type);
    Q_INVOKABLE void setWetDry(int unitId, float amount);
    Q_INVOKABLE void setDeckAssignment(int unitId, int deck, bool active);
    /// Toggle BPM sync on/off for a unit. When enabled, delay time is locked
    /// to beatDiv beats of the assigned deck's BPM.
    Q_INVOKABLE void setSyncEnabled(int unitId, bool enabled);
    /// Set beat division for synced delay (e.g. 0.25 = 1/4 note, 1.0 = 1 bar).
    Q_INVOKABLE void setBeatDivision(int unitId, float div);

signals:
    void effectType1Changed();
    void wetDry1Changed();
    void deck1AChanged();
    void deck1BChanged();
    void syncEnabled1Changed();
    void beatDiv1Changed();
    void displayBpm1Changed();

    void effectType2Changed();
    void wetDry2Changed();
    void deck2AChanged();
    void deck2BChanged();
    void syncEnabled2Changed();
    void beatDiv2Changed();
    void displayBpm2Changed();

    void soundColorModeChanged();
    void soundColorParamChanged();

private:
    DjEngine* m_engineA = nullptr;
    DjEngine* m_engineB = nullptr;

    // ── SoundColor state ─────────────────────────────────────────────────────
    QString m_soundColorMode { "Filter" };
    float   m_soundColorParam { 0.5f };
    float   m_soundColorValueA { 0.0f };   // bipolar -1..+1 per deck
    float   m_soundColorValueB { 0.0f };

    void applySoundColorToEngine(DjEngine* engine, const QString& mode, float value);

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

    // Convert QML string name → EffectType enum
    static EffectType effectTypeFromString(const QString& name);
    // Forward effect type + wetDry to all assigned engines for a unit
    void routeToEngines(int unitId, EffectType type, float wetDry);

    // ── BPM sync state ────────────────────────────────────────────────────────
    bool   m_syncEnabled[2] { false, false };
    float  m_beatDiv[2]     { 0.25f, 0.25f }; // default 1/4 note
    double m_cachedBpmA     { 0.0 };
    double m_cachedBpmB     { 0.0 };

    // Recompute synced delay seconds and push to all assigned engines for unitId.
    // Passes -1 to the engine when sync is off to restore amount-driven timing.
    void pushSyncedDelay(int unitId);
    // Returns the relevant deck's BPM for unitId (primary deck first).
    double bpmForUnit(int unitId) const;
};
