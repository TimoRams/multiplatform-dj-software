#include "LibraryCoverService.h"
#include "library/MediaIoScheduler.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutexLocker>

namespace {

QString pathKey(const QString& path)
{
    if (path.isEmpty())
        return {};
    return QStringLiteral("path_") + QFileInfo(path).absoluteFilePath();
}

} // namespace

CoverArtProvider::CoverArtProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage CoverArtProvider::requestImage(const QString& id, QSize* size,
                                      const QSize& requestedSize)
{
    QMutexLocker lock(&m_mutex);

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

void CoverArtProvider::setCover(const QString& id, const QByteArray& data)
{
    QImage img;
    if (!data.isEmpty())
        img.loadFromData(data);
    setCoverImage(id, img);
}

void CoverArtProvider::setCoverImage(const QString& id, const QImage& image)
{
    QMutexLocker lock(&m_mutex);
    m_covers[id] = image;
}

void CoverArtProvider::clearCover(const QString& id)
{
    QMutexLocker lock(&m_mutex);
    m_covers.remove(id);
}

bool CoverArtProvider::hasCover(const QString& id)
{
    QMutexLocker lock(&m_mutex);
    return m_covers.contains(id) && !m_covers.value(id).isNull()
           && m_covers.value(id).width() > 1;
}

QImage CoverArtProvider::coverImage(const QString& id)
{
    QMutexLocker lock(&m_mutex);
    return m_covers.value(id);
}

namespace {
constexpr std::uint32_t kCoverServiceOwner = 2;
}

LibraryCoverService::LibraryCoverService(CoverArtProvider* provider,
                                         MediaIoScheduler& mediaIoScheduler,
                                         QObject* parent)
    : QObject(parent)
    , m_provider(provider)
    , m_mediaIoScheduler(mediaIoScheduler)
{
    m_mediaIoScheduler.setCurrentGeneration(kCoverServiceOwner, m_generation);
    m_resultTimer.setInterval(20);
    m_resultTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_resultTimer, &QTimer::timeout, this, &LibraryCoverService::collectResults);
    m_resultTimer.start();
}

QString LibraryCoverService::cacheKey(const QString& path, const QString& trackId)
{
    if (!trackId.isEmpty())
        return QStringLiteral("track_") + trackId;
    return pathKey(path);
}

QString LibraryCoverService::urlForKey(const QString& key) const
{
    if (key.isEmpty())
        return {};

    const quint64 gen = m_urlGen.value(key, 0);
    if (gen == 0)
        return {};

    return QStringLiteral("image://coverart/") + key
           + QStringLiteral("?t=") + QString::number(gen);
}

QString LibraryCoverService::urlForPath(const QString& path, const QString& trackId) const
{
    return urlForKey(cacheKey(path, trackId));
}

void LibraryCoverService::storeCover(const QString& key, const QByteArray& data,
                                     const QString& path, const QString& trackId)
{
    if (key.isEmpty() || data.isEmpty() || !m_provider)
        return;

    m_provider->setCover(key, data);
    m_loaded.insert(key);
    m_urlGen[key] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    emit coverReady(path, trackId, urlForKey(key));
}

void LibraryCoverService::preload(const QString& path, const QString& trackId)
{
    const QString key = cacheKey(path, trackId);
    if (key.isEmpty() || m_pending.contains(key))
        return;

    if (m_loaded.contains(key) || (m_provider && m_provider->hasCover(key))) {
        if (!m_urlGen.contains(key))
            m_urlGen[key] = 1;
        m_loaded.insert(key);
        emit coverReady(path, trackId, urlForKey(key));
        return;
    }

    PendingRequest pending;
    pending.requestId = m_nextRequestId++;
    pending.path = path;
    pending.trackId = trackId;
    pending.cancellation = std::make_shared<std::atomic_bool>(false);
    m_pending.insert(key, pending);

    MediaIoRequest request;
    request.type = MediaIoRequestType::ReadCoverArt;
    request.priority = MediaIoPriority::CoverArt;
    request.ownerId = kCoverServiceOwner;
    request.requestId = pending.requestId;
    request.generation = m_generation;
    request.inputPath = path;
    request.maximumBytes = 16 * 1024 * 1024;
    request.cancellation = pending.cancellation;
    if (!m_mediaIoScheduler.enqueue(std::move(request)))
        m_pending.remove(key);
}

void LibraryCoverService::publishCover(const QString& trackId, const QByteArray& data)
{
    if (trackId.isEmpty() || data.isEmpty())
        return;

    const QString key = QStringLiteral("track_") + trackId;
    if (m_loaded.contains(key) && m_urlGen.contains(key))
        return;

    storeCover(key, data, {}, trackId);
}

void LibraryCoverService::collectResults()
{
    for (auto& result : m_mediaIoScheduler.takeResultsForOwner(kCoverServiceOwner)) {
        if (result.generation != m_generation)
            continue;
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (it->requestId != result.requestId)
                continue;
            const QString key = it.key();
            const auto pending = it.value();
            m_pending.erase(it);
            if (result.success && !result.cancelled && !result.stale && !result.data.isEmpty())
                storeCover(key, result.data, pending.path, pending.trackId);
            break;
        }
    }
}

void LibraryCoverService::clearCache()
{
    for (auto& pending : m_pending)
        pending.cancellation->store(true, std::memory_order_release);
    ++m_generation;
    m_mediaIoScheduler.setCurrentGeneration(kCoverServiceOwner, m_generation);
    m_urlGen.clear();
    m_pending.clear();
    m_loaded.clear();
}
