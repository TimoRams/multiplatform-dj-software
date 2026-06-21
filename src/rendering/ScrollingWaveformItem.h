#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGVertexColorMaterial>
#include <QVariantList>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <array>
#include <vector>
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
    ~ScrollingWaveformItem() override;

    DjEngine* engine() const;
    void setEngine(DjEngine* engine);

    float pixelsPerPoint() const { return m_pixelsPerPoint; }
    void setPixelsPerPoint(float ppp);

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void requestUpdate() { update(); }
    // Returns beat label data for external callers; not used for waveform rendering.
    Q_INVOKABLE QVariantList beatLabels() const;

signals:
    void engineChanged();
    void pixelsPerPointChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private slots:
    void onTrackLoaded();
    void onTrackEjected();
    void onDataUpdated();
    void cleanupSgResources(); // called from render thread via sceneGraphInvalidated

private:
    static float clampToZoomLevel(float ppp);

    QPointer<DjEngine> m_engine;
    bool m_forceUpdate = false;

    // Coalesces frequent data-update signals during progressive analysis so the
    // render thread is not asked to rebuild geometry/textures faster than ~15 fps.
    QTimer* m_dataUpdateThrottle = nullptr;

    float m_pixelsPerPoint = 1.5f;

    mutable QVector<TrackData::RgbWaveformFrame> m_rgbSliceBuf;
    mutable QVector<TrackData::PeakFrame>        m_peakSliceBuf;

    // Render-thread visual stabilizer. Audio/transport remain authoritative;
    // this only absorbs sub-pixel backward timer jitter during continuous playback.
    double m_lastCenterIndexRender = 0.0;
    bool m_hasLastCenterIndexRender = false;

    // Coordinate cache for beatLabels() — render thread writes, main thread reads.
    mutable std::atomic<double> m_lastCenterForBeats{0.0};
    mutable std::atomic<double> m_lastBeatPpp{1.5};
    mutable std::atomic<double> m_lastRenderedWidth{100.0};

    // ── Texture-based overlay (beat labels + cue badges) ──────────────────────
    // All created and destroyed on the render thread.
    struct LabelTex {
        QSGTexture* tex  = nullptr;
        float       logW = 0.0f;
        float       logH = 0.0f;
        bool valid() const { return tex != nullptr; }
    };

    // Text and badge texture cache — keyed by content+style hash.
    QHash<quint64, LabelTex> m_texCache;
    QVector<QSGTexture*>     m_allTextures; // owns every texture for cleanup

    // Reusable QSGSimpleTextureNode pool — avoids per-frame allocation.
    std::vector<QSGSimpleTextureNode*> m_texNodePool;

    // Render-thread helpers. Must only be called from updatePaintNode().
    LabelTex getLabelTex(const QString& text, bool downbeat, double dpr);
    LabelTex getBadgeTex(const QString& text, const QColor& color,
                         float logW, float logH, double dpr);
    // Detach all children of container and return them to the pool.
    void drainToPool(QSGNode* container);
    // Get a node from pool or create a new one.
    QSGSimpleTextureNode* getFromPool();

    static constexpr std::array<float, 20> ZOOM_LEVELS = {
        0.10f, 0.14f, 0.18f, 0.22f, 0.29f, 0.38f, 0.52f, 0.70f, 0.95f,
        1.30f, 1.80f, 2.50f, 3.50f, 5.00f, 7.20f, 10.00f, 14.00f,
        20.00f, 28.00f, 40.00f
    };
};
