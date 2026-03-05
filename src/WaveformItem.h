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
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified NOTIFY rectifiedChanged)
    QML_ELEMENT

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    DjEngine* engine() const;
    void setEngine(DjEngine* engine);

    bool rectified() const { return m_rectified; }
    void setRectified(bool r) { if (m_rectified == r) return; m_rectified = r; m_geometryChanged = true; update(); emit rectifiedChanged(); }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* updatePaintNodeData) override;

signals:
    void engineChanged();
    void rectifiedChanged();

private slots:
    void onTrackLoaded();
    void onDataUpdated();
    void onProgressChanged();

private:
    DjEngine* m_engine = nullptr;
    bool m_geometryChanged = false;
    bool m_rectified = false;
};
