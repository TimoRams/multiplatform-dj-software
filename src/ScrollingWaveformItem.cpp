#include "ScrollingWaveformItem.h"
#include <QDebug>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <cmath>

ScrollingWaveformItem::ScrollingWaveformItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);

    // The render timer fires continuously once a track is loaded,
    // independent of playback state. update() is cheap when nothing changed.
    m_renderTimer = new QTimer(this);
    m_renderTimer->setInterval(8);
    connect(m_renderTimer, &QTimer::timeout, this, [this]() { update(); });
    // Timer starts in onTrackLoaded()
}

DjEngine* ScrollingWaveformItem::engine() const
{
    return m_engine;
}

void ScrollingWaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine) return;

    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }
    m_engine = engine;
    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded, this, &ScrollingWaveformItem::onTrackLoaded);
        connect(m_engine, &DjEngine::progressChanged, this, &ScrollingWaveformItem::onProgressChanged);
    } else {
        m_renderTimer->stop();
    }
    emit engineChanged();
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::setPixelsPerPoint(float ppp)
{
    ppp = std::max(ZOOM_MIN, std::min(ZOOM_MAX, ppp));
    if (qFuzzyCompare(m_pixelsPerPoint, ppp)) return;
    m_pixelsPerPoint = ppp;
    emit pixelsPerPointChanged();
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::zoomIn()
{
    setPixelsPerPoint(m_pixelsPerPoint * ZOOM_FACTOR);
}

void ScrollingWaveformItem::zoomOut()
{
    setPixelsPerPoint(m_pixelsPerPoint / ZOOM_FACTOR);
}

void ScrollingWaveformItem::onTrackLoaded()
{
    if (m_engine && m_engine->getTrackData()) {
        connect(m_engine->getTrackData(), &TrackData::dataUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::dataCleared, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::bpmAnalyzed, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
    }
    m_renderTimer->start();
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::onDataUpdated()
{
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::onProgressChanged()
{
    // The render timer calls update() on its own; nothing needed here.
}

QSGNode* ScrollingWaveformItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    if (!m_engine || !m_engine->getTrackData()) {
        if (oldNode) delete oldNode;
        return nullptr;
    }

    QVector<TrackData::FrequencyData> allData = m_engine->getTrackData()->getWaveformData();

    if (allData.isEmpty()) {
        if (oldNode) delete oldNode;
        return nullptr;
    }

    // Scene graph node order (back to front):
    //   0: lowPeakNode - blue halo    (bass, base layer)
    //   1: lowRmsNode  - bright blue body (bass)
    //   2: midNode     - orange strip (mids)
    //   3: highNode    - white strip  (highs)
    //   4: beatNode    - beat grid lines (on top)
    QSGNode* rootNode = oldNode;
    if (!rootNode) {
        rootNode = new QSGNode();

        auto makeStrip = [](QSGNode* parent) -> QSGGeometryNode* {
            auto* node = new QSGGeometryNode();
            auto* geo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            geo->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            node->setGeometry(geo);
            node->setFlag(QSGNode::OwnsGeometry);
            node->setMaterial(new QSGVertexColorMaterial());
            node->setFlag(QSGNode::OwnsMaterial);
            parent->appendChildNode(node);
            return node;
        };

        makeStrip(rootNode); // 0: lowPeak
        makeStrip(rootNode); // 1: lowRms
        makeStrip(rootNode); // 2: mid
        makeStrip(rootNode); // 3: high

        // 4: beatNode (DrawLines)
        auto* beatNode = new QSGGeometryNode();
        auto* beatGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        beatGeo->setDrawingMode(QSGGeometry::DrawLines);
        beatGeo->setLineWidth(1.0f);
        beatNode->setGeometry(beatGeo);
        beatNode->setFlag(QSGNode::OwnsGeometry);
        beatNode->setMaterial(new QSGVertexColorMaterial());
        beatNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(beatNode);
    }

    auto* lowPeakNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowRmsNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* midNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* highNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* beatNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));

    int wInt = static_cast<int>(width());
    if (wInt <= 0) return rootNode;

    lowPeakNode->geometry()->allocate(wInt * 2);
    lowRmsNode ->geometry()->allocate(wInt * 2);
    midNode    ->geometry()->allocate(wInt * 2);
    highNode   ->geometry()->allocate(wInt * 2);

    auto* lowPeakV = lowPeakNode->geometry()->vertexDataAsColoredPoint2D();
    auto* lowRmsV  = lowRmsNode ->geometry()->vertexDataAsColoredPoint2D();
    auto* midV     = midNode    ->geometry()->vertexDataAsColoredPoint2D();
    auto* highV    = highNode   ->geometry()->vertexDataAsColoredPoint2D();

    const float w             = static_cast<float>(wInt);
    const float midY          = static_cast<float>(height()) / 2.0f;
    const float pointsPerSec  = 150.0f;
    const float pixelsPerPoint = m_pixelsPerPoint;
    const float centerIndexReal = m_engine->getVisualPosition() * pointsPerSec;

    // Catmull-Rom Spline
    auto catmull = [](float p0, float p1, float p2, float p3, float t) {
        float v = 0.5f * ((2.0f*p1) + (-p0+p2)*t
                          + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3)*t*t
                          + (-p0 + 3.0f*p1 - 3.0f*p2 + p3)*t*t*t);
        return std::max(0.0f, v);
    };

    const TrackData::FrequencyData zeroFD{};
    auto getD = [&](int idx) -> const TrackData::FrequencyData& {
        if (idx < 0 || idx >= allData.size()) return zeroFD;
        return allData[idx];
    };

    // Step 1: Catmull-Rom interpolation per output pixel.
    struct ScrollPixel {
        float lowPeak = 0.0f, lowRms  = 0.0f;
        float midPeak = 0.0f, midRms  = 0.0f;
        float highPeak = 0.0f, highRms = 0.0f;
    };
    std::vector<ScrollPixel> pixels(wInt);

    for (int x = 0; x < wInt; ++x) {
        float dataPos = centerIndexReal + (static_cast<float>(x) - w * 0.5f) / pixelsPerPoint;
        int   i0      = static_cast<int>(std::floor(dataPos)) - 1;
        float t       = dataPos - std::floor(dataPos);

        const auto& d0 = getD(i0);
        const auto& d1 = getD(i0+1);
        const auto& d2 = getD(i0+2);
        const auto& d3 = getD(i0+3);

        pixels[x].lowPeak  = catmull(d0.lowPeak,  d1.lowPeak,  d2.lowPeak,  d3.lowPeak,  t);
        pixels[x].lowRms   = catmull(d0.lowRms,   d1.lowRms,   d2.lowRms,   d3.lowRms,   t);
        pixels[x].midPeak  = catmull(d0.midPeak,  d1.midPeak,  d2.midPeak,  d3.midPeak,  t);
        pixels[x].midRms   = catmull(d0.midRms,   d1.midRms,   d2.midRms,   d3.midRms,   t);
        pixels[x].highPeak = catmull(d0.highPeak, d1.highPeak, d2.highPeak, d3.highPeak, t);
        pixels[x].highRms  = catmull(d0.highRms,  d1.highRms,  d2.highRms,  d3.highRms,  t);
    }

    // Step 2: local normalisation (sliding max window over lowPeak).
    const int localWin = std::max(30, wInt / 12);
    std::vector<float> localMax(wInt, 0.001f);
    for (int x = 0; x < wInt; ++x) {
        float lm = 0.001f;
        for (int j = std::max(0, x - localWin); j <= std::min(wInt-1, x + localWin); ++j)
            if (pixels[j].lowPeak > lm) lm = pixels[j].lowPeak;
        localMax[x] = lm;
    }

    float globalMax = m_engine->getTrackData()->getGlobalMaxPeak();
    if (globalMax < 0.001f) globalMax = 0.001f;

    // Step 3: draw 4 strips per pixel.
    for (int x = 0; x < wInt; ++x) {
        // Mixed global (25%) + local (75%) reference for beat contrast while scrolling.
        float normRef = globalMax * 0.25f + localMax[x] * 0.75f;
        if (normRef < 0.001f) normRef = 0.001f;

        // Normalise + sqrt compression
        auto norm = [&](float v) {
            return std::sqrt(std::min(1.0f, v / normRef));
        };

        float lowPeakN = norm(pixels[x].lowPeak);
        float lowRmsN  = norm(pixels[x].lowRms);
        float midN     = norm(pixels[x].midPeak);
        float highN    = norm(pixels[x].highPeak);

        // The gain weighting is already baked in by the analyzer (LOW×1.5, MID×0.7, HIGH×0.3).
        // Just map normalised values to pixel heights.
        float lowPeakY = lowPeakN * midY;
        float lowRmsY  = lowRmsN  * midY;
        float midY_    = midN     * midY;
        float highY_   = highN    * midY;

        int vIdx = x * 2;
        const float fx = static_cast<float>(x);

        // Layer 0: LOW PEAK - dark blue halo (alpha 120, semi-transparent)
        lowPeakV[vIdx  ].set(fx, midY - lowPeakY, 30,  80, 220, 120);
        lowPeakV[vIdx+1].set(fx, midY + lowPeakY, 30,  80, 220, 120);

        // Layer 1: LOW RMS - bright blue core (alpha 220, near-opaque)
        lowRmsV[vIdx  ].set(fx, midY - lowRmsY, 60, 150, 255, 220);
        lowRmsV[vIdx+1].set(fx, midY + lowRmsY, 60, 150, 255, 220);

        // Layer 2: MID - amber/orange (alpha 200)
        midV[vIdx  ].set(fx, midY - midY_, 255, 140, 0, 200);
        midV[vIdx+1].set(fx, midY + midY_, 255, 140, 0, 200);

        // Layer 3: HIGH - white with slight blue tint (alpha 204)
        highV[vIdx  ].set(fx, midY - highY_, 240, 240, 255, 204);
        highV[vIdx+1].set(fx, midY + highY_, 240, 240, 255, 204);
    }

    lowPeakNode->markDirty(QSGNode::DirtyGeometry);
    lowRmsNode ->markDirty(QSGNode::DirtyGeometry);
    midNode    ->markDirty(QSGNode::DirtyGeometry);
    highNode   ->markDirty(QSGNode::DirtyGeometry);

    // Beatgrid rendering.
    QSGGeometry* beatGeo = beatNode->geometry();
    TrackData* td = m_engine->getTrackData();
    if (td->isBpmAnalyzed()) {
        double bpm            = td->getBpm();
        qint64 firstBeatSamp  = td->getFirstBeatSample();
        double sr             = td->getSampleRate();
        const float pps       = 150.0f; // must match the analyzer's pointsPerSecond

        double samplesPerBeat = sr * (60.0 / bpm);
        double pointsPerBeat  = samplesPerBeat / (sr / pps);

        double firstBeatPoint = static_cast<double>(firstBeatSamp) / (sr / pps);

        double visiblePoints  = w / pixelsPerPoint;
        double leftPoint      = centerIndexReal - visiblePoints / 2.0;
        double rightPoint     = centerIndexReal + visiblePoints / 2.0;

        // First and last visible beat index.
        int beatStart = static_cast<int>(std::floor((leftPoint - firstBeatPoint) / pointsPerBeat));
        int beatEnd   = static_cast<int>(std::ceil((rightPoint - firstBeatPoint) / pointsPerBeat));
        int visibleBeats = beatEnd - beatStart + 1;
        if (visibleBeats < 0) visibleBeats = 0;
        if (visibleBeats > 2000) visibleBeats = 2000; // Sicherheit

        // 2 Vertices pro Beat-Linie (oben + unten)
        beatGeo->allocate(visibleBeats * 2);
        QSGGeometry::ColoredPoint2D* bVerts = beatGeo->vertexDataAsColoredPoint2D();
        int bIdx = 0;

        for (int b = beatStart; b <= beatEnd; ++b) {
            double beatPoint = firstBeatPoint + b * pointsPerBeat;
            // Pixel-X dieser Beat-Linie
            float beatX = static_cast<float>(
                w / 2.0 + (beatPoint - centerIndexReal) * pixelsPerPoint
            );
            if (beatX < 0.0f || beatX > w) continue;

            // Downbeats (every 4th beat) are rendered brighter.
            bool isDownbeat = ((b % 4) == 0);
            uchar alpha = isDownbeat ? 180 : 100;
            uchar brightness = isDownbeat ? 255 : 200;

            bVerts[bIdx].set(beatX, 0.0f, brightness, brightness, brightness, alpha);
            bVerts[bIdx+1].set(beatX, height(), brightness, brightness, brightness, alpha);
            bIdx += 2;
        }

        // Zero out unused vertex slots.
        for (int i = bIdx; i < visibleBeats * 2; ++i)
            bVerts[i].set(0, 0, 0, 0, 0, 0);

    } else {
        beatGeo->allocate(0);
    }

    beatNode->markDirty(QSGNode::DirtyGeometry);
    
    return rootNode;
}
