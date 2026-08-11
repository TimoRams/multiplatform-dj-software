#pragma once

#include <QPointer>
#include <QColor>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "deck/DjEngine.h"
#include "waveform/WaveformDemand.h"

namespace waveform_render {
class WaveformTileRasterizer;
}

class ScrollingWaveformItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(float pixelsPerPoint READ pixelsPerPoint WRITE setPixelsPerPoint NOTIFY pixelsPerPointChanged)
    Q_PROPERTY(double effectivePixelsPerSecond READ effectivePixelsPerSecond
               NOTIFY effectivePixelsPerSecondChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor
               NOTIFY backgroundColorChanged)
    QML_ELEMENT

public:
    explicit ScrollingWaveformItem(QQuickItem* parent = nullptr);
    ~ScrollingWaveformItem() override;

    [[nodiscard]] DjEngine* engine() const;
    void setEngine(DjEngine* engine);

    [[nodiscard]] float pixelsPerPoint() const noexcept { return m_pixelsPerPoint; }
    void setPixelsPerPoint(float pixelsPerPoint);
    [[nodiscard]] double effectivePixelsPerSecond() const noexcept;
    [[nodiscard]] QColor backgroundColor() const noexcept { return m_backgroundColor; }
    void setBackgroundColor(const QColor& color);
    Q_INVOKABLE double screenDeltaToSeconds(double screenDelta) const noexcept;
    Q_INVOKABLE double timelineSecondsAtX(double screenX,
                                          double playheadSeconds) const noexcept;

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    // Playback calls this once per frame. It only schedules a scene-graph sync;
    // stable waveform geometry remains untouched.
    Q_INVOKABLE void requestUpdate();
    // Layout/overlay changes explicitly invalidate the guarded render window.
    Q_INVOKABLE void invalidateGeometry();
    Q_INVOKABLE QVariantList beatLabels() const;
    Q_INVOKABLE QVariantMap renderStats() const;
    Q_INVOKABLE void resetRenderStats();

signals:
    void engineChanged();
    void pixelsPerPointChanged();
    void effectivePixelsPerSecondChanged();
    void backgroundColorChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private slots:
    void onTrackLoaded();
    void onTrackEjected();
    void onDataUpdated();
    void onLoopUpdated();
    void onOverlayUpdated();
    void onTimelineScaleChanged();

private:
    static float clampZoom(float pixelsPerPoint);

    QPointer<DjEngine> m_engine;
    QTimer* m_dataUpdateThrottle = nullptr;
    std::unique_ptr<waveform_render::WaveformTileRasterizer> m_tileRasterizer;
    std::atomic<bool> m_tilesReady{false};
    bool m_forceRebuild = true;
    float m_pixelsPerPoint = 0.22f;
    QColor m_backgroundColor{16, 17, 20};
    std::optional<waveform::WaveformDemand> m_lastPublishedDemand;

    void publishViewportDemand();

    mutable std::atomic<double> m_lastPlayheadSec{0.0};
    mutable std::atomic<double> m_lastPixelsPerSecond{1.0};
    mutable std::atomic<double> m_lastRenderedWidth{0.0};

    std::atomic<std::uint64_t> m_frameCount{0};
    std::atomic<std::uint64_t> m_geometryRebuildCount{0};
    std::atomic<std::uint64_t> m_transformUpdateCount{0};
    std::atomic<std::uint64_t> m_sceneGraphNodeCreationCount{0};
    std::atomic<std::uint64_t> m_lastRenderedLineCount{0};
    std::atomic<std::uint64_t> m_lastVisibleChunkCount{0};
    std::atomic<std::uint64_t> m_incompleteTileRejectedCount{0};
    std::atomic<std::uint64_t> m_readyVisibleTileCount{0};
    std::atomic<std::uint64_t> m_missingVisibleTileCount{0};
    std::atomic<std::uint64_t> m_detailCoveragePermille{0};
    std::atomic<std::uint64_t> m_overviewFallbackFrameCount{0};
    std::atomic<std::uint64_t> m_viewGeneration{0};
    std::atomic<std::uint64_t> m_trackGeneration{0};
    std::atomic<std::uint64_t> m_zoomTransitionCount{0};
    std::atomic<std::uint64_t> m_staleZoomTilesRejected{0};
    std::atomic<std::uint64_t> m_textureUploadCount{0};
    std::atomic<std::uint64_t> m_textureReplacementCount{0};
    std::atomic<std::uint64_t> m_textureUploadBytes{0};
    std::atomic<std::uint64_t> m_estimatedGpuTextureBytes{0};
    std::atomic<std::uint64_t> m_worstGeometryBuildUsec{0};

    // Three more zoomOut() steps below the previous 0.08f floor (0.08 / 1.15^3).
    static constexpr float kMinimumZoom = 0.0526f;
    static constexpr float kMaximumZoom = 10.0f;
    static constexpr float kZoomFactor = 1.15f;
};
