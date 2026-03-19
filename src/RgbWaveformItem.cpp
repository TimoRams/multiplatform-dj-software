#include "RgbWaveformItem.h"

#include <QPainter>
#include <algorithm>

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

    painter->setPen(Qt::NoPen);

    const int drawWidth = std::clamp(
        static_cast<int>(std::llround((static_cast<double>(analyzedFrames) / static_cast<double>(totalExpected)) * static_cast<double>(w))),
        0,
        w);

    for (int x = 0; x < drawWidth; ++x) {
        const int i0 = static_cast<int>((static_cast<int64_t>(x) * analyzedFrames) / std::max(1, drawWidth));
        int i1 = static_cast<int>((static_cast<int64_t>(x + 1) * analyzedFrames) / std::max(1, drawWidth));
        i1 = std::max(i0 + 1, std::min(i1, analyzedFrames));

        float maxRms = 0.0f;
        float wr = 0.0f;
        float wg = 0.0f;
        float wb = 0.0f;
        float wsum = 0.0f;

        for (int i = i0; i < i1; ++i) {
            const auto& f = frames[i];
            maxRms = std::max(maxRms, f.rms);
            const float weight = std::max(0.05f, f.rms);
            wr += static_cast<float>(f.color.red()) * weight;
            wg += static_cast<float>(f.color.green()) * weight;
            wb += static_cast<float>(f.color.blue()) * weight;
            wsum += weight;
        }

        if (wsum <= 0.0f)
            continue;

        const int r = std::clamp(static_cast<int>((wr / wsum) * 1.10f + 8.0f), 0, 255);
        const int g = std::clamp(static_cast<int>((wg / wsum) * 1.10f + 8.0f), 0, 255);
        const int b = std::clamp(static_cast<int>((wb / wsum) * 1.10f + 8.0f), 0, 255);
        const QColor c(r, g, b, 230);

        const float barH = std::clamp(maxRms, 0.0f, 1.0f) * maxBarH;
        painter->setBrush(c);

        if (m_rectified) {
            painter->drawRect(QRectF(static_cast<qreal>(x),
                                     static_cast<qreal>(baseline - barH),
                                     1.0,
                                     static_cast<qreal>(barH + 1.0f)));
        } else {
            painter->drawRect(QRectF(static_cast<qreal>(x),
                                     static_cast<qreal>(baseline - barH),
                                     1.0,
                                     static_cast<qreal>(2.0f * barH + 1.0f)));
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

        painter->setPen(QPen(c, 1.0));
        painter->drawLine(QPointF(x, 0.0), QPointF(x, static_cast<float>(h)));

        // Top cue badge: same cue color + readable number inside.
        const float badgeW = 16.0f;
        const float badgeH = 11.0f;
        const float badgeX = std::clamp(x - badgeW * 0.5f, 0.0f, static_cast<float>(w) - badgeW);
        const QRectF badgeRect(badgeX, 0.0, badgeW, badgeH);

        QColor fill = c;
        fill.setAlpha(245);
        painter->setBrush(fill);
        painter->setPen(QPen(fill.darker(130), 1.0));
        painter->drawRoundedRect(badgeRect, 2.0, 2.0);

        const int brightness = (fill.red() * 299 + fill.green() * 587 + fill.blue() * 114) / 1000;
        const QColor textColor = (brightness < 145) ? QColor("#f8f8f8") : QColor("#111111");
        painter->setPen(textColor);

        QFont f = painter->font();
        f.setBold(true);
        f.setPixelSize(8);
        painter->setFont(f);
        painter->drawText(badgeRect, Qt::AlignCenter, QString::number(cueIndex + 1));
    }
}
