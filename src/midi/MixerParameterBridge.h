#pragma once

#include <QObject>
#include <array>

class ParameterStore;
class DjEngine;

// Applies mixer channel parameters from ParameterStore to all four decks.
// UI knobs/faders write normalized values here; MIDI uses the same store.
class MixerParameterBridge : public QObject {
    Q_OBJECT
public:
    explicit MixerParameterBridge(ParameterStore* store, QObject* parent = nullptr);

    void setDecks(DjEngine* deckA, DjEngine* deckB, DjEngine* deckC, DjEngine* deckD);

private:
    void onParameterChanged(const QString& id, float value);

    [[nodiscard]] DjEngine* deckForChannelId(const QString& channelId) const;

    ParameterStore* m_store = nullptr;
    std::array<DjEngine*, 4> m_decks {};
};
