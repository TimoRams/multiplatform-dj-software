#include "RgbWaveformItem.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

static inline QColor mixBandColor(float low, float lowMid, float mid, float high, float rms)
{
    constexpr float lR = 255.0f, lG = 20.0f,  lB = 20.0f;   // vivid red
    constexpr float mR = 255.0f, mG = 130.0f, mB = 0.0f;    // orange
    constexpr float hR = 210.0f, hG = 255.0f, hB = 0.0f;    // yellow-lime
    constexpr float xR = 0.0f,   xG = 185.0f, xB = 255.0f;  // electric cyan

    // Higher exponents → dominant band wins clearly.
    const float wL  = std::pow(low,    2.8f);
    const float wLM = std::pow(lowMid, 2.5f);
    const float wM  = std::pow(mid,    2.2f);
    const float wH  = std::pow(high,   1.6f);
    const float wSum = wL + wLM + wM + wH + 1e-7f;

    float r = (wL * lR + wLM * mR + wM * hR + wH * xR) / wSum;
    float g = (wL * lG + wLM * mG + wM * hG + wH * xG) / wSum;
    float b = (wL * lB + wLM * mB + wM * hB + wH * xB) / wSum;

    // QPainter lacks scene-graph linear blending — apply brightness + saturation boost.
    const float bright = std::pow(rms, 0.35f);
    r *= bright;
    g *= bright;
    b *= bright;

    float h2 = 0.0f, s2 = 0.0f, v2 = 0.0f;
    QColor tmp(std::clamp(int(r), 0, 255), std::clamp(int(g), 0, 255), std::clamp(int(b), 0, 255));
    tmp.getHsvF(&h2, &s2, &v2);
    // Boost saturation + brightness for vivid, immediately readable overview colors.
    s2 = std::clamp(static_cast<float>(s2 * 1.70f + 0.10f), 0.0f, 1.0f);
    v2 = std::clamp(static_cast<float>(v2 * 1.22f + 0.05f), 0.0f, 1.0f);
    tmp.setHsvF(h2, s2, v2, 1.0);
    return tmp;
}

struct OverviewBin {
    float rms    = 0.0f;   // max rms in bin (peak energy)
    float rmsSum = 0.0f;   // sum for mean — used for transient detection
    float low    = 0.0f;
    float lowMid = 0.0f;
    float mid    = 0.0f;
    float high   = 0.0f;
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

RgbWaveformItem::RgbWaveformItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setOpaquePainting(false);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);

    m_updateThrottle = new QTimer(this);
    m_updateThrottle->setSingleShot(true);
    m_updateThrottle->setInterval(100);  // fallback path only — overview preview is instant
    connect(m_updateThrottle, &QTimer::timeout, this, [this]() { update(); });
}

void RgbWaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine)
        return;

    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);

    m_engine = engine;

    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded,    this, &RgbWaveformItem::onTrackLoaded,    Qt::UniqueConnection);
        connect(m_engine, &DjEngine::progressChanged, this, &RgbWaveformItem::onRgbDataChanged, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged,  this, &RgbWaveformItem::onHotCuesChanged,  Qt::UniqueConnection);
    }

    emit engineChanged();
    update();
}

void RgbWaveformItem::setRectified(bool v)
{
    if (m_rectified == v)
        return;

    m_rectified = v;
    emit rectifiedChanged();
    update();
}

void RgbWaveformItem::onTrackLoaded()
{
    m_frameCache = QImage();

    if (!m_engine || !m_engine->getTrackData()) {
        update();
        return;
    }

    auto* td = m_engine->getTrackData();
    connect(td, &TrackData::rgbWaveformUpdated, this, &RgbWaveformItem::onRgbDataChanged,  Qt::UniqueConnection);
    connect(td, &TrackData::dataCleared,        this, &RgbWaveformItem::onRgbDataChanged,  Qt::UniqueConnection);
    // Overview arrives once after cache load — repaint immediately when ready.
    connect(td, &TrackData::overviewRgbUpdated, this, &RgbWaveformItem::onOverviewRgbUpdated, Qt::UniqueConnection);
    update();
}

void RgbWaveformItem::onRgbDataChanged()
{
    // During progressive analysis, rgbWaveformUpdated fires with every new chunk
    // and progressChanged fires at ~30 fps. Throttle so paint() (which still needs
    // to bin the growing full-res data) doesn't hammer the render thread.
    // Once overviewRgbUpdated fires, paint() switches to the pre-downsampled path
    // and subsequent calls are O(kOverviewBins), so the throttle becomes a no-op cost.
    if (!m_updateThrottle->isActive())
        m_updateThrottle->start();
}

void RgbWaveformItem::onOverviewRgbUpdated()
{
    update();
}

void RgbWaveformItem::onHotCuesChanged()
{
    // Cue pins must jump immediately — bypass the throttle.
    update();
}

void RgbWaveformItem::paintCompactOverview(QPainter* painter,
                                           const QVector<TrackData::RgbWaveformFrame>& frames,
                                           int drawWidth, int w, int h)
{
    const float baseline = static_cast<float>(h - 1);
    const float maxBarH  = static_cast<float>(h - 2);

    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(Qt::NoPen);

    std::vector<float> heights(static_cast<size_t>(drawWidth), 0.0f);
    std::vector<QColor> colors(static_cast<size_t>(drawWidth));

    for (int x = 0; x < drawWidth; ++x) {
        const int i0 = static_cast<int>((static_cast<int64_t>(x) * frames.size()) / std::max(1, drawWidth));
        int i1 = static_cast<int>((static_cast<int64_t>(x + 1) * frames.size()) / std::max(1, drawWidth));
        i1 = std::max(i0 + 1, std::min(i1, static_cast<int>(frames.size())));

        float rms = 0.0f, low = 0.0f, lowMid = 0.0f, mid = 0.0f, high = 0.0f;
        for (int i = i0; i < i1; ++i) {
            const auto& f = frames[i];
            rms    = std::max(rms,    f.rms);
            low    = std::max(low,    f.low);
            lowMid = std::max(lowMid, f.lowMid);
            mid    = std::max(mid,    f.mid);
            high   = std::max(high,   f.high);
        }

        if (rms <= 0.001f)
            continue;

        const float logH = std::log1p(rms * 8.0f) / std::log1p(8.0f);
        heights[static_cast<size_t>(x)] = std::clamp(logH, 0.04f, 1.0f) * maxBarH;
        colors[static_cast<size_t>(x)]  = mixBandColor(low, lowMid, mid, high, rms);
    }

    // Single-pass light smoothing for a clean DJ-software silhouette.
    for (int x = 1; x < drawWidth - 1; ++x) {
        heights[static_cast<size_t>(x)] =
            heights[static_cast<size_t>(x - 1)] * 0.20f +
            heights[static_cast<size_t>(x)]     * 0.60f +
            heights[static_cast<size_t>(x + 1)] * 0.20f;
    }

    // Body fill — saturated, bottom-aligned bars.
    for (int x = 0; x < drawWidth; ++x) {
        const float barH = heights[static_cast<size_t>(x)];
        if (barH <= 0.5f)
            continue;
        const QColor c = colors[static_cast<size_t>(x)];
        painter->setBrush(QColor(c.red(), c.green(), c.blue(), 210));
        painter->drawRect(QRectF(static_cast<double>(x), baseline - barH, 1.0, barH + 1.0));
    }

    // Peak edge — thin bright cap for transients.
    for (int x = 0; x < drawWidth; ++x) {
        const float barH = heights[static_cast<size_t>(x)];
        if (barH <= 1.5f)
            continue;
        const QColor c = colors[static_cast<size_t>(x)];
        painter->setBrush(QColor(
            std::min(255, c.red()   + 70),
            std::min(255, c.green() + 70),
            std::min(255, c.blue()  + 70), 230));
        painter->drawRect(QRectF(static_cast<double>(x), baseline - barH, 1.0, 1.5));
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

void RgbWaveformItem::paint(QPainter* painter)
{
    const int w = std::max(1, static_cast<int>(width()));
    const int h = std::max(1, static_cast<int>(height()));

    if (!m_engine || !m_engine->getTrackData()) {
        if (!m_frameCache.isNull() && m_frameCache.size() == QSize(w, h))
            painter->drawImage(0, 0, m_frameCache);
        return;
    }

    auto* td = m_engine->getTrackData();

    const QVector<TrackData::RgbWaveformFrame> overview = td->getOverviewRgbData();
    const bool hasOverview = !overview.isEmpty();

    int ovrProcessed = 0;
    const QVector<TrackData::RgbWaveformFrame> progressiveOvr =
        hasOverview ? QVector<TrackData::RgbWaveformFrame>()
                    : td->getProgressiveOvrData(&ovrProcessed);

    const QVector<TrackData::RgbWaveformFrame>& frames = hasOverview ? overview : progressiveOvr;
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
    if (m_rectified && h <= 56) {
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
            bin.low    = std::max(bin.low,    f.low);
            bin.lowMid = std::max(bin.lowMid, f.lowMid);
            bin.mid    = std::max(bin.mid,    f.mid);
            bin.high   = std::max(bin.high,   f.high);
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

    // ── Render column computation ────────────────────────────────────────────────
    std::vector<RenderCol> cols(static_cast<size_t>(drawWidth));
    for (int x = 0; x < drawWidth; ++x) {
        const auto& bin = bins[static_cast<size_t>(x)];
        if (bin.rms <= 0.0001f) continue;
        const float rms   = std::clamp(bin.rms, 0.0f, 1.0f);
        const float bodyH = smoothH[static_cast<size_t>(x)] * maxBarH;
        cols[static_cast<size_t>(x)] = {
            mixBandColor(bin.low, bin.lowMid, bin.mid, bin.high, rms),
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
