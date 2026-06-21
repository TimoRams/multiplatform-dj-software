#pragma once

#include "DeckBoundQuickItem.h"
#include <QVector>
#include <QtQml/qqml.h>

class WaveformItem : public DeckBoundQuickItem
{
    Q_OBJECT
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified NOTIFY rectifiedChanged)
    QML_ELEMENT

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    bool rectified() const { return m_rectified; }
    void setRectified(bool r);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* updatePaintNodeData) override;
    void onEngineChanged() override;
    void onTrackLoaded() override;
    void onTrackEjected() override;
    void onTrackDataUpdated() override;

signals:
    void rectifiedChanged();

private slots:
    void onProgressChanged();

private:
    bool m_geometryChanged = false;
    bool m_rectified = false;
};
