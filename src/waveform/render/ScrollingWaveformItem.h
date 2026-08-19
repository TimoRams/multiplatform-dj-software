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
    Q_PROPERTY(double tempoRatio READ tempoRatio NOTIFY tempoRatioChanged)
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
    [[nodiscard]] double tempoRatio() const noexcept
    {
        return m_tempoRatio.load(std::memory_order_relaxed);
    }
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
    void tempoRatioChanged();
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
    QTimer* m_resizeThrottle = nullptr;
    // Set when a progressive publication arrived while the throttle window was
    // still open, so the trailing edge knows it has work to repaint.
    bool m_pendingDataUpdate = false;
    bool m_resizeDeferred = false;
    std::unique_ptr<waveform_render::WaveformTileRasterizer> m_tileRasterizer;
    std::atomic<bool> m_tilesReady{false};
    bool m_forceRebuild = true;
    float m_pixelsPerPoint = 0.22f;
    // Scene-graph scale belongs to this waveform instance. Keeping a local
    // snapshot prevents another deck's engine state (or a render-thread read
    // during its update) from leaking into this item's horizontal scale.
    std::atomic<double> m_tempoRatio{1.0};
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
    // Frames where the coarse whole-track fallback was deliberately hidden
    // because the current zoom would have magnified it into fake detail.
    std::atomic<std::uint64_t> m_fallbackSuppressedFrameCount{0};
    std::atomic<std::uint64_t> m_viewGeneration{0};
    std::atomic<std::uint64_t> m_trackGeneration{0};
    std::atomic<std::uint64_t> m_zoomTransitionCount{0};
    std::atomic<std::uint64_t> m_staleZoomTilesRejected{0};
    std::atomic<std::uint64_t> m_textureUploadCount{0};
    std::atomic<std::uint64_t> m_textureReplacementCount{0};
    std::atomic<std::uint64_t> m_textureUploadBytes{0};
    std::atomic<std::uint64_t> m_estimatedGpuTextureBytes{0};
    std::atomic<std::uint64_t> m_worstGeometryBuildUsec{0};
    // Whole-updatePaintNode cost and the store-snapshot acquisition inside it.
    // The snapshot figure is the direct read-out of render-thread lock
    // contention: it used to queue behind progressive chunk staging on the
    // coarse TrackData mutex, which is what stalled the UI while scratching.
    std::atomic<std::uint64_t> m_worstPaintNodeUsec{0};
    std::atomic<std::uint64_t> m_worstSnapshotAcquireUsec{0};
    std::atomic<std::uint64_t> m_deferredResizeFrameCount{0};
    std::atomic<std::uint64_t> m_deferredTextureUploadCount{0};
    std::atomic<std::uint64_t> m_textureUploadBudgetBytes{0};
    std::atomic<std::uint64_t> m_textureUploadBudgetIncreases{0};
    std::uint64_t m_textureUploadBudgetBytesPerFrame = 0;
    unsigned int m_underusedTextureBudgetFrames = 0;

    // Keep the item clamp identical to WaveformZoomController::kMinimum.
    static constexpr float kMinimumZoom = 0.0056f;
    static constexpr float kMaximumZoom = 10.0f;
    static constexpr float kZoomFactor = 1.15f;
};
