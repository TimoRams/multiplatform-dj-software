#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QTimer>
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

signals:
    void engineChanged();
    void pixelsPerPointChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private slots:
    void onTrackLoaded();
    void onDataUpdated();
    void onProgressChanged();

private:
    DjEngine* m_engine = nullptr;
    bool m_forceUpdate = false;

    // Continuous render timer: fires regardless of playback state.
    // 8 ms interval (~120 FPS cap); getVisualPosition() interpolates sub-frame.
    QTimer* m_renderTimer = nullptr;

    // Zoom level in pixels per data point.
    // Higher = zoomed in (fewer seconds visible, more detail).
    float m_pixelsPerPoint = 3.0f;

    static constexpr float ZOOM_MIN    = 0.8f;
    static constexpr float ZOOM_MAX    = 12.0f;
    static constexpr float ZOOM_FACTOR = 1.3f;
};
