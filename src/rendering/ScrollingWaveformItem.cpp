#include "ScrollingWaveformItem.h"
#include <QDebug>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <QQuickWindow>
#include <QColor>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Rekordbox-style RGB waveform color blend.
// Frequency → hue mapping mirrors the industry-standard DJ color scheme:
//   low     → vivid red      (sub-bass / kick)
//   lowMid  → orange         (bass body / warmth)
//   mid     → yellow-lime    (snare / melody / vocals)
//   high    → electric cyan  (hi-hat / transients / air)
// Brightness scales with pow(rms, 0.40) — vivid even at moderate levels.
static inline QColor mixRekordboxColor(float low, float lowMid, float mid, float high, float rms)
{
    low    = std::clamp(low,    0.0f, 1.0f);
    lowMid = std::clamp(lowMid, 0.0f, 1.0f);
    mid    = std::clamp(mid,    0.0f, 1.0f);
    high   = std::clamp(high,   0.0f, 1.0f);
    rms    = std::clamp(rms,    0.0f, 1.0f);

    constexpr float lR = 255.0f, lG = 20.0f,  lB = 20.0f;   // vivid red
    constexpr float mR = 255.0f, mG = 130.0f, mB = 0.0f;    // orange
    constexpr float hR = 210.0f, hG = 255.0f, hB = 0.0f;    // yellow-lime
    constexpr float xR = 0.0f,   xG = 185.0f, xB = 255.0f;  // electric cyan

    // Higher exponents → dominant frequency band wins more decisively (Rekordbox look).
    // Highs use a lower exponent so transient detail shows through bass content.
    const float wL  = std::pow(low,    2.8f);
    const float wLM = std::pow(lowMid, 2.5f);
    const float wM  = std::pow(mid,    2.2f);
    const float wH  = std::pow(high,   1.6f);
    const float wSum = wL + wLM + wM + wH + 1e-7f;

    float r = (wL * lR + wLM * mR + wM * hR + wH * xR) / wSum;
    float g = (wL * lG + wLM * mG + wM * hG + wH * xG) / wSum;
    float b = (wL * lB + wLM * mB + wM * hB + wH * xB) / wSum;

    // pow(rms, 0.35) → vivid at moderate volumes, not just at full amplitude.
    const float bright = std::pow(rms, 0.35f);
    r *= bright;
    g *= bright;
    b *= bright;

    return QColor(
        std::clamp(static_cast<int>(r), 0, 255),
        std::clamp(static_cast<int>(g), 0, 255),
        std::clamp(static_cast<int>(b), 0, 255));
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
        connect(m_engine, &DjEngine::mainCueChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
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
        connect(m_engine->getTrackData(), &TrackData::peakMipUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
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

        // Beat/downbeat lines are rendered as explicit 1-device-pixel-wide quads
        // (DrawTriangles) instead of DrawLines to guarantee consistent thickness on
        // all GPUs and DPR settings — Vulkan DrawLines can vary between 1 and 2px
        // depending on sub-pixel x position.
        auto makeQuadLinesNode = [](QSGNode* parent) -> QSGGeometryNode* {
            auto* node = new QSGGeometryNode();
            auto* geo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            geo->setDrawingMode(QSGGeometry::DrawTriangles);
            node->setGeometry(geo);
            node->setFlag(QSGNode::OwnsGeometry);
            node->setMaterial(new QSGVertexColorMaterial());
            node->setFlag(QSGNode::OwnsMaterial);
            parent->appendChildNode(node);
            return node;
        };

        makeStrip(rootNode);          // 0: low
        makeStrip(rootNode);          // 1: lowMid
        makeStrip(rootNode);          // 2: mid
        makeStrip(rootNode);          // 3: high
        makeQuadLinesNode(rootNode);  // 4: regular beat lines (DrawTriangles, white)
        makeQuadLinesNode(rootNode);  // 5: downbeat lines (DrawTriangles, red)
        // Note: node 5 is reused for BOTH the red vertical line AND the triangle.
        // We allocate two separate geometry nodes inside it: the child QSGNode
        // approach would require more nodes, so instead we use a shared Lines node
        // for the line and a sibling TrianglesNode appended directly to rootNode.

        // 6: beat triangle markers — all beats, top + bottom (DrawTriangles)
        //    downbeat = red larger, regular = gray/white smaller
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

        // 12: hotcue triangle markers (DrawTriangles, colored top+bottom)
        auto* hotCueTriNode = new QSGGeometryNode();
        auto* hotCueTriGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        hotCueTriGeo->setDrawingMode(QSGGeometry::DrawTriangles);
        hotCueTriNode->setGeometry(hotCueTriGeo);
        hotCueTriNode->setFlag(QSGNode::OwnsGeometry);
        hotCueTriNode->setMaterial(new QSGVertexColorMaterial());
        hotCueTriNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(hotCueTriNode);

        // 13: main cue line (DrawLines, gray)
        auto* mainCueLineNode = new QSGGeometryNode();
        auto* mainCueLineGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        mainCueLineGeo->setDrawingMode(QSGGeometry::DrawLines);
        mainCueLineGeo->setLineWidth(2.0f);
        mainCueLineNode->setGeometry(mainCueLineGeo);
        mainCueLineNode->setFlag(QSGNode::OwnsGeometry);
        mainCueLineNode->setMaterial(new QSGVertexColorMaterial());
        mainCueLineNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(mainCueLineNode);

        // 14: main cue triangles (DrawTriangles, orange top+bottom)
        auto* mainCueTriNode = new QSGGeometryNode();
        auto* mainCueTriGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        mainCueTriGeo->setDrawingMode(QSGGeometry::DrawTriangles);
        mainCueTriNode->setGeometry(mainCueTriGeo);
        mainCueTriNode->setFlag(QSGNode::OwnsGeometry);
        mainCueTriNode->setMaterial(new QSGVertexColorMaterial());
        mainCueTriNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(mainCueTriNode);
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
    auto* hotCueTriNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(12));
    auto* mainCueLineNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(13));
    auto* mainCueTriNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(14));

    int wInt = static_cast<int>(std::lround(width()));
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
    const double dpr          = window() ? std::max(1.0, static_cast<double>(window()->effectiveDevicePixelRatio())) : 1.0;
    const double snapScale    = dpr;
    const double pointsPerSec = m_engine->waveformPointsPerSecond();
    const double tempoRatio   = m_engine->getTempoRatio();
    const double pixelsPerPoint = static_cast<double>(m_pixelsPerPoint) / std::max(0.0001, tempoRatio);
    // Use exact sub-pixel position so the waveform body scrolls smoothly.
    // Individual sharp elements (beat lines, cues) are independently snapped
    // to device-pixel boundaries via snapDevicePixelX.
    const double centerIndexRender = static_cast<double>(m_engine->getVisualPosition()) * pointsPerSec;

    const auto snapDevicePixelX = [snapScale](double x) -> float {
        return static_cast<float>(std::round(x * snapScale) / snapScale);
    };

    // Chunk-wise waveform data access: fetch only what is visible (+guard).
    const double visiblePoints = wD / std::max(0.0001, pixelsPerPoint);
    const int guardPoints = 96;
    const int sliceStart = static_cast<int>(std::floor(centerIndexRender - visiblePoints * 0.5)) - guardPoints - 4;
    const int sliceEnd = static_cast<int>(std::ceil(centerIndexRender + visiblePoints * 0.5)) + guardPoints + 4;
    int sliceBaseIndex = 0;
    QVector<TrackData::RgbWaveformFrame> rgbData = td->getRgbWaveformSlice(sliceStart, sliceEnd, &sliceBaseIndex);
    if (rgbData.isEmpty()) {
        delete rootNode;
        return nullptr;
    }

    // Catmull-Rom Spline — clamps to ≥ 0 (safe for RMS / band envelope values).
    auto catmull = [](float p0, float p1, float p2, float p3, float t) {
        float v = 0.5f * ((2.0f*p1) + (-p0+p2)*t
                          + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3)*t*t
                          + (-p0 + 3.0f*p1 - 3.0f*p2 + p3)*t*t*t);
        return std::max(0.0f, v);
    };
    // Signed variant for peak-mip data: no zero clamp so negative minSample
    // values survive interpolation and produce the correct bottom waveform trace.
    auto catmullSigned = [](float p0, float p1, float p2, float p3, float t) -> float {
        return 0.5f * ((2.0f*p1) + (-p0+p2)*t
                       + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3)*t*t
                       + (-p0 + 3.0f*p1 - 3.0f*p2 + p3)*t*t*t);
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

    // Catmull-Rom interpolation per output pixel (bands + amplitude + peak oscillation).
    struct ScrollPixel {
        float rms     = 0.0f;
        float low     = 0.0f;
        float lowMid  = 0.0f;
        float mid     = 0.0f;
        float high    = 0.0f;
        float peakMin = 0.0f;  // signed min amplitude (oscillation mode)
        float peakMax = 0.0f;  // signed max amplitude (oscillation mode)
    };
    std::vector<ScrollPixel> pixels(wInt);

    // LOD smoothstep blend over [kBlendStart, kBlendEnd] px/pt.
    // Peak data is fetched from kBlendStart so the bar→line morph begins before
    // the hard oscilloscope threshold.  lodBlend=0 → pure bar, lodBlend=1 → pure line.
    constexpr float kBlendStart = 2.50f;
    constexpr float kBlendEnd   = 7.20f;
    const float blendRaw = std::clamp(
        (m_pixelsPerPoint - kBlendStart) / (kBlendEnd - kBlendStart), 0.0f, 1.0f);
    const float lodBlend = blendRaw * blendRaw * (3.0f - 2.0f * blendRaw);  // smoothstep
    const bool usePeakData = (m_pixelsPerPoint >= kBlendStart);
    constexpr int PEAK_RATIO = TrackData::PEAK_POINTS_PER_SECOND
                               / static_cast<int>(DjEngine::WAVEFORM_POINTS_PER_SECOND);
    QVector<TrackData::PeakFrame> peakData;
    int peakSliceBase = 0;
    if (usePeakData && td->getPeakMipSize() > 0) {
        const int peakStart = (sliceStart - 4) * PEAK_RATIO;
        const int peakEnd   = (sliceEnd   + 4) * PEAK_RATIO;
        peakData = td->getPeakMipSlice(std::max(0, peakStart), peakEnd, &peakSliceBase);
    }

    const auto getPeakMinF = [&](int idx) -> float {
        const int local = idx - peakSliceBase;
        if (local < 0 || local >= peakData.size()) return 0.0f;
        return peakData[local].minSample / 127.0f;
    };
    const auto getPeakMaxF = [&](int idx) -> float {
        const int local = idx - peakSliceBase;
        if (local < 0 || local >= peakData.size()) return 0.0f;
        return peakData[local].maxSample / 127.0f;
    };

    // Mixxx-style no-wiggle lock: keep sample lookup on a deterministic visual
    // sample grid when zoomed out, so transient spikes do not morph per frame.
    const bool lockVisualSampleGrid = pixelsPerPoint <= 1.25;
    const double visualSamplesPerPixel =
        std::max(1.0, 1.0 / std::max(0.0001, pixelsPerPoint));
    const int subSamples = lockVisualSampleGrid ? 1 : 2;
    for (int x = 0; x < wInt; ++x) {
        const double dataPosRaw = centerIndexRender
            + (static_cast<double>(x) - wD * 0.5) / pixelsPerPoint;
        const double dataPos = lockVisualSampleGrid
            ? std::round(dataPosRaw / visualSamplesPerPixel) * visualSamplesPerPixel
            : dataPosRaw;
        float maxRms    = 0.0f;
        float sumLow    = 0.0f;
        float sumLowMid = 0.0f;
        float sumMid    = 0.0f;
        float sumHigh   = 0.0f;

        for (int s = 0; s < subSamples; ++s) {
            const double ofs = (subSamples == 1)
                ? 0.0
                : ((static_cast<double>(s) + 0.5) / static_cast<double>(subSamples) - 0.5) * 0.72;
            const double samplePos = dataPos + ofs;
            const double sampleFloor = std::floor(samplePos);
            const int i0 = static_cast<int>(sampleFloor) - 1;
            const float t = static_cast<float>(samplePos - sampleFloor);

            const auto& d0 = getD(i0);
            const auto& d1 = getD(i0+1);
            const auto& d2 = getD(i0+2);
            const auto& d3 = getD(i0+3);

            const float rms    = catmull(d0.rms,    d1.rms,    d2.rms,    d3.rms,    t);
            const float low    = catmull(d0.low,    d1.low,    d2.low,    d3.low,    t);
            const float lowMid = catmull(d0.lowMid, d1.lowMid, d2.lowMid, d3.lowMid, t);
            const float mid    = catmull(d0.mid,    d1.mid,    d2.mid,    d3.mid,    t);
            const float high   = catmull(d0.high,   d1.high,   d2.high,   d3.high,   t);

            maxRms = std::max(maxRms, rms);
            sumLow    += low;
            sumLowMid += lowMid;
            sumMid    += mid;
            sumHigh   += high;
        }

        const float invN = 1.0f / static_cast<float>(subSamples);
        pixels[x].rms    = maxRms;
        pixels[x].low    = sumLow    * invN;
        pixels[x].lowMid = sumLowMid * invN;
        pixels[x].mid    = sumMid    * invN;
        pixels[x].high   = sumHigh   * invN;

        // Peak mip: Catmull-Rom on high-res signed min/max data.
        // dataPosRaw is already computed above.
        if (usePeakData && !peakData.isEmpty()) {
            const double peakPos = dataPosRaw * static_cast<double>(PEAK_RATIO);
            const int pi  = static_cast<int>(std::floor(peakPos));
            const float pt2 = static_cast<float>(peakPos - std::floor(peakPos));
            pixels[x].peakMin = catmullSigned(getPeakMinF(pi-1), getPeakMinF(pi), getPeakMinF(pi+1), getPeakMinF(pi+2), pt2);
            pixels[x].peakMax = catmullSigned(getPeakMaxF(pi-1), getPeakMaxF(pi), getPeakMaxF(pi+1), getPeakMaxF(pi+2), pt2);
        }
    }

    // Lightweight horizontal smoothing to avoid blocky edges at high zoom-out.
    // peakMin/peakMax are excluded — smoothing destroys oscillation shape.
    if (wInt >= 3 && pixelsPerPoint < 0.95) {
        std::vector<ScrollPixel> smooth = pixels;
        for (int x = 1; x < wInt - 1; ++x) {
            smooth[x].rms    = pixels[x-1].rms    * 0.16f + pixels[x].rms    * 0.68f + pixels[x+1].rms    * 0.16f;
            smooth[x].low    = pixels[x-1].low    * 0.16f + pixels[x].low    * 0.68f + pixels[x+1].low    * 0.16f;
            smooth[x].lowMid = pixels[x-1].lowMid * 0.16f + pixels[x].lowMid * 0.68f + pixels[x+1].lowMid * 0.16f;
            smooth[x].mid    = pixels[x-1].mid    * 0.16f + pixels[x].mid    * 0.68f + pixels[x+1].mid    * 0.16f;
            smooth[x].high   = pixels[x-1].high   * 0.16f + pixels[x].high   * 0.68f + pixels[x+1].high   * 0.16f;
            // peakMin/peakMax intentionally not smoothed
        }
        pixels.swap(smooth);
    }

    // LOD-blended waveform renderer.
    //
    // lodBlend=0 (low zoom): symmetric DJ bars — glow edge (node 0), main body (node 1),
    //   bright spine (node 2).  Height driven by RMS amplitude.
    //
    // lodBlend=1 (high zoom): PCM oscilloscope thin line — node 1 only, positioned at
    //   the instantaneous signal midpoint (peakMax+peakMin)/2, ±lineHalfW pixels.
    //
    // In between: all geometry and alpha values are continuously interpolated so there
    // is no visible mode switch at any zoom level.
    const bool hasPeakData = usePeakData && !peakData.isEmpty();

    for (int x = 0; x < wInt; ++x) {
        const float fx = snapDevicePixelX(static_cast<double>(x) + 0.5);
        const int vIdx = x * 2;

        const float rms    = std::clamp(pixels[x].rms,    0.0f, 1.0f);
        const float low    = std::clamp(pixels[x].low,    0.0f, 1.0f);
        const float lowMid = std::clamp(pixels[x].lowMid, 0.0f, 1.0f);
        const float mid    = std::clamp(pixels[x].mid,    0.0f, 1.0f);
        const float high   = std::clamp(pixels[x].high,   0.0f, 1.0f);

        // Amplitude for color: blend RMS (bar mode) → peak-abs (line mode).
        float peakAbs = 0.0f;
        float signal  = 0.0f;
        if (hasPeakData) {
            const float rawMax = pixels[x].peakMax;
            const float rawMin = pixels[x].peakMin;
            peakAbs = std::clamp(std::max(std::abs(rawMax), std::abs(rawMin)), 0.0f, 1.0f);
            signal  = (rawMax + rawMin) * 0.5f;
        }
        const float amp = rms + (peakAbs - rms) * lodBlend;

        if (amp <= 0.0005f) {
            lowV[vIdx].set(fx, midY, 0, 0, 0, 0);    lowV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            lowMidV[vIdx].set(fx, midY, 0, 0, 0, 0); lowMidV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            midV[vIdx].set(fx, midY, 0, 0, 0, 0);    midV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            highV[vIdx].set(fx, midY, 0, 0, 0, 0);   highV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            continue;
        }

        const QColor c = mixRekordboxColor(low, lowMid, mid, high, amp);
        const uchar cr = static_cast<uchar>(c.red());
        const uchar cg = static_cast<uchar>(c.green());
        const uchar cb = static_cast<uchar>(c.blue());

        // Bar mode geometry.
        const float bodyAmp = amp * midY;
        const float glowAmp = bodyAmp * 1.04f;
        const float coreAmp = bodyAmp * 0.28f;

        // Line mode geometry.
        const float signalY   = midY - std::clamp(signal, -1.0f, 1.0f) * midY;
        const float lineHalfW = std::clamp(0.70f + m_pixelsPerPoint * 0.025f, 0.70f, 1.5f);

        // Morph body: bar edges [midY ± bodyAmp] → line edges [signalY ± lineHalfW].
        const float bodyTop = (midY - bodyAmp) + (signalY - lineHalfW - (midY - bodyAmp)) * lodBlend;
        const float bodyBot = (midY + bodyAmp) + (signalY + lineHalfW - (midY + bodyAmp)) * lodBlend;

        // Glow and spine fade out as lodBlend → 1.
        const uchar glowA  = static_cast<uchar>(18.0f  * (1.0f - lodBlend) + 0.5f);
        const uchar spineA = static_cast<uchar>(255.0f * (1.0f - lodBlend) + 0.5f);

        // Spine collapses toward midY as lodBlend → 1.
        const float coreScale = 1.0f - lodBlend;
        const uchar coreR = static_cast<uchar>(static_cast<int>(cr) + (255 - static_cast<int>(cr)) * 55 / 100);
        const uchar coreG = static_cast<uchar>(static_cast<int>(cg) + (255 - static_cast<int>(cg)) * 55 / 100);
        const uchar coreB = static_cast<uchar>(static_cast<int>(cb) + (255 - static_cast<int>(cb)) * 55 / 100);

        if (glowA > 0) {
            lowV[vIdx  ].set(fx, midY - glowAmp, cr, cg, cb, glowA);
            lowV[vIdx+1].set(fx, midY + glowAmp, cr, cg, cb, glowA);
        } else {
            lowV[vIdx  ].set(fx, midY, 0, 0, 0, 0);
            lowV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
        }

        lowMidV[vIdx  ].set(fx, bodyTop, cr, cg, cb, 252);
        lowMidV[vIdx+1].set(fx, bodyBot, cr, cg, cb, 252);

        if (spineA > 0) {
            midV[vIdx  ].set(fx, midY - coreAmp * coreScale, coreR, coreG, coreB, spineA);
            midV[vIdx+1].set(fx, midY + coreAmp * coreScale, coreR, coreG, coreB, spineA);
        } else {
            midV[vIdx  ].set(fx, midY, 0, 0, 0, 0);
            midV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
        }

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
        const double pps = pointsPerSec;

        // Prefer elastic BeatMarker grid; fall back to rigid grid.
        std::vector<TrackData::BeatMarker> beatGrid = td->getBeatGrid();
        const bool hasElasticGrid = !beatGrid.empty();

        const double ppp          = static_cast<double>(pixelsPerPoint);
        const double visiblePoints = w / ppp;
        const double leftSec       = (centerIndexRender - visiblePoints / 2.0) / pps;
        const double rightSec      = (centerIndexRender + visiblePoints / 2.0) / pps;

        // Anchor beat lines to a device-pixel-aligned center that is monotonically
        // non-decreasing.  floor() — not round() — is critical here: round() oscillates
        // at snap boundaries when the interpolated position has sub-frame backward jitter
        // (e.g. the audio timer fires slightly early and resets m_snapPosition a fraction
        // of a device pixel behind where linear interpolation placed it).  That 1-device-
        // pixel back-and-forth makes MSAA sample coverage alternate frame-to-frame, which
        // appears as bright/dark flickering.  floor() absorbs any backward jitter smaller
        // than one snap step and only ever steps forward.
        const double centerForBeats = std::floor(centerIndexRender * ppp * snapScale)
                                      / (ppp * snapScale);
        // One device pixel expressed in logical-pixel space.
        const float invDpr = 1.0f / static_cast<float>(snapScale);

        // Converts a continuous beat position to the LEFT edge of the device pixel it
        // falls on.  Using floor (not round) guarantees the resulting quad spans exactly
        // [N, N+1] device coordinates — never straddling two columns, which is what
        // caused the "±halfPx from center" approach to flicker on pixel boundaries.
        const auto beatPixelLeft = [ppp, snapScale, &centerForBeats, w](double beatPoint) -> float {
            const double bx = w / 2.0 + (beatPoint - centerForBeats) * ppp;
            return static_cast<float>(std::floor(bx * snapScale) / snapScale);
        };

        // Collect visible markers, separated into regular and downbeat lists.
        struct VisibleBeat { float xl; bool isDownbeat; int barNumber; };
        std::vector<VisibleBeat> visible;
        visible.reserve(256);

        if (hasElasticGrid) {
            auto cmp = [](const TrackData::BeatMarker& m, double t){
                return m.positionSec < t; };
            auto it = std::lower_bound(beatGrid.begin(), beatGrid.end(),
                                       leftSec - 0.5, cmp);
            for (; it != beatGrid.end() && it->positionSec <= rightSec + 0.5; ++it) {
                const float xl = beatPixelLeft(it->positionSec * pps);
                if (xl >= -invDpr && xl <= w)
                    visible.push_back({xl, it->isDownbeat, it->barNumber});
            }
        } else {
            double bpm          = td->getBpm();
            qint64 firstBeatSamp = td->getFirstBeatSample();
            double firstBeatSec  = static_cast<double>(firstBeatSamp) / sr;
            double beatPeriod    = 60.0 / bpm;
            int beatStart = static_cast<int>(std::floor((leftSec  - firstBeatSec) / beatPeriod));
            int beatEnd   = static_cast<int>(std::ceil ((rightSec - firstBeatSec) / beatPeriod));
            beatStart = std::max(beatStart, -1);
            beatEnd   = std::min(beatEnd,   100000);
            for (int b = beatStart; b <= beatEnd; ++b) {
                const float xl = beatPixelLeft((firstBeatSec + b * beatPeriod) * pps);
                if (xl >= -invDpr && xl <= w)
                    visible.push_back({xl, (b % 4 == 0), b / 4 + 1});
            }
        }

        // Count regular vs downbeat lines for allocation.
        int numRegular  = 0;
        int numDownbeat = 0;
        for (auto& v : visible) {
            if (v.isDownbeat) ++numDownbeat; else ++numRegular;
        }

        const float hF = static_cast<float>(height());

        // Beat lines: dark shadow backing + bright core.
        // Core is 2 device pixels wide — robust against sub-pixel positioning so
        // coverage is always full regardless of zoom level or display DPR.
        // Shadow is 6 device pixels wide (2 px either side of core) for contrast
        // against bright waveform peaks at beat positions.
        // All widths are expressed in logical pixels via invDpr so they are
        // physically constant across all zoom levels and screen densities.
        const float corePx   = 2.0f * invDpr;   // 2 device pixels
        const float shadowPx = 6.0f * invDpr;   // 6 device pixels (1 px each side beyond core)

        // ── Node 4: regular beat lines (shadow + white core) ──────────────────
        beatGeo->allocate(numRegular * 12);
        {
            auto* v = beatGeo->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                if (vb.isDownbeat) continue;
                const float cxl = vb.xl;
                const float cxr = cxl + corePx;
                const float sxl = cxl - (shadowPx - corePx) * 0.5f;
                const float sxr = cxl + (shadowPx + corePx) * 0.5f;
                v[idx++].set(sxl, 0.0f,  0,   0,   0, 120);
                v[idx++].set(sxl, hF,    0,   0,   0, 120);
                v[idx++].set(sxr, hF,    0,   0,   0, 120);
                v[idx++].set(sxl, 0.0f,  0,   0,   0, 120);
                v[idx++].set(sxr, hF,    0,   0,   0, 120);
                v[idx++].set(sxr, 0.0f,  0,   0,   0, 120);
                v[idx++].set(cxl, 0.0f, 220, 220, 220, 230);
                v[idx++].set(cxl, hF,   220, 220, 220, 230);
                v[idx++].set(cxr, hF,   220, 220, 220, 230);
                v[idx++].set(cxl, 0.0f, 220, 220, 220, 230);
                v[idx++].set(cxr, hF,   220, 220, 220, 230);
                v[idx++].set(cxr, 0.0f, 220, 220, 220, 230);
            }
        }

        // ── Node 5: downbeat lines (shadow + red core) ────────────────────────
        downGeo->allocate(numDownbeat * 12);
        {
            auto* v = downGeo->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                if (!vb.isDownbeat) continue;
                const float cxl = vb.xl;
                const float cxr = cxl + corePx;
                const float sxl = cxl - (shadowPx - corePx) * 0.5f;
                const float sxr = cxl + (shadowPx + corePx) * 0.5f;
                v[idx++].set(sxl, 0.0f,  0,   0,   0, 140);
                v[idx++].set(sxl, hF,    0,   0,   0, 140);
                v[idx++].set(sxr, hF,    0,   0,   0, 140);
                v[idx++].set(sxl, 0.0f,  0,   0,   0, 140);
                v[idx++].set(sxr, hF,    0,   0,   0, 140);
                v[idx++].set(sxr, 0.0f,  0,   0,   0, 140);
                v[idx++].set(cxl, 0.0f, 235,   0,   0, 245);
                v[idx++].set(cxl, hF,   235,   0,   0, 210);
                v[idx++].set(cxr, hF,   235,   0,   0, 210);
                v[idx++].set(cxl, 0.0f, 235,   0,   0, 245);
                v[idx++].set(cxr, hF,   235,   0,   0, 210);
                v[idx++].set(cxr, 0.0f, 235,   0,   0, 245);
            }
        }

        // ── Node 6: beat triangle markers — every beat line, top AND bottom ──
        // Downbeat: red,   triH=10, triW=4.5  (prominent)
        // Regular:  gray,  triH=7,  triW=3.5  (subtle)
        // Top triangle: base at y=0,  tip points down at y=+triH
        // Bottom triangle: base at y=hF, tip points up at y=hF-triH
        const float triHD = 10.0f, triWD = 4.5f;
        const float triHR =  7.0f, triWR = 3.5f;
        triGeo2->allocate(static_cast<int>(visible.size()) * 6);
        {
            auto* v = triGeo2->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                const float cx = vb.xl + invDpr;  // center of 2-device-pixel core
                if (vb.isDownbeat) {
                    v[idx++].set(cx - triWD, 0.0f,       230,   0,   0, 230);
                    v[idx++].set(cx + triWD, 0.0f,       230,   0,   0, 230);
                    v[idx++].set(cx,         triHD,       230,   0,   0, 195);
                    v[idx++].set(cx - triWD, hF,          230,   0,   0, 230);
                    v[idx++].set(cx + triWD, hF,          230,   0,   0, 230);
                    v[idx++].set(cx,         hF - triHD,  230,   0,   0, 195);
                } else {
                    v[idx++].set(cx - triWR, 0.0f,       185, 185, 185, 145);
                    v[idx++].set(cx + triWR, 0.0f,       185, 185, 185, 145);
                    v[idx++].set(cx,         triHR,       185, 185, 185, 100);
                    v[idx++].set(cx - triWR, hF,          185, 185, 185, 145);
                    v[idx++].set(cx + triWR, hF,          185, 185, 185, 145);
                    v[idx++].set(cx,         hF - triHR,  185, 185, 185, 100);
                }
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

        float xIn = snapDevicePixelX(w / 2.0 + (loopInPoint - centerIndexRender) * pixelsPerPoint);
        float xOut = snapDevicePixelX(w / 2.0 + (loopOutPoint - centerIndexRender) * pixelsPerPoint);

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
            const float x = snapDevicePixelX(w / 2.0 + (cuePoint - centerIndexRender) * pixelsPerPoint);
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

    // ── Hot cue triangle markers (top + bottom, in each cue's color) ──────
    {
        const float hF      = static_cast<float>(height());
        const float cTriH   = 9.0f;
        const float cTriW   = 4.0f;
        QSGGeometry* hotCueTriGeo = hotCueTriNode->geometry();
        // re-use visibleCues collected above — rebuild since the struct is local
        const QVariantList cuesAgain = m_engine->hotCues();
        struct CuePos { float x; uchar r, g, b; };
        std::vector<CuePos> cuePositions;
        cuePositions.reserve(static_cast<size_t>(cuesAgain.size()));
        for (const QVariant& qv : cuesAgain) {
            const QVariantMap m = qv.toMap();
            if (!m.value("set").toBool()) continue;
            const double cueSec = m.value("positionSec").toDouble();
            const float cx = snapDevicePixelX(w / 2.0 + (cueSec * pointsPerSec - centerIndexRender) * pixelsPerPoint);
            if (cx < 0.0f || cx > w) continue;
            QColor c(m.value("color").toString());
            if (!c.isValid()) c = QColor("#e04040");
            cuePositions.push_back({cx, static_cast<uchar>(c.red()), static_cast<uchar>(c.green()), static_cast<uchar>(c.blue())});
        }
        hotCueTriGeo->allocate(static_cast<int>(cuePositions.size()) * 6);
        {
            auto* v = hotCueTriGeo->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (const auto& cp : cuePositions) {
                v[idx++].set(cp.x - cTriW, 0.0f,        cp.r, cp.g, cp.b, 230);
                v[idx++].set(cp.x + cTriW, 0.0f,        cp.r, cp.g, cp.b, 230);
                v[idx++].set(cp.x,         cTriH,        cp.r, cp.g, cp.b, 190);
                v[idx++].set(cp.x - cTriW, hF,           cp.r, cp.g, cp.b, 230);
                v[idx++].set(cp.x + cTriW, hF,           cp.r, cp.g, cp.b, 230);
                v[idx++].set(cp.x,         hF - cTriH,   cp.r, cp.g, cp.b, 190);
            }
        }
        hotCueTriNode->markDirty(QSGNode::DirtyGeometry);
    }

    // ── Main cue point (gray line + orange triangles) ─────────────────────
    {
        const float hF = static_cast<float>(height());
        const double mainCueSec = m_engine->mainCueSec();
        QSGGeometry* mainCueLineGeo = mainCueLineNode->geometry();
        QSGGeometry* mainCueTriGeo  = mainCueTriNode->geometry();

        if (mainCueSec >= 0.0) {
            const float mx = snapDevicePixelX(w / 2.0 + (mainCueSec * pointsPerSec - centerIndexRender) * pixelsPerPoint);
            if (mx >= 0.0f && mx <= w) {
                mainCueLineGeo->allocate(2);
                auto* lv = mainCueLineGeo->vertexDataAsColoredPoint2D();
                lv[0].set(mx, 0.0f, 160, 160, 160, 210);
                lv[1].set(mx, hF,   160, 160, 160, 170);

                const float mTriH = 11.0f, mTriW = 5.0f;
                mainCueTriGeo->allocate(6);
                auto* tv = mainCueTriGeo->vertexDataAsColoredPoint2D();
                tv[0].set(mx - mTriW, 0.0f,        255, 145,  0, 240);
                tv[1].set(mx + mTriW, 0.0f,        255, 145,  0, 240);
                tv[2].set(mx,         mTriH,        255, 145,  0, 200);
                tv[3].set(mx - mTriW, hF,           255, 145,  0, 240);
                tv[4].set(mx + mTriW, hF,           255, 145,  0, 240);
                tv[5].set(mx,         hF - mTriH,   255, 145,  0, 200);
            } else {
                mainCueLineGeo->allocate(0);
                mainCueTriGeo->allocate(0);
            }
        } else {
            mainCueLineGeo->allocate(0);
            mainCueTriGeo->allocate(0);
        }
        mainCueLineNode->markDirty(QSGNode::DirtyGeometry);
        mainCueTriNode->markDirty(QSGNode::DirtyGeometry);
    }

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

            float x1 = snapDevicePixelX(w / 2.0 + (startPoint - centerIndexRender) * pixelsPerPoint);
            float x2 = snapDevicePixelX(w / 2.0 + (endPoint - centerIndexRender) * pixelsPerPoint);

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
