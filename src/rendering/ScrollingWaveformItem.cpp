#include "ScrollingWaveformItem.h"

#include "TrackData.h"
#include "waveform/WaveformLineStore.h"

#include <QColor>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGGeometryNode>
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

QSGGeometryNode* makeLineNode(QSGNode* parent, float lineWidth = 1.0f)
{
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawLines);
    geometry->setLineWidth(lineWidth);

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
    }

    QSGTransformNode* timeline = nullptr;
    QSGGeometryNode* loopFill = nullptr;
    std::array<QSGGeometryNode*, kWaveformNodePoolSize> waveformNodes{};
    QSGGeometryNode* regularBeats = nullptr;
    QSGGeometryNode* downbeats = nullptr;
    QSGGeometryNode* loopEdges = nullptr;
    QSGGeometryNode* cueLines = nullptr;

    bool hasWindow = false;
    std::uint64_t trackGeneration = 0;
    std::uint64_t dataGeneration = 0;
    std::int64_t windowStartLine = 0;
    std::int64_t windowEndLine = 0;
    double innerStartLine = 0.0;
    double innerEndLine = 0.0;
    double pixelsPerLine = 0.0;
    double devicePixelRatio = 1.0;
    QSizeF renderedSize;
    QRectF clipBounds;
};

void writeMarkerGeometry(QSGGeometryNode* node,
                         const std::vector<MarkerLine>& lines,
                         std::int64_t windowStartLine,
                         double pixelsPerLine)
{
    auto* geometry = node->geometry();
    geometry->allocate(static_cast<int>(lines.size() * 2));
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int out = 0;
    for (const auto& marker : lines) {
        const float x = static_cast<float>(
            (marker.linePosition - static_cast<double>(windowStartLine)) * pixelsPerLine);
        vertices[out++].set(x, marker.top,
                            static_cast<uchar>(marker.color.red()),
                            static_cast<uchar>(marker.color.green()),
                            static_cast<uchar>(marker.color.blue()),
                            static_cast<uchar>(marker.color.alpha()));
        vertices[out++].set(x, marker.bottom,
                            static_cast<uchar>(marker.color.red()),
                            static_cast<uchar>(marker.color.green()),
                            static_cast<uchar>(marker.color.blue()),
                            static_cast<uchar>(marker.color.alpha()));
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

std::uint64_t writeWaveformChunk(QSGGeometryNode* node,
                                 const WaveformLineChunk& chunk,
                                 std::uint32_t beginLine,
                                 std::uint32_t endLine,
                                 std::int64_t windowStartLine,
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
    geometry->allocate(static_cast<int>(groupCount * 2));
    auto* vertices = geometry->vertexDataAsColoredPoint2D();

    const float centerY = height * 0.5f;
    const float halfHeight = std::max(1.0f, height * 0.48f);
    const float minimumVisibleHeight = static_cast<float>(1.0 / std::max(1.0, devicePixelRatio));
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
        const float x = static_cast<float>(
            (centerLine - static_cast<double>(windowStartLine)) * pixelsPerLine);
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
        vertices[out++].set(x, top, r, g, b, 248);
        vertices[out++].set(x, bottom, r, g, b, 248);
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
        return 1.5f;
    return std::clamp(pixelsPerPoint, kMinimumZoom, kMaximumZoom);
}

ScrollingWaveformItem::ScrollingWaveformItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_dataUpdateThrottle = new QTimer(this);
    m_dataUpdateThrottle->setSingleShot(true);
    m_dataUpdateThrottle->setInterval(66);
    connect(m_dataUpdateThrottle, &QTimer::timeout, this, [this]() {
        invalidateGeometry();
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
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::mainCueChanged,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
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
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::bpmAnalyzed,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
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
    const bool configurationChanged = !scene->hasWindow
        || scene->trackGeneration != snapshot->trackGeneration
        || scene->dataGeneration != snapshot->dataGeneration
        || !qFuzzyCompare(scene->pixelsPerLine, pixelsPerLine)
        || !qFuzzyCompare(scene->devicePixelRatio, dpr)
        || scene->renderedSize != bounds.size();

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
                    scene->windowStartLine, pixelsPerLine, dpr,
                    static_cast<float>(bounds.height()));
                ++visibleChunkCount;
            }
        }

        const double leftSec = static_cast<double>(scene->windowStartLine)
            / static_cast<double>(snapshot->linesPerSecond);
        const double rightSec = static_cast<double>(scene->windowEndLine)
            / static_cast<double>(snapshot->linesPerSecond);
        const auto beats = visibleBeatGrid(*trackData, leftSec, rightSec);
        std::vector<MarkerLine> regularBeats;
        std::vector<MarkerLine> downbeats;
        regularBeats.reserve(beats.size());
        downbeats.reserve(beats.size() / 4 + 1);
        for (const auto& beat : beats) {
            MarkerLine marker;
            marker.linePosition = beat.positionSec * snapshot->linesPerSecond;
            marker.top = 0.0f;
            marker.bottom = static_cast<float>(bounds.height());
            if (beat.isDownbeat || beat.beatInBar == 1) {
                marker.color = QColor(255, 255, 255, 82);
                downbeats.push_back(marker);
            } else {
                marker.color = QColor(255, 255, 255, 34);
                regularBeats.push_back(marker);
            }
        }
        writeMarkerGeometry(scene->regularBeats, regularBeats,
                            scene->windowStartLine, pixelsPerLine);
        writeMarkerGeometry(scene->downbeats, downbeats,
                            scene->windowStartLine, pixelsPerLine);

        auto* loopGeometry = scene->loopFill->geometry();
        std::vector<MarkerLine> loopEdges;
        if (engine->loopActive() && engine->loopOutPosition() > engine->loopInPosition()) {
            const double loopInLine = engine->loopInPosition() * snapshot->linesPerSecond;
            const double loopOutLine = engine->loopOutPosition() * snapshot->linesPerSecond;
            const float x0 = static_cast<float>(
                (loopInLine - scene->windowStartLine) * pixelsPerLine);
            const float x1 = static_cast<float>(
                (loopOutLine - scene->windowStartLine) * pixelsPerLine);
            loopGeometry->allocate(4);
            auto* vertices = loopGeometry->vertexDataAsColoredPoint2D();
            const float h = static_cast<float>(bounds.height());
            vertices[0].set(x0, 0.0f, 70, 190, 255, 22);
            vertices[1].set(x0, h, 70, 190, 255, 22);
            vertices[2].set(x1, 0.0f, 70, 190, 255, 22);
            vertices[3].set(x1, h, 70, 190, 255, 22);
            loopEdges.push_back({loopInLine, QColor(70, 190, 255, 190), 0.0f, h});
            loopEdges.push_back({loopOutLine, QColor(70, 190, 255, 190), 0.0f, h});
        } else {
            loopGeometry->allocate(0);
        }
        scene->loopFill->markDirty(QSGNode::DirtyGeometry);
        writeMarkerGeometry(scene->loopEdges, loopEdges,
                            scene->windowStartLine, pixelsPerLine);

        std::vector<MarkerLine> cueLines;
        const QVariantList cues = engine->hotCues();
        cueLines.reserve(static_cast<std::size_t>(cues.size() + 1));
        for (const QVariant& cueValue : cues) {
            const QVariantMap cue = cueValue.toMap();
            if (!cue.value(QStringLiteral("set")).toBool())
                continue;
            QColor color(cue.value(QStringLiteral("color")).toString());
            if (!color.isValid())
                color = QColor(235, 65, 75);
            color.setAlpha(215);
            cueLines.push_back({
                cue.value(QStringLiteral("positionSec")).toDouble()
                    * snapshot->linesPerSecond,
                color, 0.0f, static_cast<float>(bounds.height())});
        }
        if (engine->mainCueSec() >= 0.0) {
            cueLines.push_back({engine->mainCueSec() * snapshot->linesPerSecond,
                                QColor(255, 70, 78, 210), 0.0f,
                                static_cast<float>(bounds.height())});
        }
        writeMarkerGeometry(scene->cueLines, cueLines,
                            scene->windowStartLine, pixelsPerLine);

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

    const double translationX = bounds.width() * 0.5
        - (playheadLine - static_cast<double>(scene->windowStartLine)) * pixelsPerLine;
    QMatrix4x4 transform;
    transform.translate(static_cast<float>(translationX), 0.0f);
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
