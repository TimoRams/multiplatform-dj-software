#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <array>
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
    static float clampToZoomLevel(float ppp);

    DjEngine* m_engine = nullptr;
    bool m_forceUpdate = false;

    // Zoom level in pixels per data point.
    float m_pixelsPerPoint = 1.5f;

    // Oscillation mode (actual PCM waveform shape) activates at 3.50 px/pt and above.
    // At that zoom, each peak-mip bin (4× analysis rate) is ≥ 0.875 px wide — enough
    // to show the actual audio oscillation envelope via Catmull-Rom interpolation.
    static constexpr float kOscilloscopeThreshold = 3.50f;

    static constexpr std::array<float, 20> ZOOM_LEVELS = {
        0.10f, 0.14f, 0.18f, 0.22f, 0.29f, 0.38f, 0.52f, 0.70f, 0.95f,
        1.30f, 1.80f, 2.50f, 3.50f, 5.00f, 7.20f, 10.00f, 14.00f,
        20.00f, 28.00f, 40.00f
    };
};
