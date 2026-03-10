#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include "DjEngine.h"
#include "TrackData.h"

class ScrollingWaveformItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(float pixelsPerPoint READ pixelsPerPoint WRITE setPixelsPerPoint NOTIFY pixelsPerPointChanged)
    QML_ELEMENT

public:
    explicit ScrollingWaveformItem(QQuickItem* parent = nullptr);

    DjEngine* engine() const;
    void setEngine(DjEngine* engine);

    float pixelsPerPoint() const { return m_pixelsPerPoint; }
    void setPixelsPerPoint(float ppp);

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    // Called by the QML FrameAnimation every VSync frame to request a repaint.
    // When isPlaying == false the FrameAnimation stops, so this is never called
    // unnecessarily — exactly what we want for instant-freeze on pause.
    Q_INVOKABLE void requestUpdate() { update(); }

signals:
    void engineChanged();
    void pixelsPerPointChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private slots:
    void onTrackLoaded();
    void onDataUpdated();

private:
    DjEngine* m_engine = nullptr;
    bool m_forceUpdate = false;

    // Zoom level in pixels per data point.
    float m_pixelsPerPoint = 1.5f;

    static constexpr float ZOOM_MIN    = 0.35f;  // matches zoomBase * 1.3^-5
    static constexpr float ZOOM_MAX    = 11.0f;  // matches zoomBase * 1.3^7
    static constexpr float ZOOM_FACTOR = 1.3f;
};
