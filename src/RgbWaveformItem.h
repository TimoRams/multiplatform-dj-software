#pragma once

#include <QQuickPaintedItem>
#include <QtQml/qqml.h>

#include "DjEngine.h"
#include "TrackData.h"

class RgbWaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified NOTIFY rectifiedChanged)
    QML_ELEMENT

public:
    explicit RgbWaveformItem(QQuickItem* parent = nullptr);

    DjEngine* engine() const { return m_engine; }
    void setEngine(DjEngine* engine);

    bool rectified() const { return m_rectified; }
    void setRectified(bool v);

    void paint(QPainter* painter) override;

signals:
    void engineChanged();
    void rectifiedChanged();

private slots:
    void onTrackLoaded();
    void onRgbDataChanged();

private:
    DjEngine* m_engine = nullptr;
    bool m_rectified = true;
};
