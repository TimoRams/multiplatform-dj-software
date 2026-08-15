#pragma once

#include "domain/DeckId.h"

#include <QObject>
#include <QString>

#include <array>

class DjEngine;
class ParameterStore;

// The mixer's state owner. Everything that can move a channel strip — QML
// directly, or MIDI/UI by way of ParameterStore — arrives here, updates the one
// copy of the channel state and forwards it to the deck. Nothing else keeps a
// second copy, so the hardware and the on-screen strip cannot drift apart.
class MixerControl : public QObject {
    Q_OBJECT
public:
    explicit MixerControl(QObject* parent = nullptr);

    void setDecks(DjEngine* deckA, DjEngine* deckB, DjEngine* deckC, DjEngine* deckD);

    // Normalized 0–1 parameters (MIDI, UI knobs) enter through the store; this
    // class converts them to engine ranges and applies them like any other move.
    void attachParameterStore(ParameterStore* store);

    Q_INVOKABLE void setTrim(const QString& channelId, double value);
    Q_INVOKABLE void setEqHigh(const QString& channelId, double value);
    Q_INVOKABLE void setEqMid(const QString& channelId, double value);
    Q_INVOKABLE void setEqLow(const QString& channelId, double value);
    Q_INVOKABLE void setFilter(const QString& channelId, double value);
    Q_INVOKABLE void setPolarityInverted(const QString& channelId, bool inverted);
    Q_INVOKABLE void toggleCue(const QString& channelId);
    Q_INVOKABLE void setChannelFader(const QString& channelId, double level);
    Q_INVOKABLE void setCrossfaderPosition(float cfPos);

    Q_INVOKABLE void syncCrossfaderState(float cfPos,
                                         const QString& assignA,
                                         const QString& assignB,
                                         const QString& assignC,
                                         const QString& assignD,
                                         float cfSharpness,
                                         const QString& cfCurveMode);

    Q_INVOKABLE void applyAllVolumes();
    Q_INVOKABLE void applyAllMixState();
    Q_INVOKABLE double faderLevel(const QString& channelId) const;

private:
    struct ChannelMixState {
        double trim = 1.0;
        double eqHigh = 0.0;
        double eqMid = 0.0;
        double eqLow = 0.0;
        double filter = 0.0;
        bool polarityInverted = false;
        float fader = 1.0f;
    };

    void onParameterChanged(const QString& id, float value);

    [[nodiscard]] DjEngine* deck(domain::DeckId id) const;
    void applyChannelVolume(domain::DeckId id);
    void applyChannelMixState(domain::DeckId id);

    std::array<DjEngine*, domain::kDeckCount> m_decks {};
    std::array<ChannelMixState, domain::kDeckCount> m_mix {};

    float m_cfPos = 0.0f;
    float m_cfSharpness = 0.0f;
    QString m_cfCurveMode = QStringLiteral("exponential");
    QString m_assignA = QStringLiteral("A");
    QString m_assignB = QStringLiteral("B");
    QString m_assignC = QStringLiteral("A");
    QString m_assignD = QStringLiteral("B");
};
