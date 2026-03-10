#include "ScrollingWaveformItem.h"
#include <QDebug>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <cmath>
#include <vector>

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
    //   1: lowMidNode  - gold/ocker    (bass body / warmth, BP 150–160 Hz)
    //   2: midNode     - orange/red    (snare / vocals,     BP 180–800 Hz)
    //   3: highNode    - pure white    (hi-hat / perc,      BP@2750 + HP@19k)
    //   4: beatNode    - regular beat lines (white, thin)
    //   5: downbeatNode- downbeat lines (red) + triangle markers
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

        auto makeLinesNode = [](QSGNode* parent) -> QSGGeometryNode* {
            auto* node = new QSGGeometryNode();
            auto* geo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            geo->setDrawingMode(QSGGeometry::DrawLines);
            geo->setLineWidth(1.0f);
            node->setGeometry(geo);
            node->setFlag(QSGNode::OwnsGeometry);
            node->setMaterial(new QSGVertexColorMaterial());
            node->setFlag(QSGNode::OwnsMaterial);
            parent->appendChildNode(node);
            return node;
        };

        makeStrip(rootNode);     // 0: low
        makeStrip(rootNode);     // 1: lowMid
        makeStrip(rootNode);     // 2: mid
        makeStrip(rootNode);     // 3: high
        makeLinesNode(rootNode); // 4: regular beat lines (DrawLines, white)
        makeLinesNode(rootNode); // 5: downbeat lines + triangle markers (DrawLines red + DrawTriangles)
        // Note: node 5 is reused for BOTH the red vertical line AND the triangle.
        // We allocate two separate geometry nodes inside it: the child QSGNode
        // approach would require more nodes, so instead we use a shared Lines node
        // for the line and a sibling TrianglesNode appended directly to rootNode.

        // 6: downbeat triangle markers (DrawTriangles, red filled)
        auto* triNode = new QSGGeometryNode();
        auto* triGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        triGeo->setDrawingMode(QSGGeometry::DrawTriangles);
        triNode->setGeometry(triGeo);
        triNode->setFlag(QSGNode::OwnsGeometry);
        triNode->setMaterial(new QSGVertexColorMaterial());
        triNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(triNode);
    }

    auto* lowNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowMidNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* midNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* highNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* beatNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));
    auto* downbeatNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(5));
    auto* triNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(6));

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

    // ── Beat-grid rendering (Rekordbox style) ────────────────────────────────
    // Node 4: regular beat lines     — white, 1px, alpha 110
    // Node 5: downbeat lines         — red (#e6, 0, 0), 1px, alpha 220
    // Node 6: downbeat triangles     — red filled, pointing down from top edge
    QSGGeometry* beatGeo     = beatNode    ->geometry();
    QSGGeometry* downGeo     = downbeatNode->geometry();
    QSGGeometry* triGeo2     = triNode     ->geometry();
    TrackData* td = m_engine->getTrackData();
    if (td->isBpmAnalyzed()) {
        const double sr  = td->getSampleRate();
        const float  pps = 150.0f;

        // Prefer elastic BeatMarker grid; fall back to rigid grid.
        std::vector<TrackData::BeatMarker> beatGrid = td->getBeatGrid();
        const bool hasElasticGrid = !beatGrid.empty();

        const double ppp          = static_cast<double>(pixelsPerPoint);
        const double visiblePoints = w / ppp;
        const double leftSec       = (centerIndexReal - visiblePoints / 2.0) / pps;
        const double rightSec      = (centerIndexReal + visiblePoints / 2.0) / pps;

        // Collect visible markers, separated into regular and downbeat lists.
        struct VisibleBeat { float x; bool isDownbeat; int barNumber; };
        std::vector<VisibleBeat> visible;
        visible.reserve(256);

        if (hasElasticGrid) {
            // Binary-search: find first marker that could be visible.
            auto cmp = [](const TrackData::BeatMarker& m, double t){
                return m.positionSec < t; };
            auto it = std::lower_bound(beatGrid.begin(), beatGrid.end(),
                                       leftSec - 0.5, cmp);
            for (; it != beatGrid.end() && it->positionSec <= rightSec + 0.5; ++it) {
                double beatPoint = it->positionSec * pps;
                float  bx = static_cast<float>(w / 2.0 + (beatPoint - centerIndexReal) * ppp);
                if (bx >= 0.0f && bx <= w)
                    visible.push_back({bx, it->isDownbeat, it->barNumber});
            }
        } else {
            // Legacy rigid-grid fallback.
            double bpm          = td->getBpm();
            qint64 firstBeatSamp = td->getFirstBeatSample();
            double firstBeatSec  = static_cast<double>(firstBeatSamp) / sr;
            double beatPeriod    = 60.0 / bpm;
            int beatStart = static_cast<int>(std::floor((leftSec  - firstBeatSec) / beatPeriod));
            int beatEnd   = static_cast<int>(std::ceil ((rightSec - firstBeatSec) / beatPeriod));
            beatStart = std::max(beatStart, -1);
            beatEnd   = std::min(beatEnd,   100000);
            for (int b = beatStart; b <= beatEnd; ++b) {
                double beatSec  = firstBeatSec + b * beatPeriod;
                double beatPoint = beatSec * pps;
                float  bx = static_cast<float>(w / 2.0 + (beatPoint - centerIndexReal) * ppp);
                if (bx >= 0.0f && bx <= w)
                    visible.push_back({bx, (b % 4 == 0), b / 4 + 1});
            }
        }

        // Count regular vs downbeat lines for allocation.
        int numRegular  = 0;
        int numDownbeat = 0;
        for (auto& v : visible) {
            if (v.isDownbeat) ++numDownbeat; else ++numRegular;
        }

        // ── Node 4: regular beat lines (white, thin) ─────────────────────────
        beatGeo->allocate(numRegular * 2);
        {
            auto* v = beatGeo->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                if (vb.isDownbeat) continue;
                v[idx  ].set(vb.x, 0.0f,    200, 200, 200, 110);
                v[idx+1].set(vb.x, height(), 200, 200, 200, 110);
                idx += 2;
            }
        }

        // ── Node 5: downbeat lines (red, full height) ─────────────────────────
        downGeo->allocate(numDownbeat * 2);
        {
            auto* v = downGeo->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                if (!vb.isDownbeat) continue;
                v[idx  ].set(vb.x, 0.0f,    230, 0, 0, 220);
                v[idx+1].set(vb.x, height(), 230, 0, 0, 180);
                idx += 2;
            }
        }

        // ── Node 6: downbeat triangle markers (red filled, top of waveform) ──
        // Each triangle: tip points down at y=10, base at y=0,
        // centred on the downbeat x position, width=8px.
        //
        //    x-4      x      x+4
        //     *-------*-------*   y = 0   (base)
        //              *           y = 10  (tip)
        //
        const float triH = 10.0f;  // height of triangle in pixels
        const float triW =  4.0f;  // half-width of triangle base
        triGeo2->allocate(numDownbeat * 3);
        {
            auto* v = triGeo2->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                if (!vb.isDownbeat) continue;
                v[idx  ].set(vb.x - triW, 0.0f, 230, 0, 0, 230); // top-left
                v[idx+1].set(vb.x + triW, 0.0f, 230, 0, 0, 230); // top-right
                v[idx+2].set(vb.x,        triH, 230, 0, 0, 200); // tip
                idx += 3;
            }
        }

    } else {
        beatGeo ->allocate(0);
        downGeo ->allocate(0);
        triGeo2 ->allocate(0);
    }

    beatNode    ->markDirty(QSGNode::DirtyGeometry);
    downbeatNode->markDirty(QSGNode::DirtyGeometry);
    triNode     ->markDirty(QSGNode::DirtyGeometry);
    
    return rootNode;
}
