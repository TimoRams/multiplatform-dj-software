#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>

class CoverArtProvider;
class MediaIoScheduler;

// Async cover-art loader for library rows. Keys covers by track id (preferred) or path.
class LibraryCoverService : public QObject
{
    Q_OBJECT

public:
    explicit LibraryCoverService(CoverArtProvider* provider,
                                 MediaIoScheduler& mediaIoScheduler,
                                 QObject* parent = nullptr);

    Q_INVOKABLE QString urlForPath(const QString& path, const QString& trackId = {}) const;
    Q_INVOKABLE void preload(const QString& path, const QString& trackId = {});
    Q_INVOKABLE void publishCover(const QString& trackId, const QByteArray& data);
    Q_INVOKABLE void clearCache();

signals:
    void coverReady(const QString& path, const QString& trackId, const QString& imageUrl);

private:
    struct PendingRequest {
        std::uint64_t requestId = 0;
        QString path;
        QString trackId;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    static QString cacheKey(const QString& path, const QString& trackId);
    QString urlForKey(const QString& key) const;
    void storeCover(const QString& key, const QByteArray& data,
                    const QString& path, const QString& trackId);
    void collectResults();

    CoverArtProvider* m_provider = nullptr;
    MediaIoScheduler& m_mediaIoScheduler;
    QTimer m_resultTimer;
    QHash<QString, quint64> m_urlGen;
    QHash<QString, PendingRequest> m_pending;
    QSet<QString> m_loaded;
    std::uint64_t m_generation = 1;
    std::uint64_t m_nextRequestId = 1;
};
