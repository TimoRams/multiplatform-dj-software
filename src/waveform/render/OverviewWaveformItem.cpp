#include "OverviewWaveformItem.h"
#include "waveform/WaveformVisualStyle.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

QColor visualColor(const waveform_visual::Rgb8& color, float opacity = 1.0f)
{
    return QColor(color[0], color[1], color[2], std::clamp(
        static_cast<int>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f)),
        0, 255));
}

// Shared visual primitive for the compact and expanded overview.  Its
// component ranges come from WaveformVisualStyle, the same mapping used by
// scrolling tiles and their fallback overview texture.
void paintVisualColumn(QPainter* painter, int x, double top, double bottom,
                       const waveform_visual::WaveformVisual& visual,
                       float opacityScale = 1.0f)
{
    const auto draw = [&](const waveform_visual::Rgb8& color, float opacity,
                          float begin, float end) {
        if (opacity <= 0.0f || end <= begin)
            return;
        const double y = top + (bottom - top) * begin;
        const double h = (bottom - top) * (end - begin);
        painter->fillRect(QRectF(x, y, 1.0, std::max(1.0, h)),
                          visualColor(color, opacity * opacityScale));
    };
    draw(visual.geometryColor, visual.geometryOpacity, 0.0f, 1.0f);
    for (std::uint8_t index = 0; index < visual.componentCount; ++index) {
        const auto& component = visual.components[index];
        draw(component.color, component.opacity, component.begin, component.end);
    }
}

struct OverviewBin {
    float rms    = 0.0f;   // max rms in bin (peak energy)
    float rmsSum = 0.0f;   // sum for mean — used for transient detection
    float bass   = 0.0f;
    float mid    = 0.0f;
    float treble = 0.0f;
    int   count  = 0;      // number of source frames in this bin
};

struct RenderCol {
    QColor color;
    float bodyH = 0.0f;
    float tipH  = 0.0f;   // edge highlight height
    float coreH = 0.0f;   // bright center detail, matching the scrolling waveform
    float energy = 0.0f;
};

} // namespace

OverviewWaveformItem::OverviewWaveformItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setOpaquePainting(false);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    // The live-resize policy temporarily hides this item until its dimensions
    // settle. Prefer the cheap FBO resize path for the final transition rather
    // than preserving pixels that will be repainted immediately afterwards.
    setPerformanceHint(QQuickPaintedItem::FastFBOResizing, true);

    m_updateThrottle = new QTimer(this);
    m_updateThrottle->setSingleShot(true);
    m_updateThrottle->setInterval(m_updateIntervalMs);
    connect(m_updateThrottle, &QTimer::timeout, this, [this]() { update(); });
    m_resizeThrottle = new QTimer(this);
    m_resizeThrottle->setSingleShot(true);
    m_resizeThrottle->setInterval(120);
    connect(m_resizeThrottle, &QTimer::timeout, this, [this]() {
        if (!m_resizeDeferred)
            return;
        m_resizeDeferred = false;
        emit resizeDeferredChanged();
        update();
    });
}

void OverviewWaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine)
        return;

    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);

    m_engine = engine;

    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded,    this, &OverviewWaveformItem::onTrackLoaded,    Qt::UniqueConnection);
        connect(m_engine, &DjEngine::trackEjected,   this, &OverviewWaveformItem::onTrackEjected,   Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged,  this, &OverviewWaveformItem::onHotCuesChanged,  Qt::UniqueConnection);
    }

    emit engineChanged();
    update();
}

void OverviewWaveformItem::setRectified(bool v)
{
    if (m_rectified == v)
        return;

    m_rectified = v;
    emit rectifiedChanged();
    update();
}

void OverviewWaveformItem::setRenderStyle(int style)
{
    const int normalized = static_cast<int>(waveform_visual::normalizeStyle(style));
    if (m_renderStyle == normalized)
        return;
    m_renderStyle = normalized;
    // This item owns only rendered pixels.  The neutral overview snapshot and
    // all analysis/cache data remain untouched.
    m_frameCache = {};
    emit renderStyleChanged();
    update();
}

void OverviewWaveformItem::setUpdateIntervalMs(int intervalMs)
{
    const int bounded = std::clamp(intervalMs, 16, 2000);
    if (m_updateIntervalMs == bounded)
        return;
    m_updateIntervalMs = bounded;
    m_updateThrottle->setInterval(m_updateIntervalMs);
    emit updateIntervalMsChanged();
}

void OverviewWaveformItem::geometryChange(const QRectF& newGeometry,
                                          const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size())
        return;

    if (!m_resizeDeferred) {
        m_resizeDeferred = true;
        emit resizeDeferredChanged();
    }
    m_resizeThrottle->start();
}

void OverviewWaveformItem::onTrackEjected()
{
    if (m_engine && m_engine->getTrackData())
        disconnect(m_engine->getTrackData(), nullptr, this, nullptr);
    m_frameCache = QImage();
    update();
}

void OverviewWaveformItem::onTrackLoaded()
{
    m_frameCache = QImage();

    if (!m_engine || !m_engine->hasTrack() || !m_engine->getTrackData()) {
        update();
        return;
    }

    auto* td = m_engine->getTrackData();
    connect(td, &TrackData::rgbWaveformUpdated, this, &OverviewWaveformItem::onRgbDataChanged,  Qt::UniqueConnection);
    connect(td, &TrackData::dataCleared,        this, &OverviewWaveformItem::onRgbDataChanged,  Qt::UniqueConnection);
    // Overview arrives once after cache load — repaint immediately when ready.
    connect(td, &TrackData::overviewRgbUpdated, this, &OverviewWaveformItem::onOverviewRgbUpdated, Qt::UniqueConnection);
    update();
}

void OverviewWaveformItem::onRgbDataChanged()
{
    // During progressive analysis rgbWaveformUpdated fires with every new chunk.
    // This item does not render a moving playhead, so transport progress must not
    // trigger a repaint. Throttle analysis updates while paint() still needs to
    // bin the growing full-resolution data.
    // Once overviewRgbUpdated fires, paint() switches to the pre-downsampled path
    // and subsequent calls are O(kOverviewBins), so the throttle becomes a no-op cost.
    if (!m_updateThrottle->isActive())
        m_updateThrottle->start();
}

void OverviewWaveformItem::onOverviewRgbUpdated()
{
    update();
}

void OverviewWaveformItem::onHotCuesChanged()
{
    // Cue pins must jump immediately — bypass the throttle.
    update();
}

void OverviewWaveformItem::paintCompactOverview(QPainter* painter,
                                           const QVector<TrackData::RgbWaveformFrame>& frames,
                                           int drawWidth, int w, int h)
{
    const float baseline = static_cast<float>(h - 1);
    const float maxBarH  = static_cast<float>(h - 2);

    // One flat rectangle per column: min/max-derived height, one colour read
    // straight off this bucket's dominant frame. No glow, no highlight, no
    // second pass — those were what made this strip look shaded/gradiented.
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(Qt::NoPen);

    for (int x = 0; x < drawWidth; ++x) {
        const int i0 = static_cast<int>((static_cast<int64_t>(x) * frames.size()) / std::max(1, drawWidth));
        int i1 = static_cast<int>((static_cast<int64_t>(x + 1) * frames.size()) / std::max(1, drawWidth));
        i1 = std::max(i0 + 1, std::min(i1, static_cast<int>(frames.size())));

        // Height uses the bucket's general loudness (mean+peak), not just its
        // loudest instant — a single transient (hi-hat, click) shouldn't yank
        // one column to a wildly different height than its neighbours.
        // Colour is an rms-weighted mean across the whole bucket rather than
        // whichever single frame happened to be loudest: picking one "winner"
        // frame's colour made the strip jump between unrelated hues from
        // column to column at busy/transient-heavy moments. A weighted mean
        // gives every column one stable, smoothly-varying colour instead.
        float peakRms = 0.0f;
        float rmsSum = 0.0f;
        float weightedBass = 0.0f;
        float weightedMid = 0.0f, weightedTreble = 0.0f, colorWeight = 0.0f;
        for (int i = i0; i < i1; ++i) {
            const auto& f = frames[i];
            peakRms = std::max(peakRms, f.rms);
            rmsSum += f.rms;
            const float weight = std::max(0.01f, f.rms);
            weightedBass += f.bass * weight;
            weightedMid += f.mid * weight;
            weightedTreble += f.treble * weight;
            colorWeight += weight;
        }
        if (peakRms <= 0.001f)
            continue;

        const float meanRms = rmsSum / static_cast<float>(std::max(1, i1 - i0));
        const float energy = waveform_visual::foldedEnergy(meanRms, peakRms);
        const float barH = std::clamp(
            waveform_visual::logarithmicAmplitude(energy), 0.018f, 1.0f) * maxBarH;
        const float invWeight = 1.0f / std::max(0.01f, colorWeight);
        const auto visual = waveform_visual::map(
            waveform_visual::normalizeStyle(m_renderStyle),
            {weightedBass * invWeight, weightedMid * invWeight,
             weightedTreble * invWeight, energy});
        paintVisualColumn(painter, x, baseline - barH, baseline, visual, 0.95f);
    }

    // Cue markers
    if (!m_engine)
        return;

    const float durationSec = std::max(0.001f, m_engine->getDuration());
    const QVariantList cues = m_engine->hotCues();
    for (const QVariant& v : cues) {
        const QVariantMap m = v.toMap();
        if (!m.value("set").toBool())
            continue;

        const double cueSec = m.value("positionSec").toDouble();
        const float progress = std::clamp(static_cast<float>(cueSec / durationSec), 0.0f, 1.0f);
        const float x = progress * static_cast<float>(w);

        QColor c(m.value("color").toString());
        if (!c.isValid())
            c = QColor("#e04040");
        painter->setPen(QPen(c, 1.5));
        painter->drawLine(QPointF(x, 0.0), QPointF(x, static_cast<float>(h)));
    }
}

void OverviewWaveformItem::paintCompactOverviewLines(QPainter* painter,
                                                const WaveformLineStoreSnapshot& snapshot,
                                                int w, int h)
{
    if (!snapshot.chunks || snapshot.totalLineCount == 0)
        return;
    m_overviewHeights.resize(w);
    m_overviewVisuals.resize(w);
    std::fill(m_overviewHeights.begin(), m_overviewHeights.end(), 0.0f);

    const auto lineAt = [&snapshot](std::uint32_t index) -> const WaveformLine* {
        const auto chunk = snapshot.chunkAt(index / snapshot.chunkSize);
        if (!chunk || !chunk->lines) return nullptr;
        const auto local = index - chunk->firstLineIndex;
        return local < chunk->lines->size() ? &(*chunk->lines)[local] : nullptr;
    };

    const float maxBarH = static_cast<float>(h - 2);
    for (int x = 0; x < w; ++x) {
        const auto begin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * snapshot.totalLineCount) / w);
        const auto end = std::max(begin + 1u, static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(x + 1) * snapshot.totalLineCount) / w));
        float peakAmplitude = 0.0f;
        float amplitudeSum = 0.0f;
        std::uint64_t rms = 0, bass = 0, mid = 0, treble = 0, weight = 0;
        int sampleCount = 0;
        for (auto index = begin; index < std::min(end, snapshot.totalLineCount); ++index) {
            const auto* line = lineAt(index);
            if (!line) continue;
            const auto magnitude = static_cast<std::uint32_t>(std::max(
                std::abs(static_cast<int>(line->minimum)), std::abs(static_cast<int>(line->maximum))));
            const float amplitude = magnitude / 32767.0f;
            peakAmplitude = std::max(peakAmplitude, amplitude);
            amplitudeSum += amplitude;
            ++sampleCount;
            const auto lineWeight = std::max(1u, magnitude);
            rms += line->rms * lineWeight;
            bass += line->bass * lineWeight;
            mid += line->mid * lineWeight;
            treble += line->treble * lineWeight;
            weight += lineWeight;
        }
        if (weight == 0) continue;
        const float meanAmplitude = amplitudeSum / static_cast<float>(std::max(1, sampleCount));
        const float energy = waveform_visual::foldedEnergy(
            meanAmplitude, peakAmplitude);
        m_overviewHeights[x] = waveform_visual::logarithmicAmplitude(energy) * maxBarH;
        const auto visual = waveform_visual::map(
            waveform_visual::normalizeStyle(m_renderStyle), {
                static_cast<float>(bass / weight) / 255.0f,
                static_cast<float>(mid / weight) / 255.0f,
                static_cast<float>(treble / weight) / 255.0f,
                static_cast<float>(rms / weight) / 255.0f});
        m_overviewVisuals[x] = visual;
    }

    const float baseline = static_cast<float>(h - 1);
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setBrush(Qt::NoBrush);
    for (int x = 0; x < w; ++x) {
        const float barH = m_overviewHeights[x];
        if (barH <= 0.5f) continue;
        paintVisualColumn(painter, x, baseline - barH, baseline,
                          m_overviewVisuals[x], 0.92f);
    }
}

void OverviewWaveformItem::paint(QPainter* painter)
{
    const int w = std::max(1, static_cast<int>(width()));
    const int h = std::max(1, static_cast<int>(height()));

    if (!m_engine || !m_engine->hasTrack() || !m_engine->getTrackData()) {
        if (!m_frameCache.isNull() && m_frameCache.size() == QSize(w, h))
            painter->drawImage(0, 0, m_frameCache);
        return;
    }

    auto* td = m_engine->getTrackData();
    const auto overviewSnapshot = td->getOverviewRgbSnapshot();
    const bool hasOverview = overviewSnapshot && !overviewSnapshot->isEmpty();
    const auto lineSnapshot = td->getWaveformLineStoreSnapshot();

    // The quick full-track overview is intentionally authoritative while
    // analysis is running.  Switching to a sparse line-store snapshot halfway
    // through analysis used to make the overview jump into a flat block.
    //
    // The rectified (single-sided "skyline") path below used to be gated by
    // h <= 56, on the assumption that deck overview strips are always that
    // short. DeckTrackInfoPanel.qml actually sizes them to 60 * scale, and
    // scale reaches 1.0 on any panel wider than 720px — so on ordinary
    // window sizes this strip silently fell through to the tall, multi-pass
    // glow/core/tip-highlight renderer further down, which is exactly the
    // soft gradient/fade look at the peaks that shouldn't be there. Every
    // rectified overview now uses this same flat, line-based path
    // regardless of height; only the (currently unused) non-rectified,
    // mirrored overview still falls through to the renderer below.
    if (m_rectified && !hasOverview
        && lineSnapshot && lineSnapshot->totalLineCount > 0) {
        if (m_frameCache.size() != QSize(w, h))
            m_frameCache = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        m_frameCache.fill(Qt::transparent);
        QPainter cachePainter(&m_frameCache);
        paintCompactOverviewLines(&cachePainter, *lineSnapshot, w, h);
        painter->drawImage(0, 0, m_frameCache);
        return;
    }

    int ovrProcessed = 0;
    const QVector<TrackData::RgbWaveformFrame> progressiveOvr =
        hasOverview ? QVector<TrackData::RgbWaveformFrame>()
                    : td->getProgressiveOvrData(&ovrProcessed);

    const QVector<TrackData::RgbWaveformFrame>& frames = hasOverview
        ? *overviewSnapshot : progressiveOvr;
    if (frames.isEmpty()) {
        if (!m_frameCache.isNull() && m_frameCache.size() == QSize(w, h))
            painter->drawImage(0, 0, m_frameCache);
        return;
    }

    const int totalExpected  = hasOverview
        ? frames.size()
        : TrackData::kProgressiveBins;
    const int analyzedFrames = hasOverview
        ? frames.size()
        : std::clamp(
            static_cast<int>((static_cast<int64_t>(ovrProcessed) * TrackData::kProgressiveBins)
                             / std::max(1, td->getTotalExpected())),
            0, TrackData::kProgressiveBins);

    // Deck overview: compact rectified path — fast, stable, no multi-pass glow.
    if (m_rectified) {
        const int drawWidth = hasOverview
            ? w
            : std::clamp(
                static_cast<int>(std::llround(
                    (static_cast<double>(analyzedFrames) / static_cast<double>(totalExpected))
                    * static_cast<double>(w))),
                0, w);

        if (m_frameCache.size() != QSize(w, h))
            m_frameCache = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        m_frameCache.fill(Qt::transparent);

        QPainter cachePainter(&m_frameCache);
        paintCompactOverview(&cachePainter, frames, std::max(1, drawWidth), w, h);
        painter->drawImage(0, 0, m_frameCache);
        return;
    }

    painter->fillRect(boundingRect(), Qt::transparent);
    const float baseline = m_rectified ? static_cast<float>(h - 1) : static_cast<float>(h) * 0.5f;
    const float maxBarH = m_rectified ? static_cast<float>(h - 1) : static_cast<float>(h) * 0.5f;

    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setPen(Qt::NoPen);

    const int drawWidth = std::clamp(
        static_cast<int>(std::llround((static_cast<double>(analyzedFrames) / static_cast<double>(totalExpected)) * static_cast<double>(w))),
        0,
        w);

    std::vector<OverviewBin> bins(static_cast<size_t>(drawWidth));

    for (int x = 0; x < drawWidth; ++x) {
        const int i0 = static_cast<int>((static_cast<int64_t>(x) * analyzedFrames) / std::max(1, drawWidth));
        int i1 = static_cast<int>((static_cast<int64_t>(x + 1) * analyzedFrames) / std::max(1, drawWidth));
        i1 = std::max(i0 + 1, std::min(i1, analyzedFrames));

        OverviewBin bin;

        for (int i = i0; i < i1; ++i) {
            const auto& f = frames[i];
            bin.rms    = std::max(bin.rms,    f.rms);
            bin.rmsSum += f.rms;
            ++bin.count;
            bin.bass = std::max(bin.bass, f.bass);
            bin.mid    = std::max(bin.mid,    f.mid);
            bin.treble = std::max(bin.treble, f.treble);
        }

        bins[static_cast<size_t>(x)] = bin;
    }


    // ── Height computation: log1p compression + transient emphasis ──────────────
    // log1p(rms × k) / log1p(k) expands the lower dynamic range so breakdowns and
    // builds show as shorter-but-visible bars rather than a flat near-zero baseline.
    // Transient bins (peak >> mean) receive a small extra boost so kicks / snares
    // remain readable as slightly taller bars against their section background.
    std::vector<float> rawH(static_cast<size_t>(drawWidth));
    for (int x = 0; x < drawWidth; ++x) {
        const auto& b = bins[static_cast<size_t>(x)];
        if (b.rms <= 0.001f) { rawH[static_cast<size_t>(x)] = 0.0f; continue; }
        const float logH    = std::log1p(b.rms * 9.0f) / std::log1p(9.0f);
        const float rmsMean = b.count > 0 ? b.rmsSum / static_cast<float>(b.count) : b.rms;
        const float tBoost  = std::clamp((b.rms - rmsMean) / (rmsMean + 0.02f), 0.0f, 0.25f);
        rawH[static_cast<size_t>(x)] = std::clamp(logH + tBoost * 0.08f, 0.0f, 1.0f);
    }

    // ── Gaussian-approximate temporal smoothing ──────────────────────────────────
    // Two passes of a symmetric 3-tap kernel [0.25, 0.50, 0.25] → σ ≈ √2 pixels.
    // Removes per-bin noise so drops, builds, and breakdowns read as smooth regions.
    // Peak preservation: restore 82% of the pre-smooth height so transients remain
    // slightly taller than their smoothed neighbors.
    std::vector<float> smoothH = rawH;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> tmp = smoothH;
        for (int x = 1; x < drawWidth - 1; ++x) {
            tmp[static_cast<size_t>(x)] =
                smoothH[static_cast<size_t>(x - 1)] * 0.25f +
                smoothH[static_cast<size_t>(x)]     * 0.50f +
                smoothH[static_cast<size_t>(x + 1)] * 0.25f;
        }
        smoothH = std::move(tmp);
    }
    for (int x = 0; x < drawWidth; ++x) {
        smoothH[static_cast<size_t>(x)] = std::max(
            smoothH[static_cast<size_t>(x)],
            rawH[static_cast<size_t>(x)] * 0.82f);
    }

    const auto style = waveform_visual::normalizeStyle(m_renderStyle);
    if (style == waveform_visual::WaveformRenderStyle::ThreeBand) {
        for (int x = 0; x < drawWidth; ++x) {
            const auto& bin = bins[static_cast<size_t>(x)];
            if (bin.rms <= 0.0001f)
                continue;
            const float bodyH = smoothH[static_cast<size_t>(x)] * maxBarH;
            const auto visual = waveform_visual::map(style,
                {bin.bass, bin.mid, bin.treble, bin.rms});
            // The same high/mid/bass lanes as scrolling tiles.  Geometry still
            // spans the full waveform body; only the independently meaningful
            // band components are partitioned inside that silhouette.
            paintVisualColumn(painter, x, baseline - bodyH,
                              baseline + bodyH, visual, 0.95f);
        }
    } else {
    // ── Render column computation ────────────────────────────────────────────────
    std::vector<RenderCol> cols(static_cast<size_t>(drawWidth));
    for (int x = 0; x < drawWidth; ++x) {
        const auto& bin = bins[static_cast<size_t>(x)];
        if (bin.rms <= 0.0001f) continue;
        const float rms   = std::clamp(bin.rms, 0.0f, 1.0f);
        const float bodyH = smoothH[static_cast<size_t>(x)] * maxBarH;
        const auto visual = waveform_visual::map(style,
            {bin.bass, bin.mid, bin.treble, rms});
        cols[static_cast<size_t>(x)] = {
            visualColor(visual.components[0].color),
            bodyH,
            bodyH * 0.18f,
            std::max(0.8f, bodyH * 0.22f),
            rms
        };
    }

    // ── Overview rendering: 3 passes ─────────────────────────────────────────────
    if (m_rectified) {
        const QColor rail(255, 255, 255, 24);
        painter->setBrush(rail);
        painter->drawRect(QRectF(0.0, baseline - 1.0, drawWidth, 1.0));
    }

    // Pass 1: Soft dynamic glow. Louder regions get more bloom so the overview
    // reads like a transparent energy map instead of a flat bar graph.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.bodyH <= 0.0f) continue;
        const auto& c = col.color;
        const int alpha = std::clamp(static_cast<int>(18 + col.energy * 34.0f), 18, 52);
        painter->setBrush(QColor(c.red(), c.green(), c.blue(), alpha));
        const double glowH = static_cast<double>(col.bodyH) * (1.20 + col.energy * 0.18);
        if (m_rectified) {
            painter->drawRect(QRectF(x - 1.0, baseline - glowH, 3.0, glowH + 1.0));
            if (col.bodyH > maxBarH * 0.34f)
                painter->drawRect(QRectF(x - 2.0, baseline - glowH * 0.82, 5.0, glowH * 0.82 + 1.0));
        } else {
            painter->drawRect(QRectF(x - 0.5, baseline - glowH, 2.0, 2.0 * glowH + 1.0));
        }
    }

    // Pass 2: Main body. Rectified overview is intentionally translucent so
    // overlays and dense sections keep depth instead of becoming solid blocks.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.bodyH <= 0.0f) continue;
        const auto& c = col.color;
        const int alpha = m_rectified
            ? std::clamp(static_cast<int>(145 + col.energy * 80.0f), 145, 225)
            : 235;
        painter->setBrush(QColor(c.red(), c.green(), c.blue(), alpha));
        if (m_rectified) {
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, col.bodyH + 1.0));
        } else {
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, 2.0 * col.bodyH + 1.0));
        }
    }

    // Pass 3: Inner energy detail.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.bodyH <= 1.0f) continue;
        const auto& c = col.color;
        const int cr = c.red()   + (255 - c.red())   * 52 / 100;
        const int cg = c.green() + (255 - c.green()) * 52 / 100;
        const int cb = c.blue()  + (255 - c.blue())  * 52 / 100;
        painter->setBrush(QColor(cr, cg, cb, m_rectified ? 180 : 245));
        if (m_rectified) {
            const double y = baseline - std::max(1.0f, col.bodyH * 0.70f);
            painter->drawRect(QRectF(x, y, 1.0, std::min<double>(col.coreH, baseline - y + 1.0)));
        } else {
            painter->drawRect(QRectF(x, baseline - col.coreH * 0.5f, 1.0, col.coreH));
        }
    }

    // Pass 4: Edge highlight — energy peaks glow at the waveform silhouette.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.tipH <= 0.5f) continue;
        const auto& c = col.color;
        const int tr = c.red()   + (255 - c.red())   * 65 / 100;
        const int tg = c.green() + (255 - c.green()) * 65 / 100;
        const int tb = c.blue()  + (255 - c.blue())  * 65 / 100;
        painter->setBrush(QColor(tr, tg, tb, m_rectified ? 205 : 255));
        if (m_rectified) {
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, std::max(0.7f, col.tipH * 0.72f)));
        } else {
            // Bright tip at both the top and bottom edges of the symmetric bar.
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, col.tipH + 0.5));
            painter->drawRect(QRectF(x, baseline + col.bodyH - col.tipH, 1.0, col.tipH + 0.5));
        }
    }

    }

    const float durationSec = std::max(0.001f, m_engine->getDuration());
    const QVariantList cues = m_engine->hotCues();
    for (const QVariant& v : cues) {
        const QVariantMap m = v.toMap();
        if (!m.value("set").toBool())
            continue;

        const int cueIndex = m.value("index").toInt();
        const double cueSec = m.value("positionSec").toDouble();
        const float progress = std::clamp(static_cast<float>(cueSec / durationSec), 0.0f, 1.0f);
        const float x = progress * static_cast<float>(w);

        QColor c(m.value("color").toString());
        if (!c.isValid())
            c = QColor("#e04040");
        c.setAlpha(230);

        // Draw a clean cue bar without shadow layers.
        painter->setPen(QPen(c, 2.2));
        painter->drawLine(QPointF(x, 0.0), QPointF(x, static_cast<float>(h)));

        // Top cue badge: same cue color + readable number inside.
        const float badgeW = 20.0f;
        const float badgeH = 14.0f;
        const float badgeX = std::clamp(x - badgeW * 0.5f, 0.0f, static_cast<float>(w) - badgeW);
        const QRectF badgeRect(badgeX, 0.0, badgeW, badgeH);

        QColor fill = c;
        fill.setAlpha(245);
        painter->setBrush(fill);
        painter->setPen(QPen(QColor(0, 0, 0, 185), 1.0));
        painter->drawRoundedRect(badgeRect, 2.5, 2.5);

        const int brightness = (fill.red() * 299 + fill.green() * 587 + fill.blue() * 114) / 1000;
        const QColor textColor = (brightness < 145) ? QColor("#f8f8f8") : QColor("#111111");
        painter->setPen(textColor);

        QFont f = painter->font();
        f.setBold(true);
        f.setPixelSize(9);
        painter->setFont(f);
        painter->drawText(badgeRect, Qt::AlignCenter, QString::number(cueIndex + 1));
    }
}
