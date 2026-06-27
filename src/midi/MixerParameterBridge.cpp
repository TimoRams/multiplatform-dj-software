#include "MixerParameterBridge.h"

#include "ParameterStore.h"
#include "DjEngine.h"

MixerParameterBridge::MixerParameterBridge(ParameterStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    if (m_store)
        connect(m_store, &ParameterStore::parameterChanged,
                this, &MixerParameterBridge::onParameterChanged);
}

void MixerParameterBridge::setDecks(DjEngine* deckA, DjEngine* deckB, DjEngine* deckC, DjEngine* deckD)
{
    m_decks = { deckA, deckB, deckC, deckD };
}

DjEngine* MixerParameterBridge::deckForChannelId(const QString& channelId) const
{
    if (channelId == QLatin1String("deckA")) return m_decks[0];
    if (channelId == QLatin1String("deckB")) return m_decks[1];
    if (channelId == QLatin1String("deckC")) return m_decks[2];
    if (channelId == QLatin1String("deckD")) return m_decks[3];
    return nullptr;
}

void MixerParameterBridge::onParameterChanged(const QString& id, float value)
{
    if (!id.startsWith(QLatin1String("deck")))
        return;

    const int sep = id.indexOf(QLatin1Char('_'));
    if (sep <= 4)
        return;

    const QString channelId = id.left(sep);
    const QString suffix    = id.mid(sep + 1);

    DjEngine* const deck = deckForChannelId(channelId);
    if (!deck)
        return;

    if (suffix == QLatin1String("gain")) {
        deck->setTrim(static_cast<double>(value) * 2.0);
    } else if (suffix == QLatin1String("eqHigh")) {
        deck->setEqHigh(static_cast<double>(value) * 2.0 - 1.0);
    } else if (suffix == QLatin1String("eqMid")) {
        deck->setEqMid(static_cast<double>(value) * 2.0 - 1.0);
    } else if (suffix == QLatin1String("eqLow")) {
        deck->setEqLow(static_cast<double>(value) * 2.0 - 1.0);
    } else if (suffix == QLatin1String("filter")) {
        deck->setFilter(static_cast<double>(value) * 2.0 - 1.0);
    } else if (suffix == QLatin1String("headphone_cue")) {
        if (value >= 0.5f)
            deck->setCueEnabled(!deck->cueEnabled());
    }
    // deckX_vol and crossfader are applied in QML (CrossfaderBar).
}
