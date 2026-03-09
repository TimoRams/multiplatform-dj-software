#include "WaveformItem.h"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QDebug>

WaveformItem::WaveformItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

DjEngine* WaveformItem::engine() const
{
    return m_engine;
}

void WaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine) return;

    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }

    m_engine = engine;

    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded, this, &WaveformItem::onTrackLoaded);
        connect(m_engine, &DjEngine::progressChanged, this, &WaveformItem::onProgressChanged);
    }

    emit engineChanged();
    
    m_geometryChanged = true;
    update();
}

void WaveformItem::onTrackLoaded()
{
    if (m_engine && m_engine->getTrackData()) {
        connect(m_engine->getTrackData(), &TrackData::dataUpdated, this, &WaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(m_engine->getTrackData(), &TrackData::dataCleared, this, &WaveformItem::onDataUpdated, Qt::UniqueConnection);
    }
    m_geometryChanged = true;
    update();
}

void WaveformItem::onDataUpdated()
{
    m_geometryChanged = true;
    update();
}

void WaveformItem::onProgressChanged()
{
    update();
}

QSGNode* WaveformItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    if (!m_engine || !m_engine->getTrackData() || m_engine->getTrackData()->getWaveformData().isEmpty()) {
        if (oldNode) delete oldNode;
        return nullptr;
    }

    // Scene graph node order (back to front):
    //   0: lowNode     - dark blue     (sub-bass / kick,    LP @ 110 Hz)
    //   1: lowMidNode  - gold/ocker    (bass body / warmth, BP 150–160 Hz)
    //   2: midNode     - orange/red    (snare / vocals,     BP 180–800 Hz)
    //   3: highNode    - pure white    (hi-hat / perc,      BP@2750 + HP@19k)
    //   4: cursorNode  - playhead line (DrawLines, on top)
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

        // 4: Cursor/Playhead (DrawLines)
        auto* cursorNode = new QSGGeometryNode();
        auto* cursorGeo  = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 2);
        cursorGeo->setDrawingMode(QSGGeometry::DrawLines);
        cursorGeo->setLineWidth(2.0f);
        cursorNode->setGeometry(cursorGeo);
        cursorNode->setFlag(QSGNode::OwnsGeometry);
        cursorNode->setMaterial(new QSGVertexColorMaterial());
        cursorNode->setFlag(QSGNode::OwnsMaterial);
        rootNode->appendChildNode(cursorNode);
    }

    auto* lowNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowMidNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* midNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* highNode   = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* cursorNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));

    if (m_geometryChanged || true)
    {
        QVector<TrackData::FrequencyData> data = m_engine->getTrackData()->getWaveformData();
        int currentDataPoints = data.size();

        const float w = static_cast<float>(width());
        const float h = static_cast<float>(height());
        const float baseline = m_rectified ? h : h / 2.0f;
        const float maxBarH  = m_rectified ? h : h / 2.0f;

        float durationSeconds = m_engine->getDuration();
        if (durationSeconds <= 0.0f) durationSeconds = 1.0f;

        const float pointsPerSec    = 150.0f;
        int totalExpectedPoints     = static_cast<int>(durationSeconds * pointsPerSec);
        if (totalExpectedPoints < currentDataPoints) totalExpectedPoints = currentDataPoints;

        int targetPoints = static_cast<int>(w);
        if (targetPoints < 100) targetPoints = 100;

        float analysisProgress = totalExpectedPoints > 0
            ? static_cast<float>(currentDataPoints) / static_cast<float>(totalExpectedPoints)
            : 1.0f;
        int maxPixels = static_cast<int>(targetPoints * analysisProgress);
        if (maxPixels <= 0) maxPixels = 1;

        lowNode   ->geometry()->allocate(maxPixels * 2);
        lowMidNode->geometry()->allocate(maxPixels * 2);
        midNode   ->geometry()->allocate(maxPixels * 2);
        highNode  ->geometry()->allocate(maxPixels * 2);

        auto* lowV    = lowNode   ->geometry()->vertexDataAsColoredPoint2D();
        auto* lowMidV = lowMidNode->geometry()->vertexDataAsColoredPoint2D();
        auto* midV    = midNode   ->geometry()->vertexDataAsColoredPoint2D();
        auto* highV   = highNode  ->geometry()->vertexDataAsColoredPoint2D();

        // Pixel binning: aggregate data points to pixel width (max within bin).
        struct PixelBin {
            float low    = 0.0f;
            float lowMid = 0.0f;
            float mid    = 0.0f;
            float high   = 0.0f;
        };
        std::vector<PixelBin> bins(maxPixels);

        for (int x = 0; x < maxPixels; ++x) {
            float relStart = static_cast<float>(x)     / static_cast<float>(targetPoints);
            float relEnd   = static_cast<float>(x + 1) / static_cast<float>(targetPoints);
            int dataStart  = static_cast<int>(relStart * totalExpectedPoints);
            int dataEnd    = static_cast<int>(relEnd   * totalExpectedPoints);
            if (dataStart >= currentDataPoints) continue;
            if (dataEnd   >  currentDataPoints) dataEnd = currentDataPoints;
            if (dataStart == dataEnd)           dataEnd = dataStart + 1;

            for (int d = dataStart; d < dataEnd; ++d) {
                if (data[d].low    > bins[x].low)    bins[x].low    = data[d].low;
                if (data[d].lowMid > bins[x].lowMid) bins[x].lowMid = data[d].lowMid;
                if (data[d].mid    > bins[x].mid)    bins[x].mid    = data[d].mid;
                if (data[d].high   > bins[x].high)   bins[x].high   = data[d].high;
            }
        }

        // Draw 4 STACKED layers (back to front).
        // Each band adds its height ON TOP of the previous one so all 4 colors
        // are visible as distinct stripes (Rekordbox-style).
        //
        // Rectified mode: baseline at bottom, bars grow up.
        // Normal mode:    baseline at centre, bars grow symmetrically up+down.
        for (int x = 0; x < maxPixels; ++x) {
            const float fx = static_cast<float>(x);
            const int vIdx = x * 2;

            // Raw band heights
            float hLow    = bins[x].low    * maxBarH;
            float hLowMid = bins[x].lowMid * maxBarH;
            float hMid    = bins[x].mid    * maxBarH;
            float hHigh   = bins[x].high   * maxBarH;

            // Stack from inside out: high innermost, low outermost
            float stackHigh   = hHigh;
            float stackMid    = hHigh + hMid;
            float stackLowMid = hHigh + hMid + hLowMid;
            float stackLow    = hHigh + hMid + hLowMid + hLow;

            // Clamp total to available height
            if (stackLow > maxBarH) {
                float scale = maxBarH / stackLow;
                stackHigh   *= scale;
                stackMid    *= scale;
                stackLowMid *= scale;
                stackLow     = maxBarH;
            }

            const float lowBot    = m_rectified ? baseline : baseline + stackLow;
            const float lowMidBot = m_rectified ? baseline : baseline + stackLowMid;
            const float midBot    = m_rectified ? baseline : baseline + stackMid;
            const float highBot   = m_rectified ? baseline : baseline + stackHigh;

            // Layer 0 (background): LOW — dark blue — outermost band
            lowV[vIdx  ].set(fx, baseline - stackLow,   0, 0, 255, 220);
            lowV[vIdx+1].set(fx, lowBot,                 0, 0, 255, 220);

            // Layer 1: LOWMID — gold/ocker
            lowMidV[vIdx  ].set(fx, baseline - stackLowMid, 255, 170, 0, 200);
            lowMidV[vIdx+1].set(fx, lowMidBot,               255, 170, 0, 200);

            // Layer 2: MID — orange/red
            midV[vIdx  ].set(fx, baseline - stackMid, 255, 68, 0, 200);
            midV[vIdx+1].set(fx, midBot,              255, 68, 0, 200);

            // Layer 3 (foreground): HIGH — pure white — innermost band
            highV[vIdx  ].set(fx, baseline - stackHigh, 255, 255, 255, 220);
            highV[vIdx+1].set(fx, highBot,               255, 255, 255, 220);
        }

        // Cursor / playhead
        float progX = m_engine->getProgress() * w;
        auto* cVerts = cursorNode->geometry()->vertexDataAsColoredPoint2D();
        cVerts[0].set(progX, 0.0f, 255, 255, 255, 220);
        cVerts[1].set(progX, h,    255, 255, 255, 220);

        lowNode   ->markDirty(QSGNode::DirtyGeometry);
        lowMidNode->markDirty(QSGNode::DirtyGeometry);
        midNode   ->markDirty(QSGNode::DirtyGeometry);
        highNode  ->markDirty(QSGNode::DirtyGeometry);
        cursorNode->markDirty(QSGNode::DirtyGeometry);
        m_geometryChanged = false;
    }

    return rootNode;
}
