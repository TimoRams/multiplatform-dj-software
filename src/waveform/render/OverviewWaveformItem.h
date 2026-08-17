#pragma once

#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QImage>
#include <QtQml/qqml.h>

#include "deck/DjEngine.h"
#include "TrackData.h"

class OverviewWaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(DjEngine* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified NOTIFY rectifiedChanged)
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
    int updateIntervalMs() const noexcept { return m_updateIntervalMs; }
    void setUpdateIntervalMs(int intervalMs);
    bool resizeDeferred() const noexcept { return m_resizeDeferred; }

    void paint(QPainter* painter) override;

signals:
    void engineChanged();
    void rectifiedChanged();
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

private:
    void paintCompactOverview(QPainter* painter,
                              const QVector<TrackData::RgbWaveformFrame>& frames,
                              int drawWidth, int w, int h);
    void paintCompactOverviewLines(QPainter* painter,
                                   const WaveformLineStoreSnapshot& snapshot,
                                   int w, int h);

    QPointer<DjEngine> m_engine;
    bool      m_rectified      = true;
    QTimer*   m_updateThrottle = nullptr;
    QTimer*   m_resizeThrottle = nullptr;
    int       m_updateIntervalMs = 100;
    bool      m_resizeDeferred = false;
    QImage    m_frameCache;
    QVector<float> m_overviewHeights;
    QVector<QColor> m_overviewColors;
};
