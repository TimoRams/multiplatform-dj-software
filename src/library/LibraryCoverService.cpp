#include "LibraryCoverService.h"
#include "CoverArtProvider.h"
#include "CoverArtExtractor.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QtConcurrent>

namespace {

QString pathKey(const QString& path)
{
    if (path.isEmpty())
        return {};
    return QStringLiteral("path_") + QFileInfo(path).absoluteFilePath();
}

} // namespace

LibraryCoverService::LibraryCoverService(CoverArtProvider* provider, QObject* parent)
    : QObject(parent)
    , m_provider(provider)
{
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

    m_pending.insert(key);

    (void)QtConcurrent::run([this, path, trackId]() {
        const auto result = CoverArtExtractor::extractCoverArt(path);
        const QByteArray data = result.first;
        QMetaObject::invokeMethod(this, "finishLoad", Qt::QueuedConnection,
                                  Q_ARG(QString, path),
                                  Q_ARG(QString, trackId),
                                  Q_ARG(QByteArray, data));
    });
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

void LibraryCoverService::finishLoad(const QString& path, const QString& trackId,
                                     const QByteArray& data)
{
    const QString key = cacheKey(path, trackId);
    m_pending.remove(key);

    if (!data.isEmpty())
        storeCover(key, data, path, trackId);
}

void LibraryCoverService::clearCache()
{
    m_urlGen.clear();
    m_pending.clear();
    m_loaded.clear();
}
