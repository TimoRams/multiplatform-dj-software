#include "RgbWaveformItem.h"

#include <QPainter>
#include <algorithm>

namespace {

static inline QColor mixDjWaveColor(float low, float mid, float high, float rms)
{
    low = std::clamp(low, 0.0f, 1.0f);
    mid = std::clamp(mid, 0.0f, 1.0f);
    high = std::clamp(high, 0.0f, 1.0f);
    rms = std::clamp(rms, 0.0f, 1.0f);

    float r = std::pow(low, 0.52f) * 1.18f + std::pow(mid, 0.85f) * 0.36f;
    float g = std::pow(mid, 0.50f) * 1.20f + std::pow(high, 0.95f) * 0.10f + std::pow(low, 1.20f) * 0.06f;
    float b = std::pow(high, 0.50f) * 1.22f + std::pow(mid, 1.00f) * 0.08f;

    const float whiteLift = std::pow(std::max({low, mid, high}), 0.70f) * (0.06f + 0.16f * rms);
    r += whiteLift;
    g += whiteLift;
    b += whiteLift;

    QColor c = QColor::fromRgbF(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f),
        1.0f);

    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    c.getHsvF(&h, &s, &v);
    // Stronger vibrance than the scrolling waveform — the overview's QPainter path
    // doesn't benefit from scene-graph linear-light blending, so we compensate here.
    s = std::clamp(static_cast<float>(s * 1.80f + 0.15f), 0.0f, 1.0f);
    v = std::clamp(static_cast<float>(v * 1.35f + 0.08f + 0.12f * rms), 0.0f, 1.0f);
    c.setHsvF(h, s, v, 1.0);
    return c;
}

struct OverviewBin {
    float rms = 0.0f;
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
};

struct RenderCol {
    QColor base;
    float glowH  = 0.0f;
    float bodyH  = 0.0f;
    float coreH  = 0.0f;
};

} // namespace

RgbWaveformItem::RgbWaveformItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setOpaquePainting(false);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
}

void RgbWaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine)
        return;

    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);

    m_engine = engine;

    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded, this, &RgbWaveformItem::onTrackLoaded, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::progressChanged, this, &RgbWaveformItem::onRgbDataChanged, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged, this, &RgbWaveformItem::onRgbDataChanged, Qt::UniqueConnection);
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
    if (!m_engine || !m_engine->getTrackData()) {
        update();
        return;
    }

    auto* td = m_engine->getTrackData();
    connect(td, &TrackData::rgbWaveformUpdated, this, &RgbWaveformItem::onRgbDataChanged, Qt::UniqueConnection);
    connect(td, &TrackData::dataCleared, this, &RgbWaveformItem::onRgbDataChanged, Qt::UniqueConnection);
    update();
}

void RgbWaveformItem::onRgbDataChanged()
{
    update();
}

void RgbWaveformItem::paint(QPainter* painter)
{
    painter->fillRect(boundingRect(), Qt::transparent);

    if (!m_engine || !m_engine->getTrackData())
        return;

    const QVector<TrackData::RgbWaveformFrame> frames = m_engine->getTrackData()->getRgbWaveformData();
    if (frames.isEmpty())
        return;

    const int totalExpected = std::max(1, m_engine->getTrackData()->getTotalExpected());
    const int analyzedFrames = std::min(static_cast<int>(frames.size()), totalExpected);

    const int w = std::max(1, static_cast<int>(width()));
    const int h = std::max(1, static_cast<int>(height()));
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
            bin.rms = std::max(bin.rms, f.rms);
            bin.low = std::max(bin.low, f.low);
            bin.mid = std::max(bin.mid, f.mid);
            bin.high = std::max(bin.high, f.high);
        }

        bins[static_cast<size_t>(x)] = bin;
    }


    // Pre-compute colors and amplitudes — mirrors ScrollingWaveformItem's 3-layer model.
    std::vector<RenderCol> cols(static_cast<size_t>(drawWidth));
    for (int x = 0; x < drawWidth; ++x) {
        const auto& bin = bins[static_cast<size_t>(x)];
        if (bin.rms <= 0.0001f) continue;
        const float bodyH = std::clamp(bin.rms, 0.0f, 1.0f) * maxBarH;
        cols[static_cast<size_t>(x)] = {
            mixDjWaveColor(bin.low, bin.mid, bin.high, bin.rms),
            std::min(maxBarH, bodyH * 1.34f + 0.7f),
            bodyH,
            bodyH * 0.56f
        };
    }

    // Pass 1: Outer glow (2px wide, low alpha — same soft-halo as scrolling waveform).
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.bodyH <= 0.0f) continue;
        const auto& c = col.base;
        painter->setBrush(QColor(c.red(), c.green(), c.blue(), 84));
        if (m_rectified)
            painter->drawRect(QRectF(x - 0.5, baseline - col.glowH,  2.0, col.glowH + 1.0));
        else
            painter->drawRect(QRectF(x - 0.5, baseline - col.glowH,  2.0, 2.0 * col.glowH + 1.0));
    }

    // Pass 2: Main body.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.bodyH <= 0.0f) continue;
        const auto& c = col.base;
        painter->setBrush(QColor(c.red(), c.green(), c.blue(), 255));
        if (m_rectified)
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, col.bodyH + 1.0));
        else
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, 2.0 * col.bodyH + 1.0));
    }

    // Pass 3: Bright core (inner portion, slightly lighter — same as scrolling waveform core).
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.coreH <= 0.5f) continue;
        const auto& c = col.base;
        const int cR = std::min(255, static_cast<int>(c.red()   * 1.10f + 10.0f));
        const int cG = std::min(255, static_cast<int>(c.green() * 1.10f + 10.0f));
        const int cB = std::min(255, static_cast<int>(c.blue()  * 1.10f + 10.0f));
        painter->setBrush(QColor(cR, cG, cB, 248));
        if (m_rectified)
            painter->drawRect(QRectF(x, baseline - col.coreH, 1.0, col.coreH + 1.0));
        else
            painter->drawRect(QRectF(x, baseline - col.coreH, 1.0, 2.0 * col.coreH + 1.0));
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
