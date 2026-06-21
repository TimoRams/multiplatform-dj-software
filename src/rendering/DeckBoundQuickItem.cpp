#include "DeckBoundQuickItem.h"

DeckBoundQuickItem::DeckBoundQuickItem(QQuickItem* parent)
    : QQuickItem(parent)
{
}

DjEngine* DeckBoundQuickItem::engine() const
{
    return m_engine.data();
}

void DeckBoundQuickItem::setEngine(DjEngine* engine)
{
    if (m_engine.data() == engine)
        return;

    disconnectTrackData();
    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);

    m_engine = engine;

    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded, this, &DeckBoundQuickItem::handleTrackLoaded);
        connect(m_engine, &DjEngine::trackEjected, this, &DeckBoundQuickItem::handleTrackEjected);
        connectTrackData();
    }

    onEngineChanged();
    emit engineChanged();
}

void DeckBoundQuickItem::disconnectTrackData()
{
    if (DjEngine* eng = m_engine.data()) {
        if (TrackData* td = eng->getTrackData())
            disconnect(td, nullptr, this, nullptr);
    }
}

void DeckBoundQuickItem::connectTrackData()
{
    if (DjEngine* eng = m_engine.data()) {
        if (TrackData* td = eng->getTrackData()) {
            connect(td, &TrackData::dataUpdated, this, &DeckBoundQuickItem::handleTrackDataUpdated,
                    Qt::UniqueConnection);
            connect(td, &TrackData::dataCleared, this, &DeckBoundQuickItem::handleTrackDataUpdated,
                    Qt::UniqueConnection);
        }
    }
}

void DeckBoundQuickItem::handleTrackLoaded()
{
    connectTrackData();
    onTrackLoaded();
}

void DeckBoundQuickItem::handleTrackEjected()
{
    disconnectTrackData();
    onTrackEjected();
}

void DeckBoundQuickItem::handleTrackDataUpdated()
{
    onTrackDataUpdated();
}
