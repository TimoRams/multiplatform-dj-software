#include "MixerParameterBridge.h"

#include "app/DeckChannels.h"
#include "app/MixerControl.h"
#include "ParameterStore.h"
#include "deck/DjEngine.h"

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

void MixerParameterBridge::setMixerControl(MixerControl* mixerControl)
{
    m_mixerControl = mixerControl;
}

DjEngine* MixerParameterBridge::deckForChannelId(const QString& channelId) const
{
    return ::deckForChannelId(channelId, m_decks[0], m_decks[1], m_decks[2], m_decks[3]);
}

void MixerParameterBridge::onParameterChanged(const QString& id, float value)
{
    if (id == QLatin1String("crossfader")) {
        if (m_mixerControl)
            m_mixerControl->setCrossfaderPosition(value * 2.0f - 1.0f);
        return;
    }

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

    // Silent apply* path: UI/MIDI knobs sync via parameterStore, not engine NOTIFY.
    if (suffix == QLatin1String("gain")) {
        deck->applyTrim(mixerTrimFromNormalized(value));
    } else if (suffix == QLatin1String("eqHigh")) {
        deck->applyEqHigh(mixerBipolarFromNormalized(value));
    } else if (suffix == QLatin1String("eqMid")) {
        deck->applyEqMid(mixerBipolarFromNormalized(value));
    } else if (suffix == QLatin1String("eqLow")) {
        deck->applyEqLow(mixerBipolarFromNormalized(value));
    } else if (suffix == QLatin1String("filter")) {
        deck->applyFilter(mixerBipolarFromNormalized(value));
    } else if (suffix == QLatin1String("vol")) {
        // Channel fader + MixerControl state (crossfader still via CrossfaderBar QML).
        if (m_mixerControl)
            m_mixerControl->setChannelFader(channelId, static_cast<double>(value));
        return;
    } else if (suffix == QLatin1String("headphone_cue")) {
        if (value >= 0.5f)
            deck->setCueEnabled(!deck->cueEnabled());
        return;
    } else {
        return;
    }

    if (m_mixerControl)
        m_mixerControl->syncMixFromNormalized(channelId, suffix, value);
}
