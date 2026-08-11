#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <memory>

#include "DjEngine.h"

namespace waveform_render {
class WaveformTileRasterizer;
}

class ScrollingWaveformItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(float pixelsPerPoint READ pixelsPerPoint WRITE setPixelsPerPoint NOTIFY pixelsPerPointChanged)
    QML_ELEMENT

public:
    explicit ScrollingWaveformItem(QQuickItem* parent = nullptr);
    ~ScrollingWaveformItem() override;

    [[nodiscard]] DjEngine* engine() const;
    void setEngine(DjEngine* engine);

    [[nodiscard]] float pixelsPerPoint() const noexcept { return m_pixelsPerPoint; }
    void setPixelsPerPoint(float pixelsPerPoint);

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    // Playback calls this once per frame. It only schedules a scene-graph sync;
    // stable waveform geometry remains untouched.
    Q_INVOKABLE void requestUpdate() { update(); }
    // Layout/overlay changes explicitly invalidate the guarded render window.
    Q_INVOKABLE void invalidateGeometry();
    Q_INVOKABLE QVariantList beatLabels() const;
    Q_INVOKABLE QVariantMap renderStats() const;
    Q_INVOKABLE void resetRenderStats();

signals:
    void engineChanged();
    void pixelsPerPointChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private slots:
    void onTrackLoaded();
    void onTrackEjected();
    void onDataUpdated();
    void onLoopUpdated();
    void onOverlayUpdated();

private:
    static float clampZoom(float pixelsPerPoint);

    QPointer<DjEngine> m_engine;
    QTimer* m_dataUpdateThrottle = nullptr;
    std::unique_ptr<waveform_render::WaveformTileRasterizer> m_tileRasterizer;
    std::atomic<bool> m_tilesReady{false};
    bool m_forceRebuild = true;
    float m_pixelsPerPoint = 0.22f;

    mutable std::atomic<double> m_lastPlayheadSec{0.0};
    mutable std::atomic<double> m_lastPixelsPerSecond{1.0};
    mutable std::atomic<double> m_lastRenderedWidth{0.0};

    std::atomic<std::uint64_t> m_frameCount{0};
    std::atomic<std::uint64_t> m_geometryRebuildCount{0};
    std::atomic<std::uint64_t> m_transformUpdateCount{0};
    std::atomic<std::uint64_t> m_sceneGraphNodeCreationCount{0};
    std::atomic<std::uint64_t> m_lastRenderedLineCount{0};
    std::atomic<std::uint64_t> m_lastVisibleChunkCount{0};
    std::atomic<std::uint64_t> m_worstGeometryBuildUsec{0};

    static constexpr float kMinimumZoom = 0.08f;
    static constexpr float kMaximumZoom = 10.0f;
    static constexpr float kZoomFactor = 1.15f;
};
