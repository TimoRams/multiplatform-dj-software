#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqml.h>
#include "DjEngine.h"
#include "TrackData.h"

class DeckBoundQuickItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    QML_NAMED_ELEMENT(DeckBoundQuickItem)
    QML_UNCREATABLE("DeckBoundQuickItem is a base class for deck-bound waveform items.")

public:
    explicit DeckBoundQuickItem(QQuickItem* parent = nullptr);

    DjEngine* engine() const;
    void setEngine(DjEngine* engine);

signals:
    void engineChanged();

protected:
    virtual void onEngineChanged() {}
    virtual void onTrackLoaded() {}
    virtual void onTrackEjected() {}
    virtual void onTrackDataUpdated() {}

    DjEngine* deckEngine() const { return m_engine.data(); }

private slots:
    void handleTrackLoaded();
    void handleTrackEjected();
    void handleTrackDataUpdated();

private:
    void disconnectTrackData();
    void connectTrackData();

    QPointer<DjEngine> m_engine;
};
