#pragma once

#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QImage>
#include <QtQml/qqml.h>

#include "deck/DjEngine.h"
#include "TrackData.h"
#include "waveform/WaveformVisualStyle.h"

#include <memory>
#include <array>

namespace brockdj::stems {
struct StemWaveformOverlay;
class StemTrackData;
}
class StemWaveformAnimator;

class OverviewWaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified NOTIFY rectifiedChanged)
    Q_PROPERTY(int renderStyle READ renderStyle WRITE setRenderStyle
               NOTIFY renderStyleChanged)
    Q_PROPERTY(int updateIntervalMs READ updateIntervalMs WRITE setUpdateIntervalMs
               NOTIFY updateIntervalMsChanged)
    Q_PROPERTY(bool resizeDeferred READ resizeDeferred NOTIFY resizeDeferredChanged)
    QML_ELEMENT

public:
    explicit OverviewWaveformItem(QQuickItem* parent = nullptr);

    DjEngine* engine() const { return m_engine; }
    void setEngine(DjEngine* engine);

    bool rectified() const { return m_rectified; }
    void setRectified(bool v);
    int renderStyle() const noexcept { return m_renderStyle; }
    void setRenderStyle(int style);
    int updateIntervalMs() const noexcept { return m_updateIntervalMs; }
    void setUpdateIntervalMs(int intervalMs);
    bool resizeDeferred() const noexcept { return m_resizeDeferred; }

    void paint(QPainter* painter) override;

signals:
    void engineChanged();
    void rectifiedChanged();
    void renderStyleChanged();
    void updateIntervalMsChanged();
    void resizeDeferredChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private slots:
    void onTrackLoaded();
    void onTrackEjected();
    void onRgbDataChanged();      // throttled — analysis progress & waveform updates
    void onHotCuesChanged();      // immediate — cue pin positions must update at once
    void onOverviewRgbUpdated();  // overview ready — repaint immediately
    void refreshStemOverlay();    // stem mutes grey out the inaudible share

private:
    void paintCompactOverview(QPainter* painter,
                              const QVector<TrackData::RgbWaveformFrame>& frames,
                              int drawWidth, int w, int h);
    void paintCompactOverviewLines(QPainter* painter,
                                   const WaveformLineStoreSnapshot& snapshot,
                                   int w, int h);
    void paintPreparedOverviewLines(QPainter* painter, int w, int h);

    // Share of column x (of w) that is still audible with the current stem
    // gains; 1 when stems are not muted or not analysed there yet.
    [[nodiscard]] float stemAudibleRatio(int x, int w) const;
    void paintColumn(QPainter* painter, int x, float baseline, float barH,
                     float energy, float audibleRatio,
                     const waveform_visual::WaveformVisual& visual,
                     float opacityScale) const;

    QPointer<DjEngine> m_engine;
    std::shared_ptr<const brockdj::stems::StemWaveformOverlay> m_stemOverlay;
    StemWaveformAnimator* m_stemAnimator = nullptr;
    mutable std::shared_ptr<const brockdj::stems::StemTrackData> m_stemEnergyData;
    mutable std::uint64_t m_stemEnergyEpoch = 0;
    mutable QVector<std::array<float, 4>> m_stemEnergy;
    QTimer*   m_stemOverlayPoll = nullptr;
    bool      m_rectified      = true;
    int       m_renderStyle = 0;
    QTimer*   m_updateThrottle = nullptr;
    QTimer*   m_resizeThrottle = nullptr;
    int       m_updateIntervalMs = 100;
    bool      m_resizeDeferred = false;
    QImage    m_frameCache;
    QVector<float> m_overviewHeights;
    QVector<float> m_overviewEnergies;
    QVector<waveform_visual::WaveformVisual> m_overviewVisuals;
    std::uint64_t m_lineOverviewTrackGeneration = 0;
    std::uint64_t m_lineOverviewDataGeneration = 0;
    QSize m_lineOverviewSize;
    int m_lineOverviewStyle = -1;
};
