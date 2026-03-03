#pragma once

#include <QQuickItem>
#include <QVector>
#include <QtQml/qqml.h>
#include "DjEngine.h"
#include "TrackData.h"

class WaveformItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    QML_ELEMENT

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    DjEngine* engine() const;
    void setEngine(DjEngine* engine);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* updatePaintNodeData) override;

signals:
    void engineChanged();

private slots:
    void onTrackLoaded();
    void onDataUpdated();
    void onProgressChanged();

private:
    DjEngine* m_engine = nullptr;
    bool m_geometryChanged = false;
};
