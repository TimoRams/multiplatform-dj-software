#include "ScrollingWaveformItem.h"
#include <QDebug>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <cmath>

ScrollingWaveformItem::ScrollingWaveformItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
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
    } else {
        // nothing to stop — FrameAnimation in QML will have stopped already
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
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::onDataUpdated()
{
    m_forceUpdate = true;
    update();
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
    //   0: lowNode    - dark blue     (sub-bass / kick,    LP @ 110 Hz)
    //   1: lowMidNode - gold/ocker    (bass body / warmth, BP 150–160 Hz)
    //   2: midNode    - orange/red    (snare / vocals,     BP 180–800 Hz)
    //   3: highNode   - pure white    (hi-hat / perc,      BP@2750 + HP@19k)
    //   4: beatNode   - beat grid lines (on top)
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

        makeStrip(rootNode); // 0: low
        makeStrip(rootNode); // 1: lowMid
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

    auto* lowNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowMidNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* midNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* highNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* beatNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));

    int wInt = static_cast<int>(width());
    if (wInt <= 0) return rootNode;

    lowNode   ->geometry()->allocate(wInt * 2);
    lowMidNode->geometry()->allocate(wInt * 2);
    midNode   ->geometry()->allocate(wInt * 2);
    highNode  ->geometry()->allocate(wInt * 2);

    auto* lowV    = lowNode   ->geometry()->vertexDataAsColoredPoint2D();
    auto* lowMidV = lowMidNode->geometry()->vertexDataAsColoredPoint2D();
    auto* midV    = midNode   ->geometry()->vertexDataAsColoredPoint2D();
    auto* highV   = highNode  ->geometry()->vertexDataAsColoredPoint2D();

    const float w             = static_cast<float>(wInt);
    const float midY          = static_cast<float>(height()) / 2.0f;
    const float pointsPerSec  = 150.0f;
    const double tempoRatio   = m_engine->getTempoRatio();
    const float pixelsPerPoint = static_cast<float>(m_pixelsPerPoint / tempoRatio);
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

    // Catmull-Rom interpolation per output pixel (4 bands).
    struct ScrollPixel {
        float low = 0.0f, lowMid = 0.0f, mid = 0.0f, high = 0.0f;
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

        pixels[x].low    = catmull(d0.low,    d1.low,    d2.low,    d3.low,    t);
        pixels[x].lowMid = catmull(d0.lowMid, d1.lowMid, d2.lowMid, d3.lowMid, t);
        pixels[x].mid    = catmull(d0.mid,    d1.mid,    d2.mid,    d3.mid,    t);
        pixels[x].high   = catmull(d0.high,   d1.high,   d2.high,   d3.high,   t);
    }

    // Draw 4 STACKED layers (back to front).
    // Each band adds its height ON TOP of the previous one, so all 4 colors
    // are visible as distinct stripes (like Rekordbox), not hidden behind
    // the largest layer.
    //
    //   totalH = low + lowMid + mid + high   (clamped to midY)
    //
    //   Layer 0 (outermost): LOW  — dark blue — from midY±totalH to midY±(totalH-low)
    //   Layer 1:             LOWMID — gold     — from midY±(lm+mid+high) to midY±(mid+high)
    //   Layer 2:             MID  — orange     — from midY±(mid+high) to midY±high
    //   Layer 3 (innermost): HIGH — white      — from midY±high to midY
    for (int x = 0; x < wInt; ++x) {
        const float fx = static_cast<float>(x);
        const int vIdx = x * 2;

        // Raw band heights (each 0..1 * midY)
        float hLow    = pixels[x].low    * midY;
        float hLowMid = pixels[x].lowMid * midY;
        float hMid    = pixels[x].mid    * midY;
        float hHigh   = pixels[x].high   * midY;

        // Stack from inside out: high is innermost, low is outermost
        float stackHigh   = hHigh;
        float stackMid    = hHigh + hMid;
        float stackLowMid = hHigh + hMid + hLowMid;
        float stackLow    = hHigh + hMid + hLowMid + hLow;

        // Clamp total to available half-height
        if (stackLow > midY) {
            float scale = midY / stackLow;
            stackHigh   *= scale;
            stackMid    *= scale;
            stackLowMid *= scale;
            stackLow     = midY;
        }

        // Layer 0 (background): LOW — dark blue — outermost band
        lowV[vIdx  ].set(fx, midY - stackLow,  0, 0, 255, 220);
        lowV[vIdx+1].set(fx, midY + stackLow,  0, 0, 255, 220);

        // Layer 1: LOWMID — gold/ocker — second band from outside
        lowMidV[vIdx  ].set(fx, midY - stackLowMid, 255, 170, 0, 200);
        lowMidV[vIdx+1].set(fx, midY + stackLowMid, 255, 170, 0, 200);

        // Layer 2: MID — orange/red — second band from inside
        midV[vIdx  ].set(fx, midY - stackMid, 255, 68, 0, 200);
        midV[vIdx+1].set(fx, midY + stackMid, 255, 68, 0, 200);

        // Layer 3 (foreground): HIGH — pure white — innermost band
        highV[vIdx  ].set(fx, midY - stackHigh, 255, 255, 255, 220);
        highV[vIdx+1].set(fx, midY + stackHigh, 255, 255, 255, 220);
    }

    lowNode   ->markDirty(QSGNode::DirtyGeometry);
    lowMidNode->markDirty(QSGNode::DirtyGeometry);
    midNode   ->markDirty(QSGNode::DirtyGeometry);
    highNode  ->markDirty(QSGNode::DirtyGeometry);

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
