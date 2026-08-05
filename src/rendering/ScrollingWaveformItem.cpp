#include "ScrollingWaveformItem.h"

#include "TrackData.h"
#include "WaveformMarkerLayout.h"
#include "waveform/WaveformLineStore.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QMatrix4x4>
#include <QPainter>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr std::size_t kWaveformNodePoolSize = 24;
constexpr std::size_t kCueLabelNodePoolSize = 9;

QSGGeometryNode* makeLineNode(QSGNode* parent)
{
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    // Backend line primitives can be rasterised as one or two pixels depending
    // on the graphics API and DPR. Explicit triangles give us an exact
    // one-physical-pixel rectangle on every backend.
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);

    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending, true);

    auto* node = new QSGGeometryNode();
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    parent->appendChildNode(node);
    return node;
}

QSGGeometryNode* makeTriangleNode(QSGNode* parent)
{
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);

    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending, true);

    auto* node = new QSGGeometryNode();
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    parent->appendChildNode(node);
    return node;
}

void clearGeometry(QSGGeometryNode* node)
{
    if (node->geometry()->vertexCount() == 0)
        return;
    node->geometry()->allocate(0);
    node->markDirty(QSGNode::DirtyGeometry);
}

struct MarkerLine {
    double linePosition = 0.0;
    QColor color;
    float top = 0.0f;
    float bottom = 0.0f;
    float physicalWidth = 1.0f;
};

struct CueLabel {
    double linePosition = 0.0;
    QColor color;
    QString text;
};

struct WaveformSceneNode final : QSGClipNode {
    WaveformSceneNode()
    {
        setIsRectangular(true);
        timeline = new QSGTransformNode();
        appendChildNode(timeline);

        // Translucent overlays sit below the audio lines.
        loopFill = makeTriangleNode(timeline);
        for (auto& node : waveformNodes)
            node = makeLineNode(timeline);
        regularBeats = makeLineNode(timeline);
        downbeats = makeLineNode(timeline);
        loopEdges = makeLineNode(timeline);
        cueLines = makeLineNode(timeline);
    }

    void clearAllGeometry()
    {
        clearGeometry(loopFill);
        for (auto* node : waveformNodes)
            clearGeometry(node);
        clearGeometry(regularBeats);
        clearGeometry(downbeats);
        clearGeometry(loopEdges);
        clearGeometry(cueLines);
        for (auto* label : cueLabels) {
            if (label)
                label->setRect({});
        }
        loopVisible = false;
        loopInLine = std::numeric_limits<double>::quiet_NaN();
        loopOutLine = std::numeric_limits<double>::quiet_NaN();
        loopAppearance = -1;
    }

    QSGTransformNode* timeline = nullptr;
    QSGGeometryNode* loopFill = nullptr;
    std::array<QSGGeometryNode*, kWaveformNodePoolSize> waveformNodes{};
    QSGGeometryNode* regularBeats = nullptr;
    QSGGeometryNode* downbeats = nullptr;
    QSGGeometryNode* loopEdges = nullptr;
    QSGGeometryNode* cueLines = nullptr;
    std::array<QSGSimpleTextureNode*, kCueLabelNodePoolSize> cueLabels{};

    bool hasWindow = false;
    std::uint64_t trackGeneration = 0;
    std::uint64_t dataGeneration = 0;
    std::int64_t windowStartLine = 0;
    std::int64_t windowEndLine = 0;
    double innerStartLine = 0.0;
    double innerEndLine = 0.0;
    double renderOriginLine = 0.0;
    double pixelsPerLine = 0.0;
    double devicePixelRatio = 1.0;
    QSizeF renderedSize;
    QRectF clipBounds;
    bool loopVisible = false;
    double loopInLine = std::numeric_limits<double>::quiet_NaN();
    double loopOutLine = std::numeric_limits<double>::quiet_NaN();
    int loopAppearance = -1;
};

void writeMarkerGeometry(QSGGeometryNode* node,
                         const MarkerLine* lines,
                         std::size_t lineCount,
                         double originLine,
                         double pixelsPerLine,
                         double devicePixelRatio)
{
    auto* geometry = node->geometry();
    geometry->allocate(static_cast<int>(lineCount * 6));
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int out = 0;
    const float halfPixel = static_cast<float>(0.5 / std::max(1.0, devicePixelRatio));
    for (std::size_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        const auto& marker = lines[lineIndex];
        const float x = static_cast<float>(waveform_render::snappedTimelineX(
            marker.linePosition, originLine, pixelsPerLine, devicePixelRatio));
        const uchar r = static_cast<uchar>(marker.color.red());
        const uchar g = static_cast<uchar>(marker.color.green());
        const uchar b = static_cast<uchar>(marker.color.blue());
        const uchar a = static_cast<uchar>(marker.color.alpha());
        const float halfWidth = halfPixel * std::max(1.0f, marker.physicalWidth);
        const float left = x - halfWidth;
        const float right = x + halfWidth;
        vertices[out++].set(left, marker.top, r, g, b, a);
        vertices[out++].set(right, marker.top, r, g, b, a);
        vertices[out++].set(left, marker.bottom, r, g, b, a);
        vertices[out++].set(left, marker.bottom, r, g, b, a);
        vertices[out++].set(right, marker.top, r, g, b, a);
        vertices[out++].set(right, marker.bottom, r, g, b, a);
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

void writeMarkerGeometry(QSGGeometryNode* node,
                         const std::vector<MarkerLine>& lines,
                         double originLine,
                         double pixelsPerLine,
                         double devicePixelRatio)
{
    writeMarkerGeometry(node, lines.data(), lines.size(), originLine,
                        pixelsPerLine, devicePixelRatio);
}

QSGSimpleTextureNode* replaceCueLabelNode(QSGSimpleTextureNode* previous,
                                          QSGNode* parent,
                                          const CueLabel& label,
                                          QQuickWindow* window,
                                          double originLine,
                                          double pixelsPerLine,
                                          double devicePixelRatio)
{
    if (!window) {
        if (previous)
            previous->setRect({});
        return previous;
    }

    constexpr qreal badgeHeight = 16.0;
    constexpr int maximumTextWidth = 86;
    QFont font;
    font.setBold(true);
    font.setPixelSize(9);
    const QFontMetrics metrics(font);
    const QString text = metrics.elidedText(
        label.text.isEmpty() ? QStringLiteral("CUE") : label.text,
        Qt::ElideRight, maximumTextWidth);
    const qreal badgeWidth = std::clamp<qreal>(
        static_cast<qreal>(metrics.horizontalAdvance(text) + 10), 22.0, 96.0);
    const qreal dpr = std::max(1.0, devicePixelRatio);
    QImage image(QSize(static_cast<int>(std::ceil(badgeWidth * dpr)),
                       static_cast<int>(std::ceil(badgeHeight * dpr))),
                 QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor fill = label.color.isValid() ? label.color : QColor(224, 64, 64);
    fill.setAlpha(245);
    painter.setBrush(fill);
    painter.setPen(QPen(QColor(0, 0, 0, 180), 1.0));
    painter.drawRoundedRect(QRectF(0.5, 0.5, badgeWidth - 1.0, badgeHeight - 1.0), 2.0, 2.0);
    const int brightness = (fill.red() * 299 + fill.green() * 587 + fill.blue() * 114) / 1000;
    painter.setPen(brightness < 145 ? QColor(248, 248, 248) : QColor(17, 17, 17));
    painter.setFont(font);
    painter.drawText(QRectF(2.0, 0.0, badgeWidth - 4.0, badgeHeight),
                     Qt::AlignCenter, text);
    painter.end();

    auto* texture = window->createTextureFromImage(image);
    auto* node = new QSGSimpleTextureNode();
    node->setTexture(texture);
    node->setOwnsTexture(true);
    node->setFiltering(QSGTexture::Linear);
    const double x = waveform_render::snappedTimelineX(
        label.linePosition, originLine, pixelsPerLine, devicePixelRatio);
    node->setRect(QRectF(x - badgeWidth * 0.5, 0.0, badgeWidth, badgeHeight));
    if (previous) {
        parent->removeChildNode(previous);
        delete previous;
    }
    parent->appendChildNode(node);
    return node;
}

std::uint64_t writeWaveformChunk(QSGGeometryNode* node,
                                 const WaveformLineChunk& chunk,
                                 std::uint32_t beginLine,
                                 std::uint32_t endLine,
                                 double renderOriginLine,
                                 double pixelsPerLine,
                                 double devicePixelRatio,
                                 float height)
{
    if (!chunk.lines || beginLine >= endLine) {
        clearGeometry(node);
        return 0;
    }

    const double physicalSpacing = pixelsPerLine * std::max(1.0, devicePixelRatio);
    const std::uint32_t step = static_cast<std::uint32_t>(std::max(
        1.0, std::ceil(1.0 / std::max(physicalSpacing, 1.0e-6))));
    const std::uint32_t groupCount = (endLine - beginLine + step - 1) / step;
    auto* geometry = node->geometry();
    geometry->allocate(static_cast<int>(groupCount * 6));
    auto* vertices = geometry->vertexDataAsColoredPoint2D();

    const auto verticalLayout = waveform_render::verticalMarkerLayout(height);
    const float centerY = height * 0.5f;
    const float halfHeight = std::max(
        1.0f, (height - verticalLayout.waveformInset * 2.0f) * 0.5f);
    const float minimumVisibleHeight = static_cast<float>(1.0 / std::max(1.0, devicePixelRatio));
    const float halfPixel = static_cast<float>(0.5 / std::max(1.0, devicePixelRatio));
    std::uint32_t out = 0;

    for (std::uint32_t globalBegin = beginLine; globalBegin < endLine; globalBegin += step) {
        const std::uint32_t globalEnd = std::min(endLine, globalBegin + step);
        std::int16_t minimum = 0;
        std::int16_t maximum = 0;
        std::uint64_t red = 0;
        std::uint64_t green = 0;
        std::uint64_t blue = 0;
        std::uint64_t weight = 0;

        for (std::uint32_t lineIndex = globalBegin; lineIndex < globalEnd; ++lineIndex) {
            const auto local = lineIndex - chunk.firstLineIndex;
            if (local >= chunk.lines->size())
                continue;
            const auto& line = (*chunk.lines)[local];
            minimum = std::min(minimum, line.minimum);
            maximum = std::max(maximum, line.maximum);
            const auto magnitude = static_cast<std::uint32_t>(std::max(
                std::abs(static_cast<int>(line.minimum)),
                std::abs(static_cast<int>(line.maximum))));
            const auto lineWeight = std::max(1u, magnitude);
            red += static_cast<std::uint64_t>(line.red) * lineWeight;
            green += static_cast<std::uint64_t>(line.green) * lineWeight;
            blue += static_cast<std::uint64_t>(line.blue) * lineWeight;
            weight += lineWeight;
        }

        const double centerLine = (static_cast<double>(globalBegin)
            + static_cast<double>(globalEnd - 1)) * 0.5;
        const float x = static_cast<float>(waveform_render::snappedTimelineX(
            centerLine, renderOriginLine, pixelsPerLine, devicePixelRatio));
        float top = centerY - (static_cast<float>(maximum) / 32767.0f) * halfHeight;
        float bottom = centerY - (static_cast<float>(minimum) / 32767.0f) * halfHeight;
        if ((minimum != 0 || maximum != 0) && bottom - top < minimumVisibleHeight) {
            const float middle = (top + bottom) * 0.5f;
            top = middle - minimumVisibleHeight * 0.5f;
            bottom = middle + minimumVisibleHeight * 0.5f;
        }

        const uchar r = static_cast<uchar>(weight > 0 ? red / weight : 150);
        const uchar g = static_cast<uchar>(weight > 0 ? green / weight : 170);
        const uchar b = static_cast<uchar>(weight > 0 ? blue / weight : 190);
        const float left = x - halfPixel;
        const float right = x + halfPixel;
        vertices[out++].set(left, top, r, g, b, 248);
        vertices[out++].set(right, top, r, g, b, 248);
        vertices[out++].set(left, bottom, r, g, b, 248);
        vertices[out++].set(left, bottom, r, g, b, 248);
        vertices[out++].set(right, top, r, g, b, 248);
        vertices[out++].set(right, bottom, r, g, b, 248);
    }

    node->markDirty(QSGNode::DirtyGeometry);
    return groupCount;
}

std::vector<TrackData::BeatMarker> visibleBeatGrid(const TrackData& trackData,
                                                   double leftSec,
                                                   double rightSec)
{
    const auto grid = trackData.getBeatGrid();
    if (!grid.empty()) {
        const auto first = std::lower_bound(grid.cbegin(), grid.cend(), leftSec,
            [](const TrackData::BeatMarker& marker, double second) {
                return marker.positionSec < second;
            });
        const auto last = std::upper_bound(first, grid.cend(), rightSec,
            [](double second, const TrackData::BeatMarker& marker) {
                return second < marker.positionSec;
            });
        return {first, last};
    }

    std::vector<TrackData::BeatMarker> fallback;
    const double bpm = trackData.getBpm();
    const double sampleRate = trackData.getSampleRate();
    if (bpm <= 0.0 || sampleRate <= 0.0)
        return fallback;

    const double firstBeatSec = static_cast<double>(trackData.getFirstBeatSample()) / sampleRate;
    const double beatPeriod = 60.0 / bpm;
    const int firstIndex = std::max(-10000,
        static_cast<int>(std::floor((leftSec - firstBeatSec) / beatPeriod)));
    const int lastIndex = std::min(10000000,
        static_cast<int>(std::ceil((rightSec - firstBeatSec) / beatPeriod)));
    fallback.reserve(static_cast<std::size_t>(std::max(0, lastIndex - firstIndex + 1)));
    for (int index = firstIndex; index <= lastIndex; ++index) {
        const int beatInBar = ((index % 4) + 4) % 4;
        TrackData::BeatMarker marker;
        marker.positionSec = firstBeatSec + static_cast<double>(index) * beatPeriod;
        marker.isDownbeat = beatInBar == 0;
        marker.beatInBar = beatInBar + 1;
        marker.barIndex = static_cast<int>(std::floor(static_cast<double>(index) / 4.0));
        marker.barNumber = marker.barIndex + 1;
        fallback.push_back(marker);
    }
    return fallback;
}

void updateWorst(std::atomic<std::uint64_t>& target, std::uint64_t value)
{
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value
           && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

} // namespace

float ScrollingWaveformItem::clampZoom(float pixelsPerPoint)
{
    if (!std::isfinite(pixelsPerPoint))
        return 0.22f;
    return std::clamp(pixelsPerPoint, kMinimumZoom, kMaximumZoom);
}

ScrollingWaveformItem::ScrollingWaveformItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_dataUpdateThrottle = new QTimer(this);
    m_dataUpdateThrottle->setSingleShot(true);
    // The playhead itself is a VSync transform and stays at full frame rate.
    // Progressive analysis only changes the underlying geometry; rebuilding it
    // at 60 Hz competes with that transform and produces visible frame drops.
    // Coalesce worker bursts to 30 Hz while keeping the first segment responsive.
    m_dataUpdateThrottle->setInterval(33);
    connect(m_dataUpdateThrottle, &QTimer::timeout, this, [this]() {
        // A progressive chunk changes only the audio-line nodes. Rebuilding
        // beat/cue geometry here made the one-pixel beatgrid lines blink while
        // the worker was publishing chunks.
        update();
    });
}

DjEngine* ScrollingWaveformItem::engine() const
{
    return m_engine;
}

void ScrollingWaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine)
        return;
    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);

    m_engine = engine;
    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded,
                this, &ScrollingWaveformItem::onTrackLoaded, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::trackEjected,
                this, &ScrollingWaveformItem::onTrackEjected, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::loopChanged,
                this, &ScrollingWaveformItem::onLoopUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::mainCueChanged,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::tempoChanged,
                this, &ScrollingWaveformItem::invalidateGeometry, Qt::UniqueConnection);
        onTrackLoaded();
    }

    emit engineChanged();
    invalidateGeometry();
}

void ScrollingWaveformItem::setPixelsPerPoint(float pixelsPerPoint)
{
    pixelsPerPoint = clampZoom(pixelsPerPoint);
    if (qFuzzyCompare(m_pixelsPerPoint, pixelsPerPoint))
        return;
    m_pixelsPerPoint = pixelsPerPoint;
    emit pixelsPerPointChanged();
    invalidateGeometry();
}

void ScrollingWaveformItem::zoomIn()
{
    setPixelsPerPoint(m_pixelsPerPoint * kZoomFactor);
}

void ScrollingWaveformItem::zoomOut()
{
    setPixelsPerPoint(m_pixelsPerPoint / kZoomFactor);
}

void ScrollingWaveformItem::invalidateGeometry()
{
    m_forceRebuild = true;
    update();
}

void ScrollingWaveformItem::geometryChange(const QRectF& newGeometry,
                                           const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        invalidateGeometry();
}

void ScrollingWaveformItem::onTrackLoaded()
{
    if (m_engine && m_engine->getTrackData()) {
        auto* trackData = m_engine->getTrackData();
        connect(trackData, &TrackData::dataUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::rgbWaveformUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::peakMipUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::dataCleared,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::beatgridChanged,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::bpmAnalyzed,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
    }
    invalidateGeometry();
}

void ScrollingWaveformItem::onTrackEjected()
{
    if (m_engine && m_engine->getTrackData())
        disconnect(m_engine->getTrackData(), nullptr, this, nullptr);
    invalidateGeometry();
}

void ScrollingWaveformItem::onDataUpdated()
{
    if (!m_dataUpdateThrottle->isActive())
        m_dataUpdateThrottle->start();
}

void ScrollingWaveformItem::onLoopUpdated()
{
    // Loop geometry is maintained independently below updatePaintNode's
    // guarded waveform window. A loop edit must not recreate audio, beatgrid,
    // or cue-label nodes.
    update();
}

void ScrollingWaveformItem::onOverlayUpdated()
{
    invalidateGeometry();
}

QSGNode* ScrollingWaveformItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    auto* scene = static_cast<WaveformSceneNode*>(oldNode);
    if (!scene) {
        scene = new WaveformSceneNode();
        m_sceneGraphNodeCreationCount.fetch_add(
            2 + kWaveformNodePoolSize + 5, std::memory_order_relaxed);
        m_forceRebuild = true;
    }

    const QRectF bounds = boundingRect();
    if (scene->clipBounds != bounds) {
        scene->setClipRect(bounds);
        scene->clipBounds = bounds;
        scene->markDirty(QSGNode::DirtyGeometry);
    }
    m_frameCount.fetch_add(1, std::memory_order_relaxed);

    DjEngine* engine = m_engine.data();
    TrackData* trackData = engine ? engine->getTrackData() : nullptr;
    const auto snapshot = trackData ? trackData->getWaveformLineStoreSnapshot() : nullptr;
    if (!engine || !trackData || !snapshot || snapshot->totalLineCount == 0
        || !snapshot->chunks || bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        if (scene->hasWindow || m_forceRebuild)
            scene->clearAllGeometry();
        scene->hasWindow = false;
        m_forceRebuild = false;
        return scene;
    }

    const double tempoRatio = std::max(0.05, std::abs(engine->getTempoRatio()));
    const double pointsPerCanonicalLine = engine->waveformPointsPerSecond()
        / static_cast<double>(snapshot->linesPerSecond);
    const double pixelsPerLine = static_cast<double>(m_pixelsPerPoint)
        * pointsPerCanonicalLine / tempoRatio;
    const double playheadSec = engine->getVisualPosition();
    const double playheadLine = playheadSec * static_cast<double>(snapshot->linesPerSecond);
    const double dpr = window() ? std::max(1.0, window()->effectiveDevicePixelRatio()) : 1.0;

    m_lastPlayheadSec.store(playheadSec, std::memory_order_relaxed);
    m_lastPixelsPerSecond.store(
        pixelsPerLine * static_cast<double>(snapshot->linesPerSecond),
        std::memory_order_relaxed);
    m_lastRenderedWidth.store(bounds.width(), std::memory_order_relaxed);

    const bool outsideGuard = !scene->hasWindow
        || playheadLine < scene->innerStartLine
        || playheadLine > scene->innerEndLine;
    const bool staticConfigurationChanged = !scene->hasWindow
        || scene->trackGeneration != snapshot->trackGeneration
        || !qFuzzyCompare(scene->pixelsPerLine, pixelsPerLine)
        || !qFuzzyCompare(scene->devicePixelRatio, dpr)
        || scene->renderedSize != bounds.size();
    const bool configurationChanged = staticConfigurationChanged
        || scene->dataGeneration != snapshot->dataGeneration;
    const bool overlayConfigurationChanged = m_forceRebuild
        || staticConfigurationChanged;
    bool renderOriginChanged = false;

    if (m_forceRebuild || outsideGuard || configurationChanged) {
        QElapsedTimer timer;
        timer.start();

        const double visibleLineCount = bounds.width() / std::max(pixelsPerLine, 1.0e-6);
        const double maximumHalfWindow = static_cast<double>(snapshot->chunkSize)
            * static_cast<double>(kWaveformNodePoolSize - 1) * 0.5;
        const double halfWindow = std::max(visibleLineCount * 0.55,
            std::min(visibleLineCount * 1.25, maximumHalfWindow));
        scene->windowStartLine = static_cast<std::int64_t>(std::floor(playheadLine - halfWindow));
        scene->windowEndLine = static_cast<std::int64_t>(std::ceil(playheadLine + halfWindow));
        const bool renderOriginNeedsRebase = !scene->hasWindow
            || staticConfigurationChanged
            || std::abs((playheadLine - scene->renderOriginLine) * pixelsPerLine) > 2'000'000.0;
        if (renderOriginNeedsRebase) {
            scene->renderOriginLine = static_cast<double>(scene->windowStartLine);
            renderOriginChanged = true;
        }
        const double availableGuard = std::max(0.0, halfWindow - visibleLineCount * 0.5);
        const double rebuildTravel = std::max(1.0, availableGuard * 0.58);
        scene->innerStartLine = playheadLine - rebuildTravel;
        scene->innerEndLine = playheadLine + rebuildTravel;

        for (auto* waveformNode : scene->waveformNodes)
            clearGeometry(waveformNode);

        const std::uint32_t sourceBegin = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            scene->windowStartLine, 0, snapshot->totalLineCount));
        const std::uint32_t sourceEnd = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            scene->windowEndLine, 0, snapshot->totalLineCount));
        std::uint64_t renderedLineCount = 0;
        std::uint64_t visibleChunkCount = 0;

        if (sourceBegin < sourceEnd) {
            const std::uint32_t firstChunk = sourceBegin / snapshot->chunkSize;
            const std::uint32_t lastChunk = (sourceEnd - 1) / snapshot->chunkSize;
            std::size_t poolIndex = 0;
            for (std::uint32_t chunkIndex = firstChunk;
                 chunkIndex <= lastChunk && poolIndex < scene->waveformNodes.size();
                 ++chunkIndex) {
                const auto chunk = snapshot->chunkAt(chunkIndex);
                if (!chunk)
                    continue;
                const std::uint32_t begin = std::max(sourceBegin, chunk->firstLineIndex);
                const std::uint32_t end = std::min(sourceEnd,
                    chunk->firstLineIndex + chunk->lineCount);
                renderedLineCount += writeWaveformChunk(
                    scene->waveformNodes[poolIndex++], *chunk, begin, end,
                    scene->renderOriginLine, pixelsPerLine, dpr,
                    static_cast<float>(bounds.height()));
                ++visibleChunkCount;
            }
        }

        // Every timeline layer uses the same persistent origin. Replacing an
        // off-screen waveform chunk therefore cannot change the pixel phase of
        // either the audio columns or an overlay at a guarded window boundary.
        if (m_forceRebuild || staticConfigurationChanged || renderOriginChanged) {
            const double gridStartSec = -10.0;
            const double gridEndSec = std::max(
                gridStartSec + 1.0, static_cast<double>(engine->getDuration()) + 10.0);
            const auto beats = visibleBeatGrid(*trackData, gridStartSec, gridEndSec);
            std::vector<MarkerLine> regularBeats;
            std::vector<MarkerLine> downbeats;
            regularBeats.reserve(beats.size() * 2);
            downbeats.reserve((beats.size() / 4 + 1) * 2);
            const float height = static_cast<float>(bounds.height());
            const auto verticalLayout = waveform_render::verticalMarkerLayout(height);
            const float edgeInset = static_cast<float>(1.0 / dpr);
            for (const auto& beat : beats) {
                const bool isDownbeat = beat.isDownbeat || beat.beatInBar == 1;
                const float tickLength = isDownbeat
                    ? verticalLayout.downbeatTickLength
                    : verticalLayout.regularTickLength;
                const QColor color = isDownbeat
                    ? QColor(242, 62, 72, 220)
                    : QColor(235, 240, 245, 112);
                auto& markers = isDownbeat ? downbeats : regularBeats;
                const double linePosition = beat.positionSec * snapshot->linesPerSecond;
                markers.push_back({linePosition, color,
                                   edgeInset, edgeInset + tickLength});
                markers.push_back({linePosition, color,
                                   height - edgeInset - tickLength, height - edgeInset});
            }
            writeMarkerGeometry(scene->regularBeats, regularBeats,
                                scene->renderOriginLine, pixelsPerLine, dpr);
            writeMarkerGeometry(scene->downbeats, downbeats,
                                scene->renderOriginLine, pixelsPerLine, dpr);

            std::vector<MarkerLine> cueLines;
            std::vector<CueLabel> cueLabels;
            const QVariantList cues = engine->hotCues();
            cueLines.reserve(static_cast<std::size_t>(cues.size() + 1));
            cueLabels.reserve(static_cast<std::size_t>(cues.size() + 1));
            for (const QVariant& cueValue : cues) {
                const QVariantMap cue = cueValue.toMap();
                if (!cue.value(QStringLiteral("set")).toBool())
                    continue;
                QColor color(cue.value(QStringLiteral("color")).toString());
                if (!color.isValid())
                    color = QColor(235, 65, 75);
                color.setAlpha(215);
                const double linePosition = cue.value(QStringLiteral("positionSec")).toDouble()
                    * snapshot->linesPerSecond;
                cueLines.push_back({
                    linePosition, color, 0.0f, static_cast<float>(bounds.height()),
                    verticalLayout.cueLinePhysicalWidth});
                QString label = cue.value(QStringLiteral("label")).toString().trimmed();
                if (label.isEmpty())
                    label = QStringLiteral("HOT CUE %1")
                        .arg(cue.value(QStringLiteral("index")).toInt() + 1);
                cueLabels.push_back({linePosition, color, label});
            }
            if (engine->mainCueSec() >= 0.0) {
                const double linePosition = engine->mainCueSec() * snapshot->linesPerSecond;
                const QColor color(255, 70, 78, 210);
                cueLines.push_back({linePosition, color, 0.0f,
                                    static_cast<float>(bounds.height()),
                                    verticalLayout.cueLinePhysicalWidth});
                cueLabels.push_back({linePosition, color, QStringLiteral("CUE")});
            }
            writeMarkerGeometry(scene->cueLines, cueLines,
                                scene->renderOriginLine, pixelsPerLine, dpr);
            std::size_t labelIndex = 0;
            for (; labelIndex < cueLabels.size()
                   && labelIndex < scene->cueLabels.size(); ++labelIndex) {
                scene->cueLabels[labelIndex] = replaceCueLabelNode(
                    scene->cueLabels[labelIndex], scene->timeline,
                    cueLabels[labelIndex], window(), scene->renderOriginLine,
                    pixelsPerLine, dpr);
                m_sceneGraphNodeCreationCount.fetch_add(1, std::memory_order_relaxed);
            }
            for (; labelIndex < scene->cueLabels.size(); ++labelIndex) {
                if (!scene->cueLabels[labelIndex])
                    continue;
                scene->timeline->removeChildNode(scene->cueLabels[labelIndex]);
                delete scene->cueLabels[labelIndex];
                scene->cueLabels[labelIndex] = nullptr;
            }
        }

        scene->trackGeneration = snapshot->trackGeneration;
        scene->dataGeneration = snapshot->dataGeneration;
        scene->pixelsPerLine = pixelsPerLine;
        scene->devicePixelRatio = dpr;
        scene->renderedSize = bounds.size();
        scene->hasWindow = true;
        m_forceRebuild = false;

        m_geometryRebuildCount.fetch_add(1, std::memory_order_relaxed);
        m_lastRenderedLineCount.store(renderedLineCount, std::memory_order_relaxed);
        m_lastVisibleChunkCount.store(visibleChunkCount, std::memory_order_relaxed);
        updateWorst(m_worstGeometryBuildUsec,
                    static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000));
    }

    const bool loopComplete = engine->loopOutPosition() > engine->loopInPosition();
    const double visualLoopOut = loopComplete
        ? engine->loopOutPosition()
        : engine->loopPreviewOutPosition();
    const bool loopVisible = engine->loopInSet()
        && visualLoopOut > engine->loopInPosition();
    const int loopAppearance = engine->loopActive() && loopComplete
        ? 2
        : (loopComplete ? 0 : 1);
    const double loopInLine = engine->loopInPosition() * snapshot->linesPerSecond;
    const double loopOutLine = visualLoopOut * snapshot->linesPerSecond;
    const bool loopGeometryChanged = overlayConfigurationChanged
        || scene->loopVisible != loopVisible
        || scene->loopAppearance != loopAppearance
        || !qFuzzyCompare(scene->loopInLine, loopInLine)
        || !qFuzzyCompare(scene->loopOutLine, loopOutLine);

    if (loopGeometryChanged) {
        auto* loopGeometry = scene->loopFill->geometry();
        std::array<MarkerLine, 2> loopEdges {};
        std::size_t loopEdgeCount = 0;
        if (loopVisible) {
            const int fillAlpha = loopAppearance == 2 ? 22 : (loopAppearance == 1 ? 14 : 9);
            const int edgeAlpha = loopAppearance == 2 ? 190 : (loopAppearance == 1 ? 150 : 95);
            const float x0 = static_cast<float>(waveform_render::snappedTimelineX(
                loopInLine, scene->renderOriginLine, pixelsPerLine, dpr));
            const float x1 = static_cast<float>(waveform_render::snappedTimelineX(
                loopOutLine, scene->renderOriginLine, pixelsPerLine, dpr));
            loopGeometry->allocate(4);
            auto* vertices = loopGeometry->vertexDataAsColoredPoint2D();
            const float h = static_cast<float>(bounds.height());
            vertices[0].set(x0, 0.0f, 70, 190, 255, fillAlpha);
            vertices[1].set(x0, h, 70, 190, 255, fillAlpha);
            vertices[2].set(x1, 0.0f, 70, 190, 255, fillAlpha);
            vertices[3].set(x1, h, 70, 190, 255, fillAlpha);
            loopEdges[loopEdgeCount++] = {
                loopInLine, QColor(70, 190, 255, edgeAlpha), 0.0f, h};
            loopEdges[loopEdgeCount++] = {
                loopOutLine, QColor(70, 190, 255, edgeAlpha), 0.0f, h};
        } else {
            loopGeometry->allocate(0);
        }
        scene->loopFill->markDirty(QSGNode::DirtyGeometry);
        writeMarkerGeometry(scene->loopEdges, loopEdges.data(), loopEdgeCount,
                            scene->renderOriginLine, pixelsPerLine, dpr);
        scene->loopVisible = loopVisible;
        scene->loopInLine = loopInLine;
        scene->loopOutLine = loopOutLine;
        scene->loopAppearance = loopAppearance;
    }

    const double pixelCenteredTranslation = waveform_render::snappedTimelineTranslation(
        bounds.width(), playheadLine, scene->renderOriginLine, pixelsPerLine, dpr);
    QMatrix4x4 transform;
    transform.translate(static_cast<float>(pixelCenteredTranslation), 0.0f);
    scene->timeline->setMatrix(transform);
    scene->timeline->markDirty(QSGNode::DirtyMatrix);
    m_transformUpdateCount.fetch_add(1, std::memory_order_relaxed);
    return scene;
}

QVariantList ScrollingWaveformItem::beatLabels() const
{
    QVariantList result;
    if (!m_engine || !m_engine->getTrackData())
        return result;

    const double width = m_lastRenderedWidth.load(std::memory_order_relaxed);
    const double pixelsPerSecond = m_lastPixelsPerSecond.load(std::memory_order_relaxed);
    const double playheadSec = m_lastPlayheadSec.load(std::memory_order_relaxed);
    if (width <= 0.0 || pixelsPerSecond <= 0.0)
        return result;

    const double halfVisibleSec = width * 0.5 / pixelsPerSecond;
    const auto beats = visibleBeatGrid(*m_engine->getTrackData(),
                                       playheadSec - halfVisibleSec,
                                       playheadSec + halfVisibleSec);
    double previousX = -std::numeric_limits<double>::infinity();
    for (const auto& beat : beats) {
        const double x = width * 0.5 + (beat.positionSec - playheadSec) * pixelsPerSecond;
        if (x - previousX < 20.0)
            continue;
        previousX = x;
        QVariantMap marker;
        marker.insert(QStringLiteral("x"), x);
        marker.insert(QStringLiteral("text"),
                      beat.isDownbeat ? QString::number(beat.barNumber)
                                      : QString::number(beat.beatInBar));
        marker.insert(QStringLiteral("isDownbeat"), beat.isDownbeat);
        result.push_back(marker);
    }
    return result;
}

QVariantMap ScrollingWaveformItem::renderStats() const
{
    QVariantMap stats;
    stats.insert(QStringLiteral("frames"),
                 QVariant::fromValue<qulonglong>(m_frameCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("geometryRebuilds"),
                 QVariant::fromValue<qulonglong>(m_geometryRebuildCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("transformUpdates"),
                 QVariant::fromValue<qulonglong>(m_transformUpdateCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("sceneGraphNodeCreations"),
                 QVariant::fromValue<qulonglong>(m_sceneGraphNodeCreationCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("renderedLines"),
                 QVariant::fromValue<qulonglong>(m_lastRenderedLineCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("visibleChunks"),
                 QVariant::fromValue<qulonglong>(m_lastVisibleChunkCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("chunkPoolCapacity"),
                 static_cast<qulonglong>(kWaveformNodePoolSize));
    stats.insert(QStringLiteral("worstGeometryBuildUsec"),
                 QVariant::fromValue<qulonglong>(m_worstGeometryBuildUsec.load(std::memory_order_relaxed)));
    return stats;
}

void ScrollingWaveformItem::resetRenderStats()
{
    m_frameCount.store(0, std::memory_order_relaxed);
    m_geometryRebuildCount.store(0, std::memory_order_relaxed);
    m_transformUpdateCount.store(0, std::memory_order_relaxed);
    m_sceneGraphNodeCreationCount.store(0, std::memory_order_relaxed);
    m_lastRenderedLineCount.store(0, std::memory_order_relaxed);
    m_lastVisibleChunkCount.store(0, std::memory_order_relaxed);
    m_worstGeometryBuildUsec.store(0, std::memory_order_relaxed);
}
