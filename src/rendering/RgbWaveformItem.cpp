#include "RgbWaveformItem.h"

#include <QPainter>
#include <algorithm>

namespace {

static inline QColor mixRekordboxColor(float low, float lowMid, float mid, float high, float rms)
{
    // Rekordbox-style RGB palette — same mapping as ScrollingWaveformItem.
    constexpr float lR = 255.0f, lG = 20.0f,  lB = 20.0f;   // vivid red
    constexpr float mR = 255.0f, mG = 130.0f, mB = 0.0f;    // orange
    constexpr float hR = 210.0f, hG = 255.0f, hB = 0.0f;    // yellow-lime
    constexpr float xR = 0.0f,   xG = 185.0f, xB = 255.0f;  // electric cyan

    // Same higher exponents as ScrollingWaveformItem — dominant band wins clearly.
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
    s2 = std::clamp(static_cast<float>(s2 * 1.40f + 0.06f), 0.0f, 1.0f);
    v2 = std::clamp(static_cast<float>(v2 * 1.15f + 0.04f), 0.0f, 1.0f);
    tmp.setHsvF(h2, s2, v2, 1.0);
    return tmp;
}

struct OverviewBin {
    float rms    = 0.0f;
    float low    = 0.0f;
    float lowMid = 0.0f;
    float mid    = 0.0f;
    float high   = 0.0f;
};

struct RenderCol {
    QColor color;
    float bodyH = 0.0f;
    float coreH = 0.0f;
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
    m_updateThrottle->setInterval(200);  // ≤5 fps during progressive analysis
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
    if (!m_engine || !m_engine->getTrackData()) {
        update();
        return;
    }

    auto* td = m_engine->getTrackData();
    connect(td, &TrackData::rgbWaveformUpdated, this, &RgbWaveformItem::onRgbDataChanged,  Qt::UniqueConnection);
    connect(td, &TrackData::dataCleared,        this, &RgbWaveformItem::onRgbDataChanged,  Qt::UniqueConnection);
    // Overview arrives once after cache load — repaint immediately when ready.
    connect(td, &TrackData::overviewRgbUpdated, this, [this]() { update(); }, Qt::UniqueConnection);
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

void RgbWaveformItem::onHotCuesChanged()
{
    // Cue pins must jump immediately — bypass the throttle.
    update();
}

void RgbWaveformItem::paint(QPainter* painter)
{
    painter->fillRect(boundingRect(), Qt::transparent);

    if (!m_engine || !m_engine->getTrackData())
        return;

    auto* td = m_engine->getTrackData();

    // Use the pre-downsampled overview (≤4096 bins) when available — this keeps
    // paint() O(kOverviewBins) instead of O(total_frames) for long mixes.
    // Fall back to full data only during progressive analysis before the overview
    // has been computed (first-time analysis, no waveform cache yet).
    const QVector<TrackData::RgbWaveformFrame> overview = td->getOverviewRgbData();
    const bool hasOverview = !overview.isEmpty();
    const QVector<TrackData::RgbWaveformFrame> frames = hasOverview
        ? overview
        : td->getRgbWaveformData();

    if (frames.isEmpty())
        return;

    // When the overview is ready the track is fully cached — draw the full width.
    // During analysis, draw only the fraction that has been processed so far.
    const int totalExpected   = hasOverview ? frames.size()
                                            : std::max(1, td->getTotalExpected());
    const int analyzedFrames  = hasOverview ? frames.size()
                                            : std::min(static_cast<int>(frames.size()), totalExpected);

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
            bin.rms    = std::max(bin.rms,    f.rms);
            bin.low    = std::max(bin.low,    f.low);
            bin.lowMid = std::max(bin.lowMid, f.lowMid);
            bin.mid    = std::max(bin.mid,    f.mid);
            bin.high   = std::max(bin.high,   f.high);
        }

        bins[static_cast<size_t>(x)] = bin;
    }


    // Rekordbox-style 2-pass overview rendering.
    // Pass 1: main body bar — vivid, fully opaque.
    // Pass 2: narrow central spine mixed 50% toward white — "lit from inside" depth.
    // No outer glow (avoids smear), no fixed-color high strip (highs tint body naturally).
    std::vector<RenderCol> cols(static_cast<size_t>(drawWidth));
    for (int x = 0; x < drawWidth; ++x) {
        const auto& bin = bins[static_cast<size_t>(x)];
        if (bin.rms <= 0.0001f) continue;
        const float rms   = std::clamp(bin.rms, 0.0f, 1.0f);
        const float bodyH = rms * maxBarH;
        cols[static_cast<size_t>(x)] = {
            mixRekordboxColor(bin.low, bin.lowMid, bin.mid, bin.high, rms),
            bodyH,
            bodyH * 0.32f
        };
    }

    // Pass 1: vivid body bars.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.bodyH <= 0.0f) continue;
        const auto& c = col.color;
        painter->setBrush(QColor(c.red(), c.green(), c.blue(), 242));
        if (m_rectified)
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, col.bodyH + 1.0));
        else
            painter->drawRect(QRectF(x, baseline - col.bodyH, 1.0, 2.0 * col.bodyH + 1.0));
    }

    // Pass 2: bright central spine — 50% mix toward white for inner depth.
    for (int x = 0; x < drawWidth; ++x) {
        const auto& col = cols[static_cast<size_t>(x)];
        if (col.coreH <= 0.0f) continue;
        const auto& c = col.color;
        const int sr = c.red()   + (255 - c.red())   / 2;
        const int sg = c.green() + (255 - c.green()) / 2;
        const int sb = c.blue()  + (255 - c.blue())  / 2;
        painter->setBrush(QColor(sr, sg, sb, 255));
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
