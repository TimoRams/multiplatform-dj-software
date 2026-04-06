#include "ScrollingWaveformItem.h"
#include <QDebug>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <QColor>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Multiband DJ color mapping from spectral band energies.
static inline QColor mixDjWaveColor(float low, float mid, float high, float rms)
{
    low = std::clamp(low, 0.0f, 1.0f);
    mid = std::clamp(mid, 0.0f, 1.0f);
    high = std::clamp(high, 0.0f, 1.0f);
    rms = std::clamp(rms, 0.0f, 1.0f);

    // Stronger DJ palette:
    //   low  -> red
    //   mid  -> yellow/green
    //   high -> blue
    float r = std::pow(low, 0.52f) * 1.18f + std::pow(mid, 0.85f) * 0.36f;
    float g = std::pow(mid, 0.50f) * 1.20f + std::pow(high, 0.95f) * 0.10f + std::pow(low, 1.20f) * 0.06f;
    float b = std::pow(high, 0.50f) * 1.22f + std::pow(mid, 1.00f) * 0.08f;

    // Keep some transient whitening, but lower than before to avoid washed colors.
    const float whiteLift = std::pow(std::max({low, mid, high}), 0.70f) * (0.06f + 0.16f * rms);
    r += whiteLift;
    g += whiteLift;
    b += whiteLift;

    QColor c = QColor::fromRgbF(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f),
        1.0f);

    // Extra vibrance pass: saturate and brighten without shifting hue semantics.
    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    c.getHsvF(&h, &s, &v);
    s = std::clamp(static_cast<float>(s * 1.30 + 0.08), 0.0f, 1.0f);
    v = std::clamp(static_cast<float>(v * 1.12 + 0.03 + 0.07 * rms), 0.0f, 1.0f);
    c.setHsvF(h, s, v, 1.0);
    return c;
}

} // namespace

float ScrollingWaveformItem::clampToZoomLevel(float ppp)
{
    float best = ZOOM_LEVELS[0];
    float bestDist = std::abs(ppp - best);
    for (float lvl : ZOOM_LEVELS) {
        const float d = std::abs(ppp - lvl);
        if (d < bestDist) {
            best = lvl;
            bestDist = d;
        }
    }
    return best;
}

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
        connect(m_engine, &DjEngine::loopChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::segmentsChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
    } else {
        // nothing to stop — FrameAnimation in QML will have stopped already
    }
    emit engineChanged();
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::setPixelsPerPoint(float ppp)
{
    ppp = clampToZoomLevel(ppp);
    if (qFuzzyCompare(m_pixelsPerPoint, ppp)) return;
    m_pixelsPerPoint = ppp;
    emit pixelsPerPointChanged();
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::zoomIn()
{
    for (float lvl : ZOOM_LEVELS) {
        if (lvl > m_pixelsPerPoint + 0.0001f) {
            setPixelsPerPoint(lvl);
            return;
        }
    }
    setPixelsPerPoint(ZOOM_LEVELS.back());
}

void ScrollingWaveformItem::zoomOut()
{
    for (int i = static_cast<int>(ZOOM_LEVELS.size()) - 1; i >= 0; --i) {
        if (ZOOM_LEVELS[static_cast<size_t>(i)] < m_pixelsPerPoint - 0.0001f) {
            setPixelsPerPoint(ZOOM_LEVELS[static_cast<size_t>(i)]);
            return;
        }
    }
    setPixelsPerPoint(ZOOM_LEVELS.front());
}

void ScrollingWaveformItem::onTrackLoaded()
{
    if (m_engine && m_engine->getTrackData()) {
        connect(m_engine->getTrackData(), &TrackData::dataUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::rgbWaveformUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
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

    TrackData* td = m_engine->getTrackData();

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

        // 7: loop overlay rectangle (DrawTriangleStrip)
        auto* loopFillNode = new QSGGeometryNode();
        auto* loopFillGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        loopFillGeo->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        loopFillNode->setGeometry(loopFillGeo);
        loopFillNode->setFlag(QSGNode::OwnsGeometry);
        loopFillNode->setMaterial(new QSGVertexColorMaterial());
        loopFillNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(loopFillNode);

        // 8: loop in/out markers (DrawLines)
        auto* loopLineNode = new QSGGeometryNode();
        auto* loopLineGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        loopLineGeo->setDrawingMode(QSGGeometry::DrawLines);
        loopLineGeo->setLineWidth(1.0f);
        loopLineNode->setGeometry(loopLineGeo);
        loopLineNode->setFlag(QSGNode::OwnsGeometry);
        loopLineNode->setMaterial(new QSGVertexColorMaterial());
        loopLineNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(loopLineNode);

        // 9: segment strip at the bottom (DrawTriangles)
        auto* segmentNode = new QSGGeometryNode();
        auto* segmentGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        segmentGeo->setDrawingMode(QSGGeometry::DrawTriangles);
        segmentNode->setGeometry(segmentGeo);
        segmentNode->setFlag(QSGNode::OwnsGeometry);
        segmentNode->setMaterial(new QSGVertexColorMaterial());
        segmentNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(segmentNode);

        // 10: hotcue marker shadow (DrawLines)
        auto* hotCueShadowNode = new QSGGeometryNode();
        auto* hotCueShadowGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        hotCueShadowGeo->setDrawingMode(QSGGeometry::DrawLines);
        hotCueShadowGeo->setLineWidth(4.2f);
        hotCueShadowNode->setGeometry(hotCueShadowGeo);
        hotCueShadowNode->setFlag(QSGNode::OwnsGeometry);
        hotCueShadowNode->setMaterial(new QSGVertexColorMaterial());
        hotCueShadowNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(hotCueShadowNode);

        // 11: hotcue markers (DrawLines)
        auto* hotCueNode = new QSGGeometryNode();
        auto* hotCueGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        hotCueGeo->setDrawingMode(QSGGeometry::DrawLines);
        hotCueGeo->setLineWidth(2.6f);
        hotCueNode->setGeometry(hotCueGeo);
        hotCueNode->setFlag(QSGNode::OwnsGeometry);
        hotCueNode->setMaterial(new QSGVertexColorMaterial());
        hotCueNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(hotCueNode);
    }

    auto* lowNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowMidNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* midNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* highNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* beatNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));
    auto* downbeatNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(5));
    auto* triNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(6));
    auto* loopFillNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(7));
    auto* loopLineNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(8));
    auto* segmentNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(9));
    auto* hotCueShadowNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(10));
    auto* hotCueNode       = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(11));

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
    const double wD           = static_cast<double>(wInt);
    const float midY          = static_cast<float>(height()) / 2.0f;
    const double pointsPerSec = m_engine->waveformPointsPerSecond();
    const double tempoRatio   = m_engine->getTempoRatio();
    const double pixelsPerPoint = static_cast<double>(m_pixelsPerPoint) / std::max(0.0001, tempoRatio);
    const double centerIndexReal = static_cast<double>(m_engine->getVisualPosition()) * pointsPerSec;

    // Chunk-wise waveform data access: fetch only what is visible (+guard).
    const double visiblePoints = wD / std::max(0.0001, pixelsPerPoint);
    const int guardPoints = 96;
    const int sliceStart = static_cast<int>(std::floor(centerIndexReal - visiblePoints * 0.5)) - guardPoints - 4;
    const int sliceEnd = static_cast<int>(std::ceil(centerIndexReal + visiblePoints * 0.5)) + guardPoints + 4;
    int sliceBaseIndex = 0;
    QVector<TrackData::RgbWaveformFrame> rgbData = td->getRgbWaveformSlice(sliceStart, sliceEnd, &sliceBaseIndex);
    if (rgbData.isEmpty()) {
        if (oldNode) delete oldNode;
        return nullptr;
    }

    // Catmull-Rom Spline
    auto catmull = [](float p0, float p1, float p2, float p3, float t) {
        float v = 0.5f * ((2.0f*p1) + (-p0+p2)*t
                          + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3)*t*t
                          + (-p0 + 3.0f*p1 - 3.0f*p2 + p3)*t*t*t);
        return std::max(0.0f, v);
    };

    const TrackData::RgbWaveformFrame zeroFD{};
    auto getD = [&](int idx) -> const TrackData::RgbWaveformFrame& {
        const int local = idx - sliceBaseIndex;
        if (local < 0)
            return zeroFD;
        if (local >= rgbData.size())
            return zeroFD;
        return rgbData[local];
    };

    // Catmull-Rom interpolation per output pixel (bands + amplitude).
    struct ScrollPixel {
        float rms = 0.0f;
        float low = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
        QColor color;
    };
    std::vector<ScrollPixel> pixels(wInt);

    const int subSamples = std::clamp(
        static_cast<int>(std::ceil(2.2 / std::max(0.12, pixelsPerPoint))),
        1,
        8);
    for (int x = 0; x < wInt; ++x) {
        const double dataPos = centerIndexReal + (static_cast<double>(x) - wD * 0.5) / pixelsPerPoint;
        float maxRms = 0.0f;
        float sumLow = 0.0f;
        float sumMid = 0.0f;
        float sumHigh = 0.0f;

        for (int s = 0; s < subSamples; ++s) {
            const double ofs = (subSamples == 1)
                ? 0.0
                : ((static_cast<double>(s) + 0.5) / static_cast<double>(subSamples) - 0.5) * 0.92;
            const double samplePos = dataPos + ofs;
            const int i0 = static_cast<int>(std::floor(samplePos)) - 1;
            const float t = static_cast<float>(samplePos - std::floor(samplePos));

            const auto& d0 = getD(i0);
            const auto& d1 = getD(i0+1);
            const auto& d2 = getD(i0+2);
            const auto& d3 = getD(i0+3);

            const float rms = catmull(d0.rms, d1.rms, d2.rms, d3.rms, t);
            const float low = catmull(d0.low, d1.low, d2.low, d3.low, t);
            const float mid = catmull(d0.mid, d1.mid, d2.mid, d3.mid, t);
            const float high = catmull(d0.high, d1.high, d2.high, d3.high, t);

            maxRms = std::max(maxRms, rms);
            sumLow += low;
            sumMid += mid;
            sumHigh += high;
        }

        const float invN = 1.0f / static_cast<float>(subSamples);
        pixels[x].rms = maxRms;
        pixels[x].low = sumLow * invN;
        pixels[x].mid = sumMid * invN;
        pixels[x].high = sumHigh * invN;
    }

    // Lightweight horizontal smoothing to avoid blocky edges at high zoom-out.
    if (wInt >= 3 && pixelsPerPoint < 0.95) {
        std::vector<ScrollPixel> smooth = pixels;
        for (int x = 1; x < wInt - 1; ++x) {
            smooth[x].rms  = pixels[x - 1].rms * 0.16f + pixels[x].rms * 0.68f + pixels[x + 1].rms * 0.16f;
            smooth[x].low  = pixels[x - 1].low * 0.16f + pixels[x].low * 0.68f + pixels[x + 1].low * 0.16f;
            smooth[x].mid  = pixels[x - 1].mid * 0.16f + pixels[x].mid * 0.68f + pixels[x + 1].mid * 0.16f;
            smooth[x].high = pixels[x - 1].high * 0.16f + pixels[x].high * 0.68f + pixels[x + 1].high * 0.16f;
        }
        pixels.swap(smooth);
    }

    for (int x = 0; x < wInt; ++x) {
        pixels[x].color = mixDjWaveColor(pixels[x].low, pixels[x].mid, pixels[x].high, pixels[x].rms);
    }

    // Premium multi-layer body: soft glow + main body + bright core.
    for (int x = 0; x < wInt; ++x) {
        const float fx = static_cast<float>(x) + 0.5f;
        const int vIdx = x * 2;

        const float rms = std::clamp(pixels[x].rms, 0.0f, 1.0f);
        const float bodyAmp = rms * midY;
        const float glowAmp = std::min(midY, bodyAmp * 1.34f + 0.7f);
        const float coreAmp = bodyAmp * 0.56f;

        const QColor c = pixels[x].color;
        const int r = c.red();
        const int g = c.green();
        const int b = c.blue();
        const int coreR = std::clamp(static_cast<int>(r * 1.10f + 10.0f), 0, 255);
        const int coreG = std::clamp(static_cast<int>(g * 1.10f + 10.0f), 0, 255);
        const int coreB = std::clamp(static_cast<int>(b * 1.10f + 10.0f), 0, 255);

        // Node 0: outer glow.
        lowV[vIdx  ].set(fx, midY - glowAmp, r, g, b, 84);
        lowV[vIdx+1].set(fx, midY + glowAmp, r, g, b, 84);

        // Node 1: main waveform body.
        lowMidV[vIdx  ].set(fx, midY - bodyAmp, r, g, b, 232);
        lowMidV[vIdx+1].set(fx, midY + bodyAmp, r, g, b, 232);

        // Node 2: bright center core for depth/contrast.
        midV[vIdx  ].set(fx, midY - coreAmp, coreR, coreG, coreB, 248);
        midV[vIdx+1].set(fx, midY + coreAmp, coreR, coreG, coreB, 248);

        // Node 3 unused in RGB mode.
        highV[vIdx  ].set(fx, midY, 0, 0, 0, 0);
        highV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
    }

    lowNode   ->markDirty(QSGNode::DirtyGeometry);
    lowMidNode->markDirty(QSGNode::DirtyGeometry);
    midNode   ->markDirty(QSGNode::DirtyGeometry);
    highNode  ->markDirty(QSGNode::DirtyGeometry);

    // ── Beat-grid rendering (DJ style) ───────────────────────────────────────
    // Node 4: regular beat lines     — white, 1px, alpha 110
    // Node 5: downbeat lines         — red (#e6, 0, 0), 1px, alpha 220
    // Node 6: downbeat triangles     — red filled, pointing down from top edge
    QSGGeometry* beatGeo     = beatNode    ->geometry();
    QSGGeometry* downGeo     = downbeatNode->geometry();
    QSGGeometry* triGeo2     = triNode     ->geometry();
    if (td->isBpmAnalyzed()) {
        const double sr  = td->getSampleRate();
        const float  pps = pointsPerSec;

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

    // ── Loop overlay rendering ───────────────────────────────────────────────
    // Draws a translucent block for active loop range and two vertical markers.
    QSGGeometry* loopFillGeo = loopFillNode->geometry();
    QSGGeometry* loopLineGeo = loopLineNode->geometry();

    const bool showLoopPreview = m_engine->loopActive() || m_engine->loopInSet();
    if (showLoopPreview) {
        const double loopInSec = m_engine->loopInPosition();
        const double loopOutSec = m_engine->loopActive()
            ? m_engine->loopOutPosition()
            : static_cast<double>(m_engine->getVisualPosition());

        const double loopInPoint = loopInSec * pointsPerSec;
        const double loopOutPoint = loopOutSec * pointsPerSec;

        float xIn = static_cast<float>(w / 2.0 + (loopInPoint - centerIndexReal) * pixelsPerPoint);
        float xOut = static_cast<float>(w / 2.0 + (loopOutPoint - centerIndexReal) * pixelsPerPoint);

        if (xOut < xIn)
            std::swap(xIn, xOut);

        const float drawLeft = std::max(0.0f, xIn);
        const float drawRight = std::min(w, xOut);

        if (drawRight > drawLeft + 0.5f) {
            loopFillGeo->allocate(4);
            auto* fv = loopFillGeo->vertexDataAsColoredPoint2D();
            fv[0].set(drawLeft,  0.0f,            90, 180, 255, 36);
            fv[1].set(drawLeft,  static_cast<float>(height()), 90, 180, 255, 36);
            fv[2].set(drawRight, 0.0f,            90, 180, 255, 36);
            fv[3].set(drawRight, static_cast<float>(height()), 90, 180, 255, 36);

            loopLineGeo->allocate(4);
            auto* lv = loopLineGeo->vertexDataAsColoredPoint2D();
            lv[0].set(drawLeft,  0.0f,            120, 210, 255, 210);
            lv[1].set(drawLeft,  static_cast<float>(height()), 120, 210, 255, 210);
            lv[2].set(drawRight, 0.0f,            120, 210, 255, 210);
            lv[3].set(drawRight, static_cast<float>(height()), 120, 210, 255, 210);
        } else {
            loopFillGeo->allocate(0);
            // Even with tiny width, show the loop-in marker so user gets instant feedback.
            const float markerX = std::clamp(xIn, 0.0f, w);
            loopLineGeo->allocate(2);
            auto* lv = loopLineGeo->vertexDataAsColoredPoint2D();
            lv[0].set(markerX, 0.0f, 120, 210, 255, 220);
            lv[1].set(markerX, static_cast<float>(height()), 120, 210, 255, 220);
        }
    } else {
        loopFillGeo->allocate(0);
        loopLineGeo->allocate(0);
    }

    loopFillNode->markDirty(QSGNode::DirtyGeometry);
    loopLineNode->markDirty(QSGNode::DirtyGeometry);

    // ── Hotcue markers ─────────────────────────────────────────────────────
    QSGGeometry* hotCueShadowGeo = hotCueShadowNode->geometry();
    QSGGeometry* hotCueGeo = hotCueNode->geometry();
    const QVariantList cues = m_engine->hotCues();
    if (!cues.isEmpty()) {
        struct CueLine {
            float x;
            QColor color;
        };
        std::vector<CueLine> visibleCues;
        visibleCues.reserve(static_cast<size_t>(cues.size()));

        for (const QVariant& v : cues) {
            const QVariantMap m = v.toMap();
            if (!m.value("set").toBool())
                continue;

            const double cueSec = m.value("positionSec").toDouble();
            const double cuePoint = cueSec * pointsPerSec;
            const float x = static_cast<float>(w / 2.0 + (cuePoint - centerIndexReal) * pixelsPerPoint);
            if (x < 0.0f || x > w)
                continue;

            QColor c(m.value("color").toString());
            if (!c.isValid())
                c = QColor("#e04040");
            visibleCues.push_back({x, c});
        }

        hotCueShadowGeo->allocate(static_cast<int>(visibleCues.size()) * 2);
        hotCueGeo->allocate(static_cast<int>(visibleCues.size()) * 2);
        auto* shadowVtx = hotCueShadowGeo->vertexDataAsColoredPoint2D();
        auto* vtx = hotCueGeo->vertexDataAsColoredPoint2D();
        int idx = 0;
        for (const auto& cue : visibleCues) {
            const auto r = static_cast<uchar>(cue.color.red());
            const auto g = static_cast<uchar>(cue.color.green());
            const auto b = static_cast<uchar>(cue.color.blue());

            const int base = idx;
            shadowVtx[base].set(cue.x, 0.0f,                         0, 0, 0, 170);
            shadowVtx[base + 1].set(cue.x, static_cast<float>(height()), 0, 0, 0, 140);

            vtx[base].set(cue.x, 0.0f,                         r, g, b, 255);
            vtx[base + 1].set(cue.x, static_cast<float>(height()), r, g, b, 220);
            idx += 2;
        }
    } else {
        hotCueShadowGeo->allocate(0);
        hotCueGeo->allocate(0);
    }

    hotCueShadowNode->markDirty(QSGNode::DirtyGeometry);
    hotCueNode->markDirty(QSGNode::DirtyGeometry);

    // ── Segment strip rendering (tiny colored bar at bottom) ───────────────
    // Draw each segment as a 4px-high rectangle aligned to the waveform timeline.
    QSGGeometry* segGeo = segmentNode->geometry();
    const QVariantList segments = m_engine->currentSegments();
    if (!segments.isEmpty()) {
        struct SegRect {
            float x1;
            float x2;
            QColor color;
        };
        std::vector<SegRect> segRects;
        segRects.reserve(static_cast<size_t>(segments.size()));

        for (const QVariant& v : segments) {
            const QVariantMap m = v.toMap();
            const double startSec = m.value("startTime").toDouble();
            const double endSec = m.value("endTime").toDouble();
            if (endSec <= startSec)
                continue;

            const double startPoint = startSec * pointsPerSec;
            const double endPoint = endSec * pointsPerSec;

            float x1 = static_cast<float>(w / 2.0 + (startPoint - centerIndexReal) * pixelsPerPoint);
            float x2 = static_cast<float>(w / 2.0 + (endPoint - centerIndexReal) * pixelsPerPoint);

            if (x2 < x1)
                std::swap(x1, x2);

            x1 = std::clamp(x1, 0.0f, w);
            x2 = std::clamp(x2, 0.0f, w);
            if (x2 <= x1 + 0.5f)
                continue;

            QColor c(m.value("colorHex").toString());
            if (!c.isValid())
                c = QColor("#666666");

            segRects.push_back({x1, x2, c});
        }

        if (!segRects.empty()) {
            const float yBottom = static_cast<float>(height());
            const float yTop = std::max(0.0f, yBottom - 4.0f);

            segGeo->allocate(static_cast<int>(segRects.size()) * 6);
            auto* vtx = segGeo->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (const auto& s : segRects) {
                const auto r = static_cast<uchar>(s.color.red());
                const auto g = static_cast<uchar>(s.color.green());
                const auto b = static_cast<uchar>(s.color.blue());
                constexpr uchar a = 220;

                // Triangle 1
                vtx[idx++].set(s.x1, yTop,    r, g, b, a);
                vtx[idx++].set(s.x1, yBottom, r, g, b, a);
                vtx[idx++].set(s.x2, yBottom, r, g, b, a);
                // Triangle 2
                vtx[idx++].set(s.x1, yTop,    r, g, b, a);
                vtx[idx++].set(s.x2, yBottom, r, g, b, a);
                vtx[idx++].set(s.x2, yTop,    r, g, b, a);
            }
        } else {
            segGeo->allocate(0);
        }
    } else {
        segGeo->allocate(0);
    }

    segmentNode->markDirty(QSGNode::DirtyGeometry);
    
    return rootNode;
}
