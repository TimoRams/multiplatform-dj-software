#pragma once

#include <QQuickImageProvider>
#include <QImage>
#include <QHash>
#include <QMutex>
#include <QByteArray>
#include <QBuffer>

// QML image provider for cover art.
// Access in QML: Image { source: "image://coverart/deckA" }
// C++ side calls setCover(id, bytes) after loading a track; the URL must include
// a timestamp query parameter to bust QML's image:// cache (see DjEngine::loadTrack).
class CoverArtProvider : public QQuickImageProvider
{
public:
    CoverArtProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requestedSize) override
    {
        QMutexLocker lock(&m_mutex);

        // The id may be "deckA?t=12345" due to the cache-busting timestamp trick.
        // Strip everything from '?' onward so the hash lookup finds the right key.
        const QString cleanId = id.section(QLatin1Char('?'), 0, 0);

        QImage img;
        if (m_covers.contains(cleanId))
            img = m_covers.value(cleanId);

        if (img.isNull()) {
            img = QImage(1, 1, QImage::Format_ARGB32);
            img.fill(Qt::transparent);
        }

        if (requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0)
            img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        if (size)
            *size = img.size();

        return img;
    }

    void setCover(const QString& id, const QByteArray& data)
    {
        QMutexLocker lock(&m_mutex);
        QImage img;
        if (!data.isEmpty())
            img.loadFromData(data);
        m_covers[id] = img;
    }

    void clearCover(const QString& id)
    {
        QMutexLocker lock(&m_mutex);
        m_covers.remove(id);
    }

    bool hasCover(const QString& id)
    {
        QMutexLocker lock(&m_mutex);
        return m_covers.contains(id) && !m_covers.value(id).isNull()
               && m_covers.value(id).width() > 1;
    }

private:
    QHash<QString, QImage> m_covers;
    QMutex m_mutex;
};
