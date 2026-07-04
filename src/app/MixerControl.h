#pragma once

#include <QObject>
#include <QString>

class DjEngine;

// C++ mixer facade — direct deck access for QML (same path as CrossfaderBar).
class MixerControl : public QObject {
    Q_OBJECT
public:
    explicit MixerControl(QObject* parent = nullptr);

    void setDecks(DjEngine* deckA, DjEngine* deckB, DjEngine* deckC, DjEngine* deckD);

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

    // State-only sync when MixerParameterBridge applies MIDI/store moves (no deck apply).
    void syncMixFromNormalized(const QString& channelId, const QString& suffix, float normalized);

private:
    struct ChannelMixState {
        double trim = 1.0;
        double eqHigh = 0.0;
        double eqMid = 0.0;
        double eqLow = 0.0;
        double filter = 0.0;
        bool polarityInverted = false;
    };

    [[nodiscard]] DjEngine* deckForChannelId(const QString& channelId) const;
    [[nodiscard]] ChannelMixState* mixStateForChannel(const QString& channelId);
    [[nodiscard]] const ChannelMixState* mixStateForChannel(const QString& channelId) const;
    [[nodiscard]] float crossfaderMultiplierForChannel(const QString& channelId) const;
    void applyChannelVolume(const QString& channelId);
    void applyChannelMixState(const QString& channelId);

    DjEngine* m_deckA = nullptr;
    DjEngine* m_deckB = nullptr;
    DjEngine* m_deckC = nullptr;
    DjEngine* m_deckD = nullptr;

    float m_cfPos = 0.0f;
    float m_cfSharpness = 0.0f;
    QString m_cfCurveMode = QStringLiteral("exponential");
    QString m_assignA = QStringLiteral("A");
    QString m_assignB = QStringLiteral("B");
    QString m_assignC = QStringLiteral("A");
    QString m_assignD = QStringLiteral("B");

    float m_faderA = 1.0f;
    float m_faderB = 1.0f;
    float m_faderC = 1.0f;
    float m_faderD = 1.0f;

    ChannelMixState m_mixA {};
    ChannelMixState m_mixB {};
    ChannelMixState m_mixC {};
    ChannelMixState m_mixD {};
};
