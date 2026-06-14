#include "ScrollingWaveformItem.h"
#include <QDebug>
#include <QSGGeometry>
#include <QSGMaterial>
#include <QSGSimpleTextureNode>
#include <QSGVertexColorMaterial>
#include <QQuickWindow>
#include <QColor>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Frequency-band RGB color blend.
//   low     → vivid red      (sub-bass / kick)
//   lowMid  → orange         (bass body / warmth)
//   mid     → yellow-lime    (snare / melody / vocals)
//   high    → electric cyan  (hi-hat / transients / air)
static inline QColor mixBandColor(float low, float lowMid, float mid, float high, float rms)
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

    // Higher exponents → dominant frequency band wins more decisively.
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

static QSGVertexColorMaterial* makeVertexColorMaterial(bool enableBlending = false)
{
    auto* material = new QSGVertexColorMaterial();
    if (enableBlending)
        material->setFlag(QSGMaterial::Blending, true);
    return material;
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
    // When the item moves to a new window, wire up scene-graph cleanup.
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* win) {
        if (win)
            connect(win, &QQuickWindow::sceneGraphInvalidated,
                    this, &ScrollingWaveformItem::cleanupSgResources,
                    Qt::DirectConnection);
    });

    // ≤15 fps coalescing of progressive-analysis data updates. Live playback
    // scrolling still repaints every frame via the normal update() path; this
    // only caps the rebuild rate driven by background analysis signals.
    m_dataUpdateThrottle = new QTimer(this);
    m_dataUpdateThrottle->setSingleShot(true);
    m_dataUpdateThrottle->setInterval(66);
    connect(m_dataUpdateThrottle, &QTimer::timeout, this, [this]() {
        m_forceUpdate = true;
        update();
    });
}

ScrollingWaveformItem::~ScrollingWaveformItem()
{
    // cleanupSgResources() is the authoritative path (render thread).
    // If we're destroyed without that signal, pool nodes still need freeing.
    for (auto* n : m_texNodePool) delete n;
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
        connect(m_engine, &DjEngine::trackEjected, this, &ScrollingWaveformItem::onTrackEjected);
        connect(m_engine, &DjEngine::loopChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::segmentsChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::mainCueChanged, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
    } else {
        // nothing to stop — FrameAnimation in QML will have stopped already
    }
    emit engineChanged();
    m_hasLastCenterIndexRender = false;
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::setPixelsPerPoint(float ppp)
{
    ppp = clampToZoomLevel(ppp);
    if (qFuzzyCompare(m_pixelsPerPoint, ppp)) return;
    m_pixelsPerPoint = ppp;
    emit pixelsPerPointChanged();
    m_hasLastCenterIndexRender = false;
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

void ScrollingWaveformItem::onTrackEjected()
{
    if (m_engine && m_engine->getTrackData())
        disconnect(m_engine->getTrackData(), nullptr, this, nullptr);
    m_rgbSliceBuf.clear();
    m_peakSliceBuf.clear();
    m_hasLastCenterIndexRender = false;
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::onTrackLoaded()
{
    if (m_engine && m_engine->getTrackData()) {
        connect(m_engine->getTrackData(), &TrackData::dataUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::rgbWaveformUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::dataCleared, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::bpmAnalyzed, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::peakMipUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::overviewRgbUpdated, this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
    }
    m_hasLastCenterIndexRender = false;
    m_forceUpdate = true;
    update();
}

void ScrollingWaveformItem::onDataUpdated()
{
    // Coalesce bursts of analysis data signals into at most one repaint per
    // throttle interval so a fast analyzer can't stall the render thread.
    if (m_dataUpdateThrottle && !m_dataUpdateThrottle->isActive())
        m_dataUpdateThrottle->start();
}

// ── Scene-graph resource helpers ──────────────────────────────────────────────

void ScrollingWaveformItem::cleanupSgResources()
{
    // Called on the render thread via sceneGraphInvalidated (DirectConnection).
    for (auto* t : m_allTextures) delete t;
    m_allTextures.clear();
    m_texCache.clear();
    for (auto* n : m_texNodePool) delete n;
    m_texNodePool.clear();
}

ScrollingWaveformItem::LabelTex
ScrollingWaveformItem::getLabelTex(const QString& text, bool downbeat, double dpr)
{
    const quint64 key = qHash(text) ^ (static_cast<quint64>(downbeat ? 0xFFFF0000u : 0u));
    auto it = m_texCache.find(key);
    if (it != m_texCache.end() && it->valid())
        return *it;

    const int px = static_cast<int>(std::round(10.0 * dpr));
    QFont font(QStringLiteral("monospace"));
    font.setPixelSize(px);
    font.setBold(false);
    QFontMetrics fm(font);
    const int imgW = fm.horizontalAdvance(text) + 4;
    const int imgH = fm.height() + 2;
    if (imgW <= 0 || imgH <= 0) return {};

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setFont(font);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setPen(QColor(255, 255, 255, 200));
        p.drawText(2, fm.ascent() + 1, text);
    }

    LabelTex lt;
    lt.tex  = window()->createTextureFromImage(img, QQuickWindow::TextureCanUseAtlas);
    lt.logW = static_cast<float>(imgW) / static_cast<float>(dpr);
    lt.logH = static_cast<float>(imgH) / static_cast<float>(dpr);
    m_allTextures.append(lt.tex);
    m_texCache[key] = lt;
    return lt;
}

ScrollingWaveformItem::LabelTex
ScrollingWaveformItem::getBadgeTex(const QString& text, const QColor& color,
                                    float logW, float logH, double dpr)
{
    const quint64 key = qHash(text) ^ (static_cast<quint64>(color.rgb()) << 20)
                        ^ (static_cast<quint64>(static_cast<int>(logW * 10)) << 50);
    auto it = m_texCache.find(key);
    if (it != m_texCache.end() && it->valid())
        return *it;

    const int imgW = std::max(1, static_cast<int>(std::round(logW * dpr)));
    const int imgH = std::max(1, static_cast<int>(std::round(logH * dpr)));
    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(dpr, dpr);
        p.setPen(QPen(QColor(0, 0, 0, 115), 1.0));
        p.setBrush(color);
        p.drawRoundedRect(QRectF(0.5, 0.5, logW - 1.0, logH - 1.0), 3.0, 3.0);

        QFont font(QStringLiteral("monospace"));
        font.setPixelSize(static_cast<int>(std::round(8.0 * dpr)));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRectF(0, 0, logW, logH), Qt::AlignCenter, text);
    }

    LabelTex lt;
    lt.tex  = window()->createTextureFromImage(img, QQuickWindow::TextureCanUseAtlas);
    lt.logW = logW;
    lt.logH = logH;
    m_allTextures.append(lt.tex);
    m_texCache[key] = lt;
    return lt;
}

void ScrollingWaveformItem::drainToPool(QSGNode* container)
{
    while (container->childCount() > 0) {
        auto* child = static_cast<QSGSimpleTextureNode*>(container->firstChild());
        container->removeChildNode(child);
        m_texNodePool.push_back(child);
    }
}

QSGSimpleTextureNode* ScrollingWaveformItem::getFromPool()
{
    if (!m_texNodePool.empty()) {
        auto* n = m_texNodePool.back();
        m_texNodePool.pop_back();
        return n;
    }
    auto* n = new QSGSimpleTextureNode();
    n->setOwnsTexture(false);
    n->setFiltering(QSGTexture::Linear);
    return n;
}

QSGNode* ScrollingWaveformItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    if (!m_engine || !m_engine->hasTrack() || !m_engine->getTrackData()) {
        if (oldNode) delete oldNode;
        return nullptr;
    }

    TrackData* td = m_engine->getTrackData();

    // Scene graph node order (back to front):
    //   0: audioRegionNode - subtle background fill for the audio region
    //   1: lowNode    - dark blue     (sub-bass / kick,    LP @ 110 Hz)
    //   2: lowMidNode  - gold/ocker    (bass body / warmth, BP 150–160 Hz)
    //   3: midNode     - orange/red    (snare / vocals,     BP 180–800 Hz)
    //   4: highNode    - pure white    (hi-hat / perc,      BP@2750 + HP@19k)
    //   5: beatNode    - regular beat lines (white, thin)
    //   6: downbeatNode- downbeat lines (red) + rectangular block markers
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

        // 0: audio region background fill (DrawTriangleStrip) — must be first (behind all)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(new QSGVertexColorMaterial());
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        makeStrip(rootNode);          // 1: low
        makeStrip(rootNode);          // 2: lowMid
        makeStrip(rootNode);          // 3: mid
        makeStrip(rootNode);          // 4: high
        makeQuadLinesNode(rootNode);  // 5: regular beat lines (DrawTriangles, white)
        makeQuadLinesNode(rootNode);  // 6: downbeat lines (DrawTriangles, red)

        // 7: downbeat block markers — top + bottom (DrawTriangles)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawTriangles);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(new QSGVertexColorMaterial());
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        // 8: loop overlay rectangle/rails (DrawTriangles)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawTriangles);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(makeVertexColorMaterial(true));
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        // 9: loop in/out markers (DrawTriangles quads, same approach as beat lines)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawTriangles);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(makeVertexColorMaterial(true));
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        // 10: segment strip at the bottom (DrawTriangles)
        makeQuadLinesNode(rootNode);

        // 11: hotcue marker shadow (DrawLines, 4.2px)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawLines);
            g->setLineWidth(4.2f);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(new QSGVertexColorMaterial());
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        // 12: hotcue markers (DrawLines, 2.6px)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawLines);
            g->setLineWidth(2.6f);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(new QSGVertexColorMaterial());
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        // 13: hotcue triangle markers (DrawTriangles, colored top+bottom)
        makeQuadLinesNode(rootNode);

        // 14: main cue line (DrawLines, 2px)
        {
            auto* n = new QSGGeometryNode();
            auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            g->setDrawingMode(QSGGeometry::DrawLines);
            g->setLineWidth(2.0f);
            n->setGeometry(g);
            n->setFlag(QSGNode::OwnsGeometry);
            n->setMaterial(new QSGVertexColorMaterial());
            n->setFlag(QSGNode::OwnsMaterial);
            rootNode->appendChildNode(n);
        }

        // 15: main cue triangles (DrawTriangles, orange top+bottom)
        makeQuadLinesNode(rootNode);

        // 16: retired — was track-start boundary line (kept as empty placeholder)
        makeQuadLinesNode(rootNode);

        // 17: beat label text container — plain parent, children are QSGSimpleTextureNode
        rootNode->appendChildNode(new QSGNode());

        // 18: cue/hotcue badge text container — same pattern
        rootNode->appendChildNode(new QSGNode());
    }

    auto* audioRegionNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowNode          = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* lowMidNode       = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* midNode          = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* highNode         = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));
    auto* beatNode         = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(5));
    auto* downbeatNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(6));
    auto* triNode          = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(7));
    auto* loopFillNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(8));
    auto* loopLineNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(9));
    auto* segmentNode      = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(10));
    auto* hotCueShadowNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(11));
    auto* hotCueNode       = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(12));
    auto* hotCueTriNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(13));
    auto* mainCueLineNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(14));
    auto* mainCueTriNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(15));
    // node 16: retired placeholder (was track-start boundary line)
    QSGNode* beatLabelNode = rootNode->childAtIndex(17);  // container for beat label textures
    QSGNode* badgeNode     = rootNode->childAtIndex(18);  // container for cue badge textures

    int wInt = static_cast<int>(std::lround(width()));
    if (wInt <= 0) return rootNode;

    const float w             = static_cast<float>(wInt);
    const double wD           = static_cast<double>(wInt);
    const float midY          = static_cast<float>(height()) / 2.0f;
    // Reserve space for the top and bottom downbeat blocks (8 px) plus a small gap.
    // The waveform amplitude is scaled to this half-height so it never touches the edges.
    constexpr float kWaveMargin = 11.0f;
    const float waveHalf = std::max(0.0f, midY - kWaveMargin);
    const double dpr          = window() ? std::max(1.0, static_cast<double>(window()->effectiveDevicePixelRatio())) : 1.0;
    const double snapScale    = dpr;
    const double pointsPerSec = m_engine->waveformPointsPerSecond();
    const double tempoRatio   = m_engine->getTempoRatio();
    const double pixelsPerPoint = static_cast<double>(m_pixelsPerPoint) / std::max(0.0001, tempoRatio);
    const double rawPlayheadSec = m_engine->isScratchVisualActive()
        ? m_engine->getPlayheadPositionAtomic()
        : static_cast<double>(m_engine->getVisualPosition());
    const double rawCenterIndexRender = rawPlayheadSec * pointsPerSec;
    double centerIndexRender = rawCenterIndexRender;
    const bool continuousPlayback = m_engine->isPlaying() && !m_engine->isScratchVisualActive();
    if (continuousPlayback && m_hasLastCenterIndexRender && pointsPerSec > 0.0) {
        const double pointsPerDevicePixel = 1.0 / std::max(0.0001, pixelsPerPoint * snapScale);
        const double jitterTolerancePoints = pointsPerDevicePixel * 2.0;
        const double delta = rawCenterIndexRender - m_lastCenterIndexRender;
        const bool oppositeMicroStep = m_engine->isReverse()
            ? (delta > 0.0 && delta <= jitterTolerancePoints)
            : (delta < 0.0 && -delta <= jitterTolerancePoints);
        if (oppositeMicroStep)
            centerIndexRender = m_lastCenterIndexRender;
    }
    m_lastCenterIndexRender = centerIndexRender;
    m_hasLastCenterIndexRender = true;

    const auto snapDevicePixelX = [snapScale](double x) -> float {
        return static_cast<float>(std::round(x * snapScale) / snapScale);
    };

    // Chunk-wise waveform data access: fetch only what is visible (+guard).
    const double visiblePoints = wD / std::max(0.0001, pixelsPerPoint);
    const int guardPoints = 96;
    const int sliceStart = static_cast<int>(std::floor(centerIndexRender - visiblePoints * 0.5)) - guardPoints - 4;
    const int sliceEnd = static_cast<int>(std::ceil(centerIndexRender + visiblePoints * 0.5)) + guardPoints + 4;
    // fillRgbWaveformSlice reuses m_rgbSliceBuf's capacity — no heap allocation once
    // the buffer has grown to the typical slice size (viewport + guard points).
    const int sliceBaseIndex = td->fillRgbWaveformSlice(m_rgbSliceBuf, sliceStart, sliceEnd);
    const QVector<TrackData::RgbWaveformFrame>& rgbData = m_rgbSliceBuf;
    // rgbData can be empty when the viewport is entirely in the pre-roll zone
    // (before sample 0). In that case, skip waveform bars but still render
    // the beatgrid, hotcues, and the audio region background.
    const bool hasWaveformData = !rgbData.isEmpty();

    // ── Audio region background fill ─────────────────────────────────────────
    // Renders a subtle fill behind the audio content so the track region is
    // visually distinct from the pre-roll / post-track void without any lines.
    {
        const float hF    = midY * 2.0f;
        const int waveSize = td->getRgbWaveformSize();
        QSGGeometry* arGeo = audioRegionNode->geometry();
        if (waveSize > 0) {
            const float ax1 = snapDevicePixelX(wD / 2.0 + (0.0 - centerIndexRender) * pixelsPerPoint);
            const float ax2 = snapDevicePixelX(wD / 2.0 + (static_cast<double>(waveSize) - centerIndexRender) * pixelsPerPoint);
            const float rx1 = std::max(0.0f, ax1);
            const float rx2 = std::min(w, ax2);
            if (rx2 > rx1) {
                arGeo->allocate(4);
                auto* av = arGeo->vertexDataAsColoredPoint2D();
                av[0].set(rx1, 0.0f, 42, 42, 42, 255);
                av[1].set(rx1, hF,   42, 42, 42, 255);
                av[2].set(rx2, 0.0f, 42, 42, 42, 255);
                av[3].set(rx2, hF,   42, 42, 42, 255);
            } else {
                arGeo->allocate(0);
            }
        } else {
            arGeo->allocate(0);
        }
        audioRegionNode->markDirty(QSGNode::DirtyGeometry);
    }

    if (hasWaveformData) {
    lowNode   ->geometry()->allocate(wInt * 2);
    lowMidNode->geometry()->allocate(wInt * 2);
    midNode   ->geometry()->allocate(wInt * 2);
    highNode  ->geometry()->allocate(wInt * 2);

    auto* lowV    = lowNode   ->geometry()->vertexDataAsColoredPoint2D();
    auto* lowMidV = lowMidNode->geometry()->vertexDataAsColoredPoint2D();
    auto* midV    = midNode   ->geometry()->vertexDataAsColoredPoint2D();
    auto* highV   = highNode  ->geometry()->vertexDataAsColoredPoint2D();

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

    // Always use peak min/max data — waveform shape is driven by the real audio
    // envelope at every zoom level, never by a separate rendering mode.
    const bool usePeakData = (td->getPeakMipSize() > 0);
    constexpr int PEAK_RATIO = TrackData::PEAK_POINTS_PER_SECOND
                               / static_cast<int>(DjEngine::WAVEFORM_POINTS_PER_SECOND);
    int peakSliceBase = 0;
    if (usePeakData) {
        const int peakStart = (sliceStart - 4) * PEAK_RATIO;
        const int peakEnd   = (sliceEnd   + 4) * PEAK_RATIO;
        // fillPeakMipSlice reuses m_peakSliceBuf's capacity — no heap allocation once warm.
        peakSliceBase = td->fillPeakMipSlice(m_peakSliceBuf, std::max(0, peakStart), peakEnd);
    } else {
        m_peakSliceBuf.clear();
    }
    const QVector<TrackData::PeakFrame>& peakData = m_peakSliceBuf;

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

    // No-wiggle lock: keep sample lookup on a deterministic visual sample grid
    // when zoomed out, so transient spikes do not morph per frame.
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

        // Peak envelope: Catmull-Rom when zoomed in (< 1 peak frame/pixel),
        // min/max binning when zoomed out (many peak frames per pixel).
        // Both paths preserve the real waveform silhouette at their resolution.
        if (usePeakData && !peakData.isEmpty()) {
            if (pixelsPerPoint >= 1.0) {
                const double peakPos = dataPosRaw * static_cast<double>(PEAK_RATIO);
                const int pi  = static_cast<int>(std::floor(peakPos));
                const float pt2 = static_cast<float>(peakPos - std::floor(peakPos));
                // Clamp to invariants (peakMax >= 0, peakMin <= 0) — Catmull-Rom
                // can overshoot its control points, making peakMax negative or
                // peakMin positive, which inverts bodyTop/bodyBot and flickers.
                pixels[x].peakMax = std::max(0.0f, catmullSigned(getPeakMaxF(pi-1), getPeakMaxF(pi), getPeakMaxF(pi+1), getPeakMaxF(pi+2), pt2));
                pixels[x].peakMin = std::min(0.0f, catmullSigned(getPeakMinF(pi-1), getPeakMinF(pi), getPeakMinF(pi+1), getPeakMinF(pi+2), pt2));
            } else {
                // Snap the window center to the same deterministic grid as band
                // data — prevents per-frame jitter from continuously-shifting
                // peak frame inclusions as dataPosRaw drifts between frames.
                const double snappedCenter = std::round(dataPosRaw / visualSamplesPerPixel) * visualSamplesPerPixel;
                const double halfSpan = 0.5 / std::max(0.0001, pixelsPerPoint);
                const int p0 = std::max(0, static_cast<int>(std::floor((snappedCenter - halfSpan) * PEAK_RATIO)));
                const int p1 = static_cast<int>(std::ceil((snappedCenter + halfSpan) * PEAK_RATIO));
                float pMin = 0.0f, pMax = 0.0f;
                for (int pi = p0; pi <= p1; ++pi) {
                    const float mn = getPeakMinF(pi);
                    const float mx = getPeakMaxF(pi);
                    if (mn < pMin) pMin = mn;
                    if (mx > pMax) pMax = mx;
                }
                pixels[x].peakMin = pMin;
                pixels[x].peakMax = pMax;
            }
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

    // Single waveform rendering style — consistent at all zoom levels.
    // Shape: peak min/max envelope (actual audio silhouette).
    // Color: frequency-band weighted RGB blend (same at every zoom).
    const bool hasPeakData = !peakData.isEmpty();

    for (int x = 0; x < wInt; ++x) {
        const float fx = snapDevicePixelX(static_cast<double>(x) + 0.5);
        const int vIdx = x * 2;

        const float rms    = std::clamp(pixels[x].rms,    0.0f, 1.0f);
        const float low    = std::clamp(pixels[x].low,    0.0f, 1.0f);
        const float lowMid = std::clamp(pixels[x].lowMid, 0.0f, 1.0f);
        const float mid    = std::clamp(pixels[x].mid,    0.0f, 1.0f);
        const float high   = std::clamp(pixels[x].high,   0.0f, 1.0f);

        float bodyTop, bodyBot, signalY, peakAbs;
        if (hasPeakData) {
            // Enforce invariants: peakMax ∈ [0,1], peakMin ∈ [-1,0].
            // With these bounds: bodyTop ≤ midY ≤ bodyBot (never inverted).
            const float pMax = std::clamp(pixels[x].peakMax, 0.0f, 1.0f);
            const float pMin = std::clamp(pixels[x].peakMin, -1.0f, 0.0f);
            bodyTop = midY - pMax * waveHalf;
            bodyBot = midY - pMin * waveHalf;
            signalY = midY - (pMax + pMin) * 0.5f * waveHalf;
            peakAbs = std::max(pMax, -pMin);
        } else {
            peakAbs = rms;
            bodyTop = midY - rms * waveHalf;
            bodyBot = midY + rms * waveHalf;
            signalY = midY;
        }

        if (peakAbs <= 0.0005f) {
            lowV[vIdx].set(fx, midY, 0, 0, 0, 0);    lowV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            lowMidV[vIdx].set(fx, midY, 0, 0, 0, 0); lowMidV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            midV[vIdx].set(fx, midY, 0, 0, 0, 0);    midV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            highV[vIdx].set(fx, midY, 0, 0, 0, 0);   highV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
            continue;
        }

        const QColor c = mixBandColor(low, lowMid, mid, high, peakAbs);
        const uchar cr = static_cast<uchar>(c.red());
        const uchar cg = static_cast<uchar>(c.green());
        const uchar cb = static_cast<uchar>(c.blue());

        // Outer glow: slightly wider outline, semi-transparent.
        lowV[vIdx  ].set(fx, bodyTop - 0.5f, cr, cg, cb, 18);
        lowV[vIdx+1].set(fx, bodyBot + 0.5f, cr, cg, cb, 18);

        // Main waveform body: filled peak envelope.
        lowMidV[vIdx  ].set(fx, bodyTop, cr, cg, cb, 252);
        lowMidV[vIdx+1].set(fx, bodyBot, cr, cg, cb, 252);

        // Central spine: bright strip following the signal midpoint.
        const float coreAmp = peakAbs * waveHalf * 0.28f;
        const uchar coreR = static_cast<uchar>(static_cast<int>(cr) + (255 - static_cast<int>(cr)) * 55 / 100);
        const uchar coreG = static_cast<uchar>(static_cast<int>(cg) + (255 - static_cast<int>(cg)) * 55 / 100);
        const uchar coreB = static_cast<uchar>(static_cast<int>(cb) + (255 - static_cast<int>(cb)) * 55 / 100);
        midV[vIdx  ].set(fx, signalY - coreAmp, coreR, coreG, coreB, 255);
        midV[vIdx+1].set(fx, signalY + coreAmp, coreR, coreG, coreB, 255);

        highV[vIdx  ].set(fx, midY, 0, 0, 0, 0);
        highV[vIdx+1].set(fx, midY, 0, 0, 0, 0);
    }

    lowNode   ->markDirty(QSGNode::DirtyGeometry);
    lowMidNode->markDirty(QSGNode::DirtyGeometry);
    midNode   ->markDirty(QSGNode::DirtyGeometry);
    highNode  ->markDirty(QSGNode::DirtyGeometry);

    } else {
        // Pre-roll zone: no waveform data, render empty strips.
        lowNode   ->geometry()->allocate(0);
        lowMidNode->geometry()->allocate(0);
        midNode   ->geometry()->allocate(0);
        highNode  ->geometry()->allocate(0);
        lowNode   ->markDirty(QSGNode::DirtyGeometry);
        lowMidNode->markDirty(QSGNode::DirtyGeometry);
        midNode   ->markDirty(QSGNode::DirtyGeometry);
        highNode  ->markDirty(QSGNode::DirtyGeometry);
    }

    // ── Beat-grid rendering ──────────────────────────────────────────────────
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
        // Publish render-thread values for beatLabels() on the main thread.
        // Atomics guarantee beatLabels() uses the same coordinate origin as these lines.
        m_lastCenterForBeats.store(centerForBeats, std::memory_order_relaxed);
        m_lastBeatPpp.store(ppp, std::memory_order_relaxed);
        m_lastRenderedWidth.store(static_cast<double>(w), std::memory_order_relaxed);
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
        struct VisibleBeat { float xl; bool isDownbeat; int barNumber; int beatInBar; };
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
                    visible.push_back({xl, it->isDownbeat, it->barNumber, it->beatInBar});
            }
        } else {
            double bpm          = td->getBpm();
            qint64 firstBeatSamp = td->getFirstBeatSample();
            double firstBeatSec  = static_cast<double>(firstBeatSamp) / sr;
            double beatPeriod    = 60.0 / bpm;
            int beatStart = static_cast<int>(std::floor((leftSec  - firstBeatSec) / beatPeriod));
            int beatEnd   = static_cast<int>(std::ceil ((rightSec - firstBeatSec) / beatPeriod));
            beatStart = std::max(beatStart, -200);
            beatEnd   = std::min(beatEnd,   100000);
            for (int b = beatStart; b <= beatEnd; ++b) {
                const int mod4 = ((b % 4) + 4) % 4;
                const int barIdx = static_cast<int>(std::floor(static_cast<double>(b) / 4.0));
                const float xl = beatPixelLeft((firstBeatSec + b * beatPeriod) * pps);
                if (xl >= -invDpr && xl <= w)
                    visible.push_back({xl, mod4 == 0, barIdx + 1, mod4 + 1});
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

        // ── Node 6: downbeat rectangular blocks — top AND bottom edges ──────
        // CDJ/Engine DJ style: solid red rectangles mark bar starts.
        // Regular beats get no symbol — their white line is sufficient.
        const float blockW = 6.0f * invDpr;   // 6 device pixels, centered on beat line
        const float blockH = 8.0f;             // logical pixels tall
        triGeo2->allocate(numDownbeat * 12);   // 2 rects × 2 triangles × 3 verts
        {
            auto* v = triGeo2->vertexDataAsColoredPoint2D();
            int idx = 0;
            for (auto& vb : visible) {
                if (!vb.isDownbeat) continue;
                const float cx  = vb.xl + corePx * 0.5f;
                const float bxl = cx - blockW * 0.5f;
                const float bxr = cx + blockW * 0.5f;
                // Top block: y = [0, blockH]
                v[idx++].set(bxl, 0.0f,   230,   0,   0, 255);
                v[idx++].set(bxr, 0.0f,   230,   0,   0, 255);
                v[idx++].set(bxr, blockH, 230,   0,   0, 210);
                v[idx++].set(bxl, 0.0f,   230,   0,   0, 255);
                v[idx++].set(bxr, blockH, 230,   0,   0, 210);
                v[idx++].set(bxl, blockH, 230,   0,   0, 210);
                // Bottom block: y = [hF - blockH, hF]
                v[idx++].set(bxl, hF - blockH, 230,   0,   0, 210);
                v[idx++].set(bxr, hF - blockH, 230,   0,   0, 210);
                v[idx++].set(bxr, hF,           230,   0,   0, 255);
                v[idx++].set(bxl, hF - blockH,  230,   0,   0, 210);
                v[idx++].set(bxr, hF,            230,   0,   0, 255);
                v[idx++].set(bxl, hF,            230,   0,   0, 255);
            }
        }

        // ── Beat label textures (node 16) ──────────────────────────────────────
        // Only downbeats get bar numbers. Positioned just below and to the right
        // of the top red block so they feel anchored to the marker.
        {
            drainToPool(beatLabelNode);
            constexpr float kMinLabelSpacing = 20.0f;
            constexpr float kBlockH  = 8.0f;
            constexpr float kGapX    = 2.0f;  // gap between label right-edge and beat line
            constexpr float kGapY    = 1.0f;  // gap between label bottom-edge and block top
            float lastLabelX = -1e6f;
            for (const auto& vb : visible) {
                if (!vb.isDownbeat) continue;
                if (vb.xl - lastLabelX < kMinLabelSpacing) continue;
                const LabelTex lt = getLabelTex(QString::number(vb.barNumber), true, snapScale);
                if (!lt.valid()) continue;
                // Anchor bottom-left: label sits just above the bottom red block,
                // right-edge aligned slightly left of the beat line.
                const float lx = std::clamp(vb.xl - lt.logW - kGapX, 0.0f, w - lt.logW);
                const float ly = hF - kBlockH - lt.logH - kGapY;
                auto* tn = getFromPool();
                tn->setTexture(lt.tex);
                tn->setOwnsTexture(false);
                tn->setRect(QRectF(static_cast<double>(lx), static_cast<double>(ly),
                                   static_cast<double>(lt.logW), static_cast<double>(lt.logH)));
                beatLabelNode->appendChildNode(tn);
                lastLabelX = vb.xl;
            }
        }
    } else {
        beatGeo ->allocate(0);
        downGeo ->allocate(0);
        triGeo2 ->allocate(0);
        drainToPool(beatLabelNode);
    }

    beatNode    ->markDirty(QSGNode::DirtyGeometry);
    downbeatNode->markDirty(QSGNode::DirtyGeometry);
    triNode     ->markDirty(QSGNode::DirtyGeometry);

    // ── Loop overlay rendering ───────────────────────────────────────────────
    // Authoritative loop renderer. The QML overlay in EnlargedWaveform is
    // intentionally empty — all loop drawing happens here.
    //
    // Completed loops render loopStart→loopEnd. While only LOOP IN is set, draw
    // a live preview from loopStart to the exact LOOP OUT position that would be
    // committed right now, including quantize/minimum-length handling.
    QSGGeometry* loopFillGeo = loopFillNode->geometry();
    QSGGeometry* loopLineGeo = loopLineNode->geometry();

    const bool completedLoopSet = m_engine->loopInPosition() < m_engine->loopOutPosition();
    const bool pendingLoopSet = m_engine->loopInSet() && !completedLoopSet;
    const bool loopSet = completedLoopSet || pendingLoopSet;
    if (loopSet) {
        const bool   active      = completedLoopSet && m_engine->loopActive();
        const bool   pending     = pendingLoopSet;
        const double loopInSec   = m_engine->loopInPosition();
        const double loopOutSec  = completedLoopSet
            ? m_engine->loopOutPosition()
            : m_engine->loopPreviewOutPosition();

        const double loopInPoint  = loopInSec  * pointsPerSec;
        const double loopOutPoint = loopOutSec * pointsPerSec;

        // Actual screen positions (may be outside [0,w] when off-screen).
        const float xLoopIn  = snapDevicePixelX(w / 2.0 + (loopInPoint  - centerIndexRender) * pixelsPerPoint);
        const float xLoopOut = snapDevicePixelX(w / 2.0 + (loopOutPoint - centerIndexRender) * pixelsPerPoint);

        // Clamped fill extents — swap to handle reverse direction correctly.
        const float drawLeft  = std::max(0.0f, std::min(xLoopIn,  xLoopOut));
        const float drawRight = std::min(w,    std::max(xLoopIn,  xLoopOut));

        // Active loops use a bright cyan tint. Pending loops use the same family
        // at lower alpha so LOOP IN + the future LOOP OUT point stay readable
        // without looking like the loop is already armed.
        const uchar fa = active ? 100 : (pending ? 58 : 70);
        const float hF = static_cast<float>(height());

        if (drawRight > drawLeft + 0.5f) {
            const int fillVerts = 6;
            const int railVerts = active ? 0 : 12;
            loopFillGeo->allocate(fillVerts + railVerts);
            auto* fv = loopFillGeo->vertexDataAsColoredPoint2D();
            int fi = 0;

            auto appendRect = [&](float x1, float y1, float x2, float y2,
                                  uchar r, uchar g, uchar b, uchar a) {
                fv[fi++].set(x1, y1, r, g, b, a);
                fv[fi++].set(x1, y2, r, g, b, a);
                fv[fi++].set(x2, y2, r, g, b, a);
                fv[fi++].set(x1, y1, r, g, b, a);
                fv[fi++].set(x2, y2, r, g, b, a);
                fv[fi++].set(x2, y1, r, g, b, a);
            };

            if (active) {
                appendRect(drawLeft, 0.0f, drawRight, hF, 90, 180, 255, fa);
            } else if (pending) {
                appendRect(drawLeft, 0.0f, drawRight, hF, 45, 150, 230, fa);
                const float railH = std::max(1.0f, 2.0f / static_cast<float>(snapScale));
                appendRect(drawLeft, 0.0f, drawRight, railH, 135, 225, 255, 150);
                appendRect(drawLeft, hF - railH, drawRight, hF, 135, 225, 255, 150);
            } else {
                appendRect(drawLeft, 0.0f, drawRight, hF, 10, 18, 24, fa);
                const float railH = std::max(1.0f, 2.0f / static_cast<float>(snapScale));
                appendRect(drawLeft, 0.0f, drawRight, railH, 110, 170, 210, 120);
                appendRect(drawLeft, hF - railH, drawRight, hF, 110, 170, 210, 120);
            }
        } else {
            loopFillGeo->allocate(0);
        }

        // Marker lines: rendered as 2-device-pixel quads (DrawTriangles) for
        // guaranteed consistent thickness on all GPUs and DPR settings — identical
        // approach to beat/downbeat lines (see comment at makeQuadLinesNode).
        //
        // Active/pending: dark shadow (6px) + bright cyan core (2px) = 12 verts/marker.
        // Inactive:       dim core only (2px) = 6 verts/marker.
        // Neither marker is drawn when its actual position is off-screen.
        const float markerW = 2.0f / static_cast<float>(snapScale);
        const float shadowW = 6.0f / static_cast<float>(snapScale);

        const bool inOnScreen  = xLoopIn  >= 0.0f && xLoopIn  <= w;
        const bool outOnScreen = xLoopOut >= 0.0f && xLoopOut <= w;
        const int  vertsPerMarker = (active || pending) ? 12 : 6;
        loopLineGeo->allocate((inOnScreen ? vertsPerMarker : 0) +
                              (outOnScreen ? vertsPerMarker : 0));

        if (loopLineGeo->vertexCount() > 0) {
            auto* lv = loopLineGeo->vertexDataAsColoredPoint2D();
            int vi = 0;

            auto drawMarker = [&](float xPos) {
                const float cxl = xPos;
                const float cxr = xPos + markerW;
                if (active || pending) {
                    const float sxl = cxl - (shadowW - markerW) * 0.5f;
                    const float sxr = cxl + (shadowW + markerW) * 0.5f;
                    const uchar coreAlpha = active ? 240 : 220;
                    lv[vi++].set(sxl, 0.0f, 0,   0,   0,   90);
                    lv[vi++].set(sxl, hF,   0,   0,   0,   90);
                    lv[vi++].set(sxr, hF,   0,   0,   0,   90);
                    lv[vi++].set(sxl, 0.0f, 0,   0,   0,   90);
                    lv[vi++].set(sxr, hF,   0,   0,   0,   90);
                    lv[vi++].set(sxr, 0.0f, 0,   0,   0,   90);
                    lv[vi++].set(cxl, 0.0f, 130, 220, 255, coreAlpha);
                    lv[vi++].set(cxl, hF,   130, 220, 255, coreAlpha);
                    lv[vi++].set(cxr, hF,   130, 220, 255, coreAlpha);
                    lv[vi++].set(cxl, 0.0f, 130, 220, 255, coreAlpha);
                    lv[vi++].set(cxr, hF,   130, 220, 255, coreAlpha);
                    lv[vi++].set(cxr, 0.0f, 130, 220, 255, coreAlpha);
                } else {
                    lv[vi++].set(cxl, 0.0f, 110, 170, 210, 95);
                    lv[vi++].set(cxl, hF,   110, 170, 210, 95);
                    lv[vi++].set(cxr, hF,   110, 170, 210, 95);
                    lv[vi++].set(cxl, 0.0f, 110, 170, 210, 95);
                    lv[vi++].set(cxr, hF,   110, 170, 210, 95);
                    lv[vi++].set(cxr, 0.0f, 110, 170, 210, 95);
                }
            };

            if (inOnScreen)  drawMarker(xLoopIn);
            if (outOnScreen) drawMarker(xLoopOut);
        }
    } else {
        loopFillGeo->allocate(0);
        loopLineGeo->allocate(0);
    }

    loopFillNode->markDirty(QSGNode::DirtyGeometry);
    loopLineNode->markDirty(QSGNode::DirtyGeometry);

    // ── Hotcue markers (lines, triangles, badges — single pass) ───────────
    drainToPool(badgeNode);
    {
        struct VisibleCue { float x; uchar r, g, b; QColor color; int index; };
        std::vector<VisibleCue> visibleCues;

        const QVariantList cues = m_engine->hotCues();
        visibleCues.reserve(static_cast<size_t>(cues.size()));
        for (int i = 0; i < cues.size(); ++i) {
            const QVariantMap m = cues[i].toMap();
            if (!m.value("set").toBool()) continue;
            const double cueSec = m.value("positionSec").toDouble();
            const float cx = snapDevicePixelX(w / 2.0 + (cueSec * pointsPerSec - centerIndexRender) * pixelsPerPoint);
            if (cx < 0.0f || cx > w) continue;
            QColor c(m.value("color").toString());
            if (!c.isValid()) c = QColor("#e04040");
            visibleCues.push_back({cx,
                static_cast<uchar>(c.red()), static_cast<uchar>(c.green()), static_cast<uchar>(c.blue()),
                c, i});
        }

        QSGGeometry* hotCueShadowGeo = hotCueShadowNode->geometry();
        QSGGeometry* hotCueGeo       = hotCueNode->geometry();
        QSGGeometry* hotCueTriGeo    = hotCueTriNode->geometry();
        const float hF     = static_cast<float>(height());
        const float cTriH  = 9.0f;
        const float cTriW  = 4.0f;
        const int   n      = static_cast<int>(visibleCues.size());

        hotCueShadowGeo->allocate(n * 2);
        hotCueGeo      ->allocate(n * 2);
        hotCueTriGeo   ->allocate(n * 6);

        auto* shadowVtx = hotCueShadowGeo->vertexDataAsColoredPoint2D();
        auto* lineVtx   = hotCueGeo      ->vertexDataAsColoredPoint2D();
        auto* triVtx    = hotCueTriGeo   ->vertexDataAsColoredPoint2D();
        int si = 0, li = 0, ti = 0;

        constexpr float kBadgeW = 20.0f, kBadgeH = 13.0f;
        for (const auto& vc : visibleCues) {
            shadowVtx[si++].set(vc.x, 0.0f, 0, 0, 0, 170);
            shadowVtx[si++].set(vc.x, hF,   0, 0, 0, 140);

            lineVtx[li++].set(vc.x, 0.0f, vc.r, vc.g, vc.b, 255);
            lineVtx[li++].set(vc.x, hF,   vc.r, vc.g, vc.b, 220);

            triVtx[ti++].set(vc.x - cTriW, 0.0f,       vc.r, vc.g, vc.b, 230);
            triVtx[ti++].set(vc.x + cTriW, 0.0f,       vc.r, vc.g, vc.b, 230);
            triVtx[ti++].set(vc.x,         cTriH,       vc.r, vc.g, vc.b, 190);
            triVtx[ti++].set(vc.x - cTriW, hF,          vc.r, vc.g, vc.b, 230);
            triVtx[ti++].set(vc.x + cTriW, hF,          vc.r, vc.g, vc.b, 230);
            triVtx[ti++].set(vc.x,         hF - cTriH,  vc.r, vc.g, vc.b, 190);

            // Badge texture above the top triangle
            const QString label = QString::number(vc.index + 1);
            const LabelTex lt = getBadgeTex(label, vc.color, kBadgeW, kBadgeH, snapScale);
            if (lt.valid()) {
                const float bx = std::clamp(vc.x - lt.logW * 0.5f, 0.0f, w - lt.logW);
                auto* tn = getFromPool();
                tn->setTexture(lt.tex);
                tn->setOwnsTexture(false);
                tn->setRect(QRectF(static_cast<double>(bx), static_cast<double>(cTriH + 1.0f),
                                   static_cast<double>(lt.logW), static_cast<double>(lt.logH)));
                badgeNode->appendChildNode(tn);
            }
        }

        hotCueShadowNode->markDirty(QSGNode::DirtyGeometry);
        hotCueNode      ->markDirty(QSGNode::DirtyGeometry);
        hotCueTriNode   ->markDirty(QSGNode::DirtyGeometry);
        badgeNode       ->markDirty(QSGNode::DirtyForceUpdate);
    }

    // ── Main cue point (gray line + orange triangles + CUE badge) ───────────
    {
        const float hF = static_cast<float>(height());
        const double mainCueSec = m_engine->mainCueSec();
        QSGGeometry* mainCueLineGeo = mainCueLineNode->geometry();
        QSGGeometry* mainCueTriGeo  = mainCueTriNode->geometry();

        // Allow pre-roll cue positions (negative seconds)
        if (mainCueSec >= -DjEngine::PRE_ROLL_SECONDS) {
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

                // CUE badge just below the top triangle
                constexpr float kCueBadgeW = 28.0f, kCueBadgeH = 13.0f;
                const LabelTex lt = getBadgeTex(QStringLiteral("CUE"),
                                                QColor(255, 145, 0),
                                                kCueBadgeW, kCueBadgeH, snapScale);
                if (lt.valid()) {
                    const float bx = std::clamp(mx - lt.logW * 0.5f, 0.0f, w - lt.logW);
                    auto* tn = getFromPool();
                    tn->setTexture(lt.tex);
                    tn->setOwnsTexture(false);
                    tn->setRect(QRectF(static_cast<double>(bx), static_cast<double>(mTriH + 1.0f),
                                       static_cast<double>(lt.logW), static_cast<double>(lt.logH)));
                    badgeNode->appendChildNode(tn);
                    badgeNode->markDirty(QSGNode::DirtyForceUpdate);
                }
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

    // Node 16 retired — audio start/end boundary lines removed.
    // The audio region background fill (node 0) provides the visual separation.

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

QVariantList ScrollingWaveformItem::beatLabels() const
{
    QVariantList result;
    if (!m_engine)
        return result;

    TrackData* td = m_engine->getTrackData();
    if (!td || !td->isBpmAnalyzed())
        return result;

    // Read the values written by the render thread in updatePaintNode().
    // Using these exact values guarantees our x-positions match the beatgrid lines pixel-for-pixel.
    const double centerForBeats = m_lastCenterForBeats.load(std::memory_order_relaxed);
    const double ppp            = m_lastBeatPpp.load(std::memory_order_relaxed);
    const double w              = m_lastRenderedWidth.load(std::memory_order_relaxed);
    if (w <= 0.0 || ppp <= 0.0)
        return result;

    const double pointsPerSec = m_engine->waveformPointsPerSecond();
    const double visiblePts   = w / ppp;
    const double leftSec      = (centerForBeats - visiblePts * 0.5) / pointsPerSec;
    const double rightSec     = (centerForBeats + visiblePts * 0.5) / pointsPerSec;

    // Same formula as beatPixelLeft() in updatePaintNode — no floor-snap here because
    // Canvas text is centered; sub-pixel accuracy is sufficient for centering.
    const auto beatX = [&](double beatSec) -> double {
        return w * 0.5 + (beatSec * pointsPerSec - centerForBeats) * ppp;
    };

    const std::vector<TrackData::BeatMarker> beatGrid = td->getBeatGrid();
    const bool hasElastic = !beatGrid.empty();

    constexpr double kMinSpacingPx = 20.0;
    double lastLabelX = -1e9;

    auto addMarker = [&](double beatSec, bool isDownbeat, int barNumber, int beatInBar) {
        const double x = beatX(beatSec);
        if (x < -20.0 || x > w + 20.0)
            return;
        if (x - lastLabelX < kMinSpacingPx)
            return;
        lastLabelX = x;
        QVariantMap m;
        m[QStringLiteral("x")]          = x;
        m[QStringLiteral("text")]       = isDownbeat ? QString::number(barNumber)
                                                     : QString::number(beatInBar);
        m[QStringLiteral("isDownbeat")] = isDownbeat;
        result.append(m);
    };

    if (hasElastic) {
        auto cmp = [](const TrackData::BeatMarker& mk, double t){ return mk.positionSec < t; };
        auto it = std::lower_bound(beatGrid.begin(), beatGrid.end(), leftSec - 0.5, cmp);
        for (; it != beatGrid.end() && it->positionSec <= rightSec + 0.5; ++it)
            addMarker(it->positionSec, it->isDownbeat, it->barNumber, it->beatInBar);
    } else {
        const double bpm          = td->getBpm();
        const double sr           = td->getSampleRate();
        const double firstBeatSec = static_cast<double>(td->getFirstBeatSample()) / sr;
        const double beatPeriod   = 60.0 / bpm;
        const int beatStart = std::max(static_cast<int>(std::floor((leftSec  - firstBeatSec) / beatPeriod)), -200);
        const int beatEnd   = std::min(static_cast<int>(std::ceil ((rightSec - firstBeatSec) / beatPeriod)), 100000);
        for (int b = beatStart; b <= beatEnd; ++b) {
            const int mod4 = ((b % 4) + 4) % 4;
            const int barIdx = static_cast<int>(std::floor(static_cast<double>(b) / 4.0));
            addMarker(firstBeatSec + b * beatPeriod, mod4 == 0, barIdx + 1, mod4 + 1);
        }
    }

    return result;
}
