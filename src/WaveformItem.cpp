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
    //   0: lowPeakNode  - dark blue halo  (bass, base layer)
    //   1: lowRmsNode   - bright blue body (bass, solid core)
    //   2: midNode      - amber strip      (mids)
    //   3: highNode     - white strip      (highs)
    //   4: cursorNode   - playhead line    (DrawLines, on top)
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

    auto* lowPeakNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
    auto* lowRmsNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    auto* midNode     = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(2));
    auto* highNode    = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(3));
    auto* cursorNode  = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(4));

    if (m_geometryChanged || true)
    {
        QVector<TrackData::FrequencyData> data = m_engine->getTrackData()->getWaveformData();
        int currentDataPoints = data.size();
        float globalMax = m_engine->getTrackData()->getGlobalMaxPeak();
        if (globalMax < 0.001f) globalMax = 0.001f;

        const float w = static_cast<float>(width());
        const float h = static_cast<float>(height());
        const float midY = h / 2.0f;

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

        lowPeakNode->geometry()->allocate(maxPixels * 2);
        lowRmsNode ->geometry()->allocate(maxPixels * 2);
        midNode    ->geometry()->allocate(maxPixels * 2);
        highNode   ->geometry()->allocate(maxPixels * 2);

        auto* lowPeakV = lowPeakNode->geometry()->vertexDataAsColoredPoint2D();
        auto* lowRmsV  = lowRmsNode ->geometry()->vertexDataAsColoredPoint2D();
        auto* midV     = midNode    ->geometry()->vertexDataAsColoredPoint2D();
        auto* highV    = highNode   ->geometry()->vertexDataAsColoredPoint2D();

        // Pixel binning: aggregate data points to pixel width.
        struct PixelBin {
            float lowPeak = 0.0f;
            float lowRms  = 0.0f;
            float midRms  = 0.0f;
            float highRms = 0.0f;
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

            double sumLowRms = 0.0, sumMidRms = 0.0, sumHighRms = 0.0;
            int count = 0;
            for (int d = dataStart; d < dataEnd; ++d) {
                if (data[d].lowPeak > bins[x].lowPeak) bins[x].lowPeak = data[d].lowPeak;
                sumLowRms  += data[d].lowRms;
                sumMidRms  += data[d].midRms;
                sumHighRms += data[d].highRms;
                ++count;
            }
            if (count > 0) {
                const float inv = 1.0f / static_cast<float>(count);
                bins[x].lowRms  = static_cast<float>(sumLowRms)  * inv;
                bins[x].midRms  = static_cast<float>(sumMidRms)  * inv;
                bins[x].highRms = static_cast<float>(sumHighRms) * inv;
            }
        }

        // Draw: global normalisation + pow(0.65) + attenuation factors.
        for (int x = 0; x < maxPixels; ++x) {
            const float fx = static_cast<float>(x);

            // Global normalisation (overview shows absolute dynamic structure).
            // pow(0.65): more contrast than sqrt, keeps quiet passages visible.
            auto normPow = [&](float v) {
                return std::pow(std::min(1.0f, v / globalMax), 0.65f);
            };

            float lowPeakN = normPow(bins[x].lowPeak);
            float lowRmsN  = normPow(bins[x].lowRms);
            float midN     = normPow(bins[x].midRms);
            float highN    = normPow(bins[x].highRms);

            // Minimum visibility: silent passages render as a hairline.
            if (bins[x].lowPeak > 0.0001f) {
                lowPeakN = std::max(lowPeakN, 0.02f);
                lowRmsN  = std::max(lowRmsN,  0.01f);
            }

            // Attenuation factors:
            //   LOW peak halo : 0.95 -> full headroom, bass dominates
            //   LOW RMS core  : 0.88 -> slightly smaller than halo
            //   MID           : 0.62 -> orange reaches at most 62% of bass height
            //   HIGH          : 0.38 -> white strip is a thin needle
            float lowPeakY = lowPeakN * midY * 0.95f;
            float lowRmsY  = lowRmsN  * midY * 0.88f;
            float midY_    = midN     * midY * 0.62f;
            float highY_   = highN    * midY * 0.38f;

            const int vIdx = x * 2;

            // Layer 0: LOW PEAK - dark blue halo (alpha 120)
            lowPeakV[vIdx  ].set(fx, midY - lowPeakY,  30,  80, 220, 120);
            lowPeakV[vIdx+1].set(fx, midY + lowPeakY,  30,  80, 220, 120);

            // Layer 1: LOW RMS - bright blue core (alpha 220)
            lowRmsV[vIdx  ].set(fx, midY - lowRmsY,  60, 150, 255, 220);
            lowRmsV[vIdx+1].set(fx, midY + lowRmsY,  60, 150, 255, 220);

            // Layer 2: MID - amber/orange (alpha 200)
            midV[vIdx  ].set(fx, midY - midY_, 255, 140, 0, 200);
            midV[vIdx+1].set(fx, midY + midY_, 255, 140, 0, 200);

            // Layer 3: HIGH - white with slight blue tint (alpha 204)
            highV[vIdx  ].set(fx, midY - highY_, 240, 240, 255, 204);
            highV[vIdx+1].set(fx, midY + highY_, 240, 240, 255, 204);
        }

        // Cursor / playhead
        float progX = m_engine->getProgress() * w;
        auto* cVerts = cursorNode->geometry()->vertexDataAsColoredPoint2D();
        cVerts[0].set(progX, 0.0f, 255, 255, 255, 220);
        cVerts[1].set(progX, h,    255, 255, 255, 220);

        lowPeakNode->markDirty(QSGNode::DirtyGeometry);
        lowRmsNode ->markDirty(QSGNode::DirtyGeometry);
        midNode    ->markDirty(QSGNode::DirtyGeometry);
        highNode   ->markDirty(QSGNode::DirtyGeometry);
        cursorNode ->markDirty(QSGNode::DirtyGeometry);
        m_geometryChanged = false;
    }

    return rootNode;
}
