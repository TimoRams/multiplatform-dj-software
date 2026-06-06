#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

class CoverArtProvider;

// Async cover-art loader for library rows. Keys covers by track id (preferred) or path.
class LibraryCoverService : public QObject
{
    Q_OBJECT

public:
    explicit LibraryCoverService(CoverArtProvider* provider, QObject* parent = nullptr);

    Q_INVOKABLE QString urlForPath(const QString& path, const QString& trackId = {}) const;
    Q_INVOKABLE void preload(const QString& path, const QString& trackId = {});
    Q_INVOKABLE void publishCover(const QString& trackId, const QByteArray& data);
    Q_INVOKABLE void clearCache();

signals:
    void coverReady(const QString& path, const QString& trackId, const QString& imageUrl);

private slots:
    void finishLoad(const QString& path, const QString& trackId, const QByteArray& data);

private:
    static QString cacheKey(const QString& path, const QString& trackId);
    QString urlForKey(const QString& key) const;
    void storeCover(const QString& key, const QByteArray& data,
                    const QString& path, const QString& trackId);

    CoverArtProvider* m_provider = nullptr;
    QHash<QString, quint64> m_urlGen;
    QSet<QString> m_pending;
    QSet<QString> m_loaded;
};
