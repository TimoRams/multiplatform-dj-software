#include "CollectionHealth.h"
#include "LibraryDatabase.h"
#include "LibrarySearchQuery.h"
#include "PlaylistFiles.h"
#include "waveform/WaveformCache.h"

#include <QDateTime>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrent>

#include <taglib/fileref.h>
#include <taglib/tag.h>

using namespace brockdj::library;

namespace {

const QString kLastMissingScanKey = QStringLiteral("health_last_missing_scan");
constexpr int kWaveformPps = 600;

QString fromTagString(const TagLib::String& value)
{
    return QString::fromStdWString(value.toWString()).trimmed();
}

bool isPlaceholder(const QString& value, bool title)
{
    const QString v = value.trimmed().toLower();
    if (v.isEmpty() || v == QStringLiteral("unknown") || v == QStringLiteral("<unknown>"))
        return true;
    if (title) {
        static const QRegularExpression trackN(QStringLiteral("^track.*\\d$"));
        return v == QStringLiteral("untitled") || trackN.match(v).hasMatch();
    }
    return v == QStringLiteral("unknown artist") || v == QStringLiteral("artist");
}

struct TagJob {
    QString id;
    QString path;
    QString artist;
    QString title;
};

// Worker side of refreshTagsFromFiles: better artist/title per track, or nothing.
QList<TagJob> readBetterTags(const QList<TagJob>& jobs)
{
    QList<TagJob> updates;
    for (const TagJob& job : jobs) {
        QString artist;
        QString title;
        if (QFileInfo::exists(job.path)) {
#ifdef Q_OS_WIN
            TagLib::FileRef file(reinterpret_cast<const wchar_t*>(job.path.utf16()), false);
#else
            TagLib::FileRef file(job.path.toUtf8().constData(), false);
#endif
            if (!file.isNull() && file.tag()) {
                artist = fromTagString(file.tag()->artist());
                title = fromTagString(file.tag()->title());
            }
        }
        const ArtistTitle parsed = artistTitleFromFileName(job.path);
        if (isPlaceholder(artist, false))
            artist = parsed.artist;
        if (isPlaceholder(title, true))
            title = parsed.title;

        TagJob update{job.id, job.path, job.artist, job.title};
        bool changed = false;
        if (isPlaceholder(job.artist, false) && !isPlaceholder(artist, false) && artist != job.artist) {
            update.artist = artist;
            changed = true;
        }
        if (isPlaceholder(job.title, true) && !isPlaceholder(title, true) && title != job.title) {
            update.title = title;
            changed = true;
        }
        if (changed)
            updates.append(update);
    }
    return updates;
}

QStringList filesNamed(const QString& root, const QSet<QString>& lowerNames)
{
    QStringList found;
    QDirIterator it(root, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (lowerNames.contains(it.fileName().toLower()))
            found.append(it.filePath());
    }
    return found;
}

// Network/content URIs cannot be checked with QFileInfo; never flag them.
bool isCheckablePath(const QString& path)
{
    return !path.isEmpty() && !path.contains(QStringLiteral("://"));
}

} // namespace

bool LibraryDatabase::beginHealthTask(const QString& task)
{
    if (!m_db.isOpen() || !m_healthTask.isEmpty())
        return false;
    m_healthTask = task;
    emit healthBusyChanged();
    return true;
}

void LibraryDatabase::finishHealthTask(const QVariantMap& result)
{
    const QString task = m_healthTask;
    m_healthTask.clear();
    emit healthBusyChanged();
    if (m_db.isOpen()) {
        scheduleTableModelRefresh();
        scheduleBackupSync();
        emit collectionHealthChanged();
    }
    emit healthTaskFinished(task, result);
}

QVariantMap LibraryDatabase::collectionHealthSummary() const
{
    QVariantMap summary;
    if (!m_db.isOpen())
        return summary;

    const auto count = [this](const QString& condition, const QVariantMap& bindings) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
                      "SELECT COUNT(DISTINCT t.id) FROM Tracks t JOIN Locations l ON t.id = l.track_id")
                  + (condition.isEmpty() ? QString() : QStringLiteral(" WHERE ") + condition));
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
            q.bindValue(it.key(), it.value());
        if (!q.exec() || !q.next()) {
            qWarning() << "[LibraryDatabase] collectionHealthSummary:" << q.lastError().text();
            return 0;
        }
        return q.value(0).toInt();
    };

    summary.insert(QStringLiteral("total"), count({}, {}));
    for (const QString& check : collectionHealthChecks()) {
        const auto search = compileLibrarySearch(QStringLiteral("health:") + check);
        summary.insert(check, count(search.condition, search.bindings));
        const auto all = compileLibrarySearch(QStringLiteral("is:") + check);
        summary.insert(check + QStringLiteral("Ignored"),
                       count(all.condition, all.bindings) - summary.value(check).toInt());
    }
    summary.insert(QStringLiteral("lastMissingScan"), getSetting(kLastMissingScanKey, QStringLiteral("0")).toLongLong());
    return summary;
}

bool LibraryDatabase::setHealthIssueIgnored(const QString& trackId, const QString& check, bool ignored)
{
    const QString name = normalizeHealthCheck(check);
    if (!m_db.isOpen() || trackId.isEmpty() || name.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare(ignored ? QStringLiteral("INSERT OR IGNORE INTO HealthIgnores (track_id, check_name) VALUES (:id, :c)")
                      : QStringLiteral("DELETE FROM HealthIgnores WHERE track_id = :id AND check_name = :c"));
    q.bindValue(QStringLiteral(":id"), trackId);
    q.bindValue(QStringLiteral(":c"), name);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] setHealthIssueIgnored:" << q.lastError().text();
        return false;
    }
    scheduleTableModelRefresh();
    scheduleBackupSync();
    emit collectionHealthChanged();
    return true;
}

bool LibraryDatabase::isHealthIssueIgnored(const QString& trackId, const QString& check) const
{
    const QString name = normalizeHealthCheck(check);
    if (!m_db.isOpen() || trackId.isEmpty() || name.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM HealthIgnores WHERE track_id = :id AND check_name = :c"));
    q.bindValue(QStringLiteral(":id"), trackId);
    q.bindValue(QStringLiteral(":c"), name);
    return q.exec() && q.next();
}

int LibraryDatabase::clearHealthIgnores(const QString& check)
{
    const QString name = normalizeHealthCheck(check);
    if (!m_db.isOpen() || name.isEmpty())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM HealthIgnores WHERE check_name = :c"));
    q.bindValue(QStringLiteral(":c"), name);
    if (!q.exec())
        return 0;
    const int removed = q.numRowsAffected();
    scheduleTableModelRefresh();
    scheduleBackupSync();
    emit collectionHealthChanged();
    return removed;
}

bool LibraryDatabase::scanMissingFiles()
{
    if (!beginHealthTask(QStringLiteral("scan")))
        return false;

    QStringList paths;
    {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral("SELECT DISTINCT file_path FROM Locations"))) {
            while (q.next())
                paths.append(q.value(0).toString());
        }
    }

    auto* watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, checked = paths.size()] {
        const QStringList missing = watcher->result();
        watcher->deleteLater();
        if (m_db.isOpen()) {
            m_db.transaction();
            QSqlQuery q(m_db);
            bool ok = q.exec(QStringLiteral("DELETE FROM MissingFiles"));
            q.prepare(QStringLiteral("INSERT OR IGNORE INTO MissingFiles (file_path) VALUES (:p)"));
            for (const QString& path : missing) {
                q.bindValue(QStringLiteral(":p"), path);
                ok &= q.exec();
            }
            if (ok) {
                m_db.commit();
                setSetting(kLastMissingScanKey, QString::number(QDateTime::currentSecsSinceEpoch()));
            } else {
                qWarning() << "[LibraryDatabase] scanMissingFiles:" << q.lastError().text();
                m_db.rollback();
            }
        }
        finishHealthTask({{QStringLiteral("checked"), checked}, {QStringLiteral("missing"), missing.size()}});
    });
    watcher->setFuture(QtConcurrent::run([paths] {
        QStringList missing;
        for (const QString& path : paths) {
            if (isCheckablePath(path) && !QFileInfo::exists(path))
                missing.append(path);
        }
        return missing;
    }));
    return true;
}

bool LibraryDatabase::relocateMissingTracks(const QString& folderUrlOrPath)
{
    const QString root = localPathFromUrlOrPath(folderUrlOrPath);
    if (root.isEmpty() || !QFileInfo(root).isDir() || !beginHealthTask(QStringLiteral("relocate")))
        return false;

    QStringList missing;
    {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral(
                "SELECT DISTINCT m.file_path FROM MissingFiles m JOIN Locations l ON l.file_path = m.file_path"))) {
            while (q.next())
                missing.append(q.value(0).toString());
        }
    }
    QSet<QString> names;
    for (const QString& path : std::as_const(missing))
        names.insert(QFileInfo(path).fileName().toLower());

    auto* watcher = new QFutureWatcher<RelocationResult>(this);
    connect(watcher, &QFutureWatcher<RelocationResult>::finished, this, [this, watcher] {
        const RelocationResult match = watcher->result();
        watcher->deleteLater();
        int relocated = 0;
        int conflicts = 0;
        if (m_db.isOpen()) {
            for (auto it = match.moves.cbegin(); it != match.moves.cend(); ++it) {
                QSqlQuery q(m_db);
                q.prepare(QStringLiteral("UPDATE Locations SET file_path = :new WHERE file_path = :old"));
                q.bindValue(QStringLiteral(":new"), it.value());
                q.bindValue(QStringLiteral(":old"), it.key());
                // The UNIQUE path constraint fails when the found file is already its own track.
                if (!q.exec() || q.numRowsAffected() <= 0) {
                    ++conflicts;
                    continue;
                }
                q.prepare(QStringLiteral("DELETE FROM MissingFiles WHERE file_path = :old"));
                q.bindValue(QStringLiteral(":old"), it.key());
                q.exec();
                // Keep the cached waveform so the moved file does not need a re-render.
                const QString oldCache = WaveformCache::cachePathFor(it.key(), kWaveformPps);
                const QString newCache = WaveformCache::cachePathFor(it.value(), kWaveformPps);
                if (!oldCache.isEmpty() && !newCache.isEmpty() && QFile::exists(oldCache)
                    && !QFile::exists(newCache))
                    QFile::rename(oldCache, newCache);
                ++relocated;
            }
        }
        finishHealthTask({{QStringLiteral("relocated"), relocated},
                          {QStringLiteral("ambiguous"), match.ambiguous.size()},
                          {QStringLiteral("notFound"), match.notFound.size()},
                          {QStringLiteral("conflicts"), conflicts}});
    });
    watcher->setFuture(QtConcurrent::run([root, missing, names] {
        return matchRelocations(missing, filesNamed(root, names));
    }));
    return true;
}

bool LibraryDatabase::refreshTagsFromFiles()
{
    if (!beginHealthTask(QStringLiteral("tags")))
        return false;

    QList<TagJob> jobs;
    {
        const auto search = compileLibrarySearch(QStringLiteral("health:noinfo"));
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
                      "SELECT t.id, MIN(l.file_path), COALESCE(t.artist,''), COALESCE(t.title,'')"
                      " FROM Tracks t JOIN Locations l ON t.id = l.track_id WHERE ")
                  + search.condition + QStringLiteral(" GROUP BY t.id"));
        for (auto it = search.bindings.cbegin(); it != search.bindings.cend(); ++it)
            q.bindValue(it.key(), it.value());
        if (q.exec()) {
            while (q.next())
                jobs.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                             q.value(3).toString()});
        } else {
            qWarning() << "[LibraryDatabase] refreshTagsFromFiles:" << q.lastError().text();
        }
    }

    auto* watcher = new QFutureWatcher<QList<TagJob>>(this);
    connect(watcher, &QFutureWatcher<QList<TagJob>>::finished, this, [this, watcher, checked = jobs.size()] {
        const QList<TagJob> updates = watcher->result();
        watcher->deleteLater();
        int updated = 0;
        if (m_db.isOpen()) {
            for (const TagJob& update : updates) {
                QSqlQuery q(m_db);
                q.prepare(QStringLiteral("UPDATE Tracks SET artist = :a, title = :t WHERE id = :id"));
                q.bindValue(QStringLiteral(":a"), update.artist);
                q.bindValue(QStringLiteral(":t"), update.title);
                q.bindValue(QStringLiteral(":id"), update.id);
                if (q.exec()) {
                    ++updated;
                    emit trackMetaChanged(update.id);
                }
            }
        }
        finishHealthTask({{QStringLiteral("checked"), checked}, {QStringLiteral("updated"), updated}});
    });
    watcher->setFuture(QtConcurrent::run(readBetterTags, jobs));
    return true;
}

int LibraryDatabase::removeMissingTracks()
{
    if (!m_db.isOpen() || !m_healthTask.isEmpty())
        return 0;
    QStringList ids;
    {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral(
                "SELECT t.id FROM Tracks t"
                " WHERE EXISTS (SELECT 1 FROM Locations l WHERE l.track_id = t.id)"
                "   AND NOT EXISTS (SELECT 1 FROM Locations l WHERE l.track_id = t.id"
                "                   AND l.file_path NOT IN (SELECT file_path FROM MissingFiles))"
                "   AND NOT EXISTS (SELECT 1 FROM HealthIgnores hi"
                "                   WHERE hi.track_id = t.id AND hi.check_name = 'missing')"))) {
            while (q.next())
                ids.append(q.value(0).toString());
        }
    }
    int removed = 0;
    for (const QString& id : std::as_const(ids)) {
        if (removeTrackFromLibrary(id))
            ++removed;
    }
    QSqlQuery cleanup(m_db);
    cleanup.exec(QStringLiteral(
        "DELETE FROM MissingFiles WHERE file_path NOT IN (SELECT file_path FROM Locations)"));
    emit collectionHealthChanged();
    return removed;
}
