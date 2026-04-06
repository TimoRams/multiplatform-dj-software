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

struct OverviewBin {
    float rms = 0.0f;
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
};

} // namespace

RgbWaveformItem::RgbWaveformItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    setOpaquePainting(false);
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

    // Gentle horizontal smoothing: keeps detail but removes jagged pixel noise.
    if (drawWidth >= 5) {
        std::vector<OverviewBin> smoothed = bins;
        for (int x = 2; x < drawWidth - 2; ++x) {
            const auto b0 = bins[static_cast<size_t>(x - 2)];
            const auto b1 = bins[static_cast<size_t>(x - 1)];
            const auto b2 = bins[static_cast<size_t>(x)];
            const auto b3 = bins[static_cast<size_t>(x + 1)];
            const auto b4 = bins[static_cast<size_t>(x + 2)];

            smoothed[static_cast<size_t>(x)].rms = b0.rms * 0.08f + b1.rms * 0.24f + b2.rms * 0.36f + b3.rms * 0.24f + b4.rms * 0.08f;
            smoothed[static_cast<size_t>(x)].low = b0.low * 0.08f + b1.low * 0.24f + b2.low * 0.36f + b3.low * 0.24f + b4.low * 0.08f;
            smoothed[static_cast<size_t>(x)].mid = b0.mid * 0.08f + b1.mid * 0.24f + b2.mid * 0.36f + b3.mid * 0.24f + b4.mid * 0.08f;
            smoothed[static_cast<size_t>(x)].high = b0.high * 0.08f + b1.high * 0.24f + b2.high * 0.36f + b3.high * 0.24f + b4.high * 0.08f;
        }
        bins.swap(smoothed);
    }

    for (int x = 0; x < drawWidth; ++x) {
        const auto& bin = bins[static_cast<size_t>(x)];
        if (bin.rms <= 0.0001f)
            continue;

        const QColor base = mixDjWaveColor(bin.low, bin.mid, bin.high, bin.rms);
        const QColor body(base.red(), base.green(), base.blue(), 236);
        const float bodyH = std::clamp(bin.rms, 0.0f, 1.0f) * maxBarH;

        if (m_rectified) {
            painter->setBrush(body);
            painter->drawRect(QRectF(static_cast<qreal>(x),
                                     static_cast<qreal>(baseline - bodyH),
                                     1.0,
                                     static_cast<qreal>(bodyH + 1.0f)));
        } else {
            painter->setBrush(body);
            painter->drawRect(QRectF(static_cast<qreal>(x),
                                     static_cast<qreal>(baseline - bodyH),
                                     1.0,
                                     static_cast<qreal>(2.0f * bodyH + 1.0f)));
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
