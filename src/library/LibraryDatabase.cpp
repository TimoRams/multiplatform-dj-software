#include "LibraryDatabase.h"
#include "LibraryTableModel.h"
#include "app/SettingsManager.h"
#include "rendering/WaveformCache.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <thread>

namespace {

bool tableHasColumn(QSqlDatabase& db, const QString& tableName, const QString& columnName)
{
    QSqlQuery q(db);
    if (!q.exec(QString("PRAGMA table_info(%1)").arg(tableName)))
        return false;

    while (q.next()) {
        if (q.value(1).toString().compare(columnName, Qt::CaseInsensitive) == 0)
            return true;
    }

    return false;
}

bool syncBackupFromPath(const QString& activePath, const QString& mirrorPath)
{
    if (activePath.isEmpty() || mirrorPath.isEmpty())
        return false;

    const QString connectionName =
        QStringLiteral("library_backup_sync_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(activePath);

        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)"));

            QFile::remove(mirrorPath);
            QFile::remove(mirrorPath + QStringLiteral("-wal"));
            QFile::remove(mirrorPath + QStringLiteral("-shm"));

            QString vacuumTarget = mirrorPath;
            vacuumTarget.replace(QLatin1Char('\''), QStringLiteral("''"));
            ok = q.exec(QStringLiteral("VACUUM INTO '%1'").arg(vacuumTarget));
            if (!ok)
                qWarning() << "[LibraryDatabase] Deferred VACUUM INTO failed for"
                           << mirrorPath << ':' << q.lastError().text();
        } else {
            qWarning() << "[LibraryDatabase] Deferred backup DB open failed:" << db.lastError().text();
        }

        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

}

QString LibraryDatabase::trackSegmentsToJson(const std::vector<TrackSegment>& segments)
{
    QJsonArray arr;

    for (const auto& s : segments) {
        QJsonObject obj;
        obj.insert("label", s.label);
        obj.insert("startTime", s.startTime);
        obj.insert("endTime", s.endTime);
        obj.insert("colorHex", s.colorHex);
        arr.append(obj);
    }

    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVariantList LibraryDatabase::trackSegmentsJsonToVariantList(const QString& json)
{
    QVariantList result;
    if (json.trimmed().isEmpty())
        return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return result;

    const QJsonArray arr = doc.array();
    result.reserve(arr.size());

    for (const auto v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        QVariantMap m;
        m.insert("label", o.value("label").toString());
        m.insert("startTime", o.value("startTime").toDouble());
        m.insert("endTime", o.value("endTime").toDouble());
        m.insert("colorHex", o.value("colorHex").toString());
        result.push_back(m);
    }

    return result;
}

LibraryDatabase::LibraryDatabase(QObject* parent)
    : QObject(parent)
{
    m_mirrorSelfCheckTimer.setInterval(3000);
    m_mirrorSelfCheckTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_mirrorSelfCheckTimer, &QTimer::timeout, this, &LibraryDatabase::performMirrorSelfCheck);

    m_backupSyncTimer.setSingleShot(true);
    m_backupSyncTimer.setInterval(1500);
    m_backupSyncTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_backupSyncTimer, &QTimer::timeout, this, &LibraryDatabase::startDeferredBackupSync);
}

LibraryDatabase::~LibraryDatabase()
{
    if (m_db.isOpen())
        m_db.close();
}

bool LibraryDatabase::open()
{
    QElapsedTimer timer;
    timer.start();

    // ── Determine the database directory ─────────────────────────────────
    // Use the same config directory as SettingsManager for consistency.
    // This ensures all config files, settings, and database live in the same place.
    QString configDir = SettingsManager::getInstance().getConfigDirectoryPath();
    if (configDir.isEmpty()) {
        qWarning() << "[LibraryDatabase] SettingsManager config directory is empty";
        return false;
    }

    QDir dbDir(configDir + "/db");
    if (!dbDir.exists()) {
        if (!dbDir.mkpath(".")) {
            qWarning() << "[LibraryDatabase] Failed to create db directory:" << dbDir.absolutePath();
            return false;
        }
    }

    const QString legacyDbPath = dbDir.filePath("RamsbrockDJ_Library.db");
    m_dbPath = dbDir.filePath("RamsbrockDJ_Library_A.db");
    m_backupDbPath = dbDir.filePath("RamsbrockDJ_Library_B.db");
    m_activeDbPath = m_dbPath;
    m_manualBackupDbPath = dbDir.filePath("RamsbrockDJ_Library_backup_manual.db");

    qDebug() << "[LibraryDatabase] DB primary path:" << m_dbPath;
    qDebug() << "[LibraryDatabase] DB backup path:" << m_backupDbPath;
    qDebug() << "[LibraryDatabase] DB manual backup path:" << m_manualBackupDbPath;

    const bool primaryHealthy = isHealthyDatabaseFile(m_dbPath);
    const bool backupHealthy = isHealthyDatabaseFile(m_backupDbPath);

    if (!primaryHealthy && backupHealthy) {
        qWarning() << "[LibraryDatabase] Primary DB is not healthy, restoring from backup";
        if (!restorePrimaryFromBackup())
            return false;
    } else if (!primaryHealthy && !backupHealthy && isHealthyDatabaseFile(legacyDbPath)) {
        qWarning() << "[LibraryDatabase] Migrating legacy DB into mirrored layout";
        if (!copyDatabaseFile(legacyDbPath, m_dbPath))
            return false;
    } else if (primaryHealthy && !backupHealthy) {
        qWarning() << "[LibraryDatabase] Backup DB is not healthy, refreshing from primary";
        if (!copyDatabaseFile(m_dbPath, m_backupDbPath))
            qWarning() << "[LibraryDatabase] Failed to refresh backup DB from primary";
    } else if (!primaryHealthy && !backupHealthy) {
        qWarning() << "[LibraryDatabase] Neither DB copy is healthy, creating a fresh database";
        m_lastRecoveryEvent = QStringLiteral("Both databases missing/corrupted — fresh database created");
    }

    // ── Open via QSqlDatabase ────────────────────────────────────────────
    clearDatabaseConnection();
    m_db = QSqlDatabase::addDatabase("QSQLITE", "library_conn");
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "[LibraryDatabase] Failed to open:" << m_db.lastError().text();
        return false;
    }
    qDebug() << "[LibraryDatabase] open(): connection established in" << timer.elapsed() << "ms";

    // Enable WAL mode for better concurrency and foreign keys.
    {
        QSqlQuery pragma(m_db);
        pragma.exec("PRAGMA journal_mode=WAL");
        pragma.exec("PRAGMA foreign_keys=ON");
    }

    if (!createSchema())
        return false;
    qDebug() << "[LibraryDatabase] open(): schema ready in" << timer.elapsed() << "ms";

    if (!syncBackupFromPrimary())
        qWarning() << "[LibraryDatabase] Failed to sync backup DB after open";

    if (!primaryHealthy && backupHealthy)
        m_primaryMirrorDegraded = true;
    if (primaryHealthy && !backupHealthy)
        m_backupMirrorDegraded = true;
    if (!primaryHealthy && !backupHealthy) {
        m_primaryMirrorDegraded = true;
        m_backupMirrorDegraded = true;
    }

    m_cachedMirrorStatus = mirroredDatabaseStatus();
    emit mirroredDatabaseStatusChanged();
    if (!m_mirrorSelfCheckTimer.isActive())
        m_mirrorSelfCheckTimer.start();

    qDebug() << "[LibraryDatabase] open(): finished in" << timer.elapsed() << "ms";

    return true;
}

bool LibraryDatabase::createSchema()
{
    QSqlQuery q(m_db);

    // ── Schema version bookkeeping ───────────────────────────────────────
    q.exec("CREATE TABLE IF NOT EXISTS Meta ("
           "  key   TEXT PRIMARY KEY,"
           "  value TEXT"
           ")");

    q.prepare("SELECT value FROM Meta WHERE key = 'schema_version'");
    q.exec();

    int currentVersion = 0;
    if (q.next())
        currentVersion = q.value(0).toInt();

    if (currentVersion >= kSchemaVersion)
        return true; // already up to date

    // ── Version 1: initial tables ────────────────────────────────────────
    if (currentVersion < 1) {
        bool ok = true;

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS Tracks ("
            "  id           TEXT PRIMARY KEY,"
            "  title        TEXT,"
            "  artist       TEXT,"
            "  duration_sec INTEGER,"
            "  bitrate_kbps INTEGER DEFAULT 0,"
            "  bpm          REAL    DEFAULT 0.0,"
            "  key          TEXT    DEFAULT '',"
            "  main_cue_sec REAL    DEFAULT -1.0,"
            "  track_segments TEXT DEFAULT '',"
            "  is_analyzed  BOOLEAN DEFAULT 0"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] Tracks:" << q.lastError().text();

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS Locations ("
            "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  track_id  TEXT,"
            "  file_path TEXT UNIQUE,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] Locations:" << q.lastError().text();

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS CuePoints ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  track_id   TEXT,"
            "  cue_index  INTEGER,"
            "  position_sec REAL,"
            "  label      TEXT DEFAULT '',"
            "  color      TEXT DEFAULT '#FF0000',"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] CuePoints:" << q.lastError().text();

        if (!ok) return false;
    }

    if (currentVersion < 2) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "first_beat_sample")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN first_beat_sample INTEGER DEFAULT 0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks first_beat_sample:" << q.lastError().text();
        }

        if (!tableHasColumn(m_db, "Tracks", "analysis_sample_rate")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN analysis_sample_rate REAL DEFAULT 44100.0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks analysis_sample_rate:" << q.lastError().text();
        }

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS BeatGridMarkers ("
            "  track_id     TEXT NOT NULL,"
            "  beat_index   INTEGER NOT NULL,"
            "  position_sec REAL NOT NULL,"
            "  is_downbeat  INTEGER NOT NULL DEFAULT 0,"
            "  bar_number   INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY(track_id, beat_index),"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] BeatGridMarkers:" << q.lastError().text();

        if (!ok) return false;
    }

    if (currentVersion < 3) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "bitrate_kbps")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN bitrate_kbps INTEGER DEFAULT 0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks bitrate_kbps:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 4) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "track_segments")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN track_segments TEXT DEFAULT ''");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks track_segments:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 5) {
        bool ok = true;

        ok &= q.exec(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_cuepoints_track_slot "
            "ON CuePoints(track_id, cue_index)");
        if (!ok) qWarning() << "[LibraryDatabase] CuePoints index:" << q.lastError().text();

        if (!tableHasColumn(m_db, "CuePoints", "label")) {
            ok &= q.exec("ALTER TABLE CuePoints ADD COLUMN label TEXT DEFAULT ''");
            if (!ok) qWarning() << "[LibraryDatabase] CuePoints label:" << q.lastError().text();
        }

        if (!tableHasColumn(m_db, "CuePoints", "color")) {
            ok &= q.exec("ALTER TABLE CuePoints ADD COLUMN color TEXT DEFAULT '#FF0000'");
            if (!ok) qWarning() << "[LibraryDatabase] CuePoints color:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 6) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "main_cue_sec")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN main_cue_sec REAL DEFAULT -1.0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks main_cue_sec:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 7) {
        bool ok = true;

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS Playlists ("
            "  id         TEXT PRIMARY KEY,"
            "  name       TEXT NOT NULL,"
            "  parent_id  TEXT DEFAULT NULL,"
            "  sort_order INTEGER DEFAULT 0,"
            "  FOREIGN KEY(parent_id) REFERENCES Playlists(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] Playlists:" << q.lastError().text();

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS PlaylistItems ("
            "  playlist_id TEXT NOT NULL,"
            "  track_id    TEXT NOT NULL,"
            "  position    INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY(playlist_id, track_id),"
            "  FOREIGN KEY(playlist_id) REFERENCES Playlists(id) ON DELETE CASCADE,"
            "  FOREIGN KEY(track_id)    REFERENCES Tracks(id)    ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] PlaylistItems:" << q.lastError().text();

        if (!ok) return false;
    }

    if (currentVersion < 8) {
        // Version 8: reserved (FLAC metadata fix applied manually to existing databases;
        // code-level fix via TagLib means new data is stored correctly from the start).
    }

    // ── Stamp current version ────────────────────────────────────────────
    q.prepare("INSERT OR REPLACE INTO Meta (key, value) VALUES ('schema_version', :v)");
    q.bindValue(":v", kSchemaVersion);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] Meta schema_version:" << q.lastError().text();
        return false;
    }

    qDebug() << "[LibraryDatabase] Schema created/updated to version" << kSchemaVersion;
    return true;
}

bool LibraryDatabase::addTrack(const QString& trackId,
                               const QString& title,
                               const QString& artist,
                               int durationSec,
                               const QString& filePath,
                               int bitrateKbps)
{
    if (!m_db.isOpen()) {
        qWarning() << "[LibraryDatabase] addTrack: database not open";
        return false;
    }

    qDebug() << "[LibraryDatabase] addTrack:" << trackId.left(12) << title << artist;
    QSqlQuery q(m_db);
    bool trackInserted = false;
    bool locationInserted = false;

    // INSERT OR IGNORE: don't overwrite existing metadata if re-added.
    q.prepare("INSERT OR IGNORE INTO Tracks (id, title, artist, duration_sec, bitrate_kbps)"
              " VALUES (:id, :title, :artist, :dur, :kbps)");
    q.bindValue(":id",     trackId);
    q.bindValue(":title",  title);
    q.bindValue(":artist", artist);
    q.bindValue(":dur",    durationSec);
    q.bindValue(":kbps",   bitrateKbps);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] addTrack Tracks:" << q.lastError().text();
        if (q.lastError().text().contains("unable to open") || q.lastError().text().contains("disk image")) {
            qWarning() << "[LibraryDatabase] Database file may have been deleted; trigger recovery";
            QTimer::singleShot(100, this, &LibraryDatabase::performMirrorSelfCheck);
        }
        return false;
    }
    trackInserted = q.numRowsAffected() > 0;

    // Update mutable fields when a previously-known track is reloaded with fresh metadata.
    q.prepare("UPDATE Tracks SET "
              "  bitrate_kbps = CASE WHEN :kbps > 0 THEN :kbps ELSE bitrate_kbps END,"
              "  title  = CASE WHEN :title  <> '' THEN :title  ELSE title  END,"
              "  artist = CASE WHEN :artist <> '' THEN :artist ELSE artist END "
              "WHERE id = :id");
    q.bindValue(":kbps",   bitrateKbps);
    q.bindValue(":title",  title);
    q.bindValue(":artist", artist);
    q.bindValue(":id",     trackId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] addTrack metadata update:" << q.lastError().text();
    }

    // Insert location (UNIQUE on file_path prevents duplicates).
    q.prepare("INSERT OR IGNORE INTO Locations (track_id, file_path)"
              " VALUES (:tid, :fp)");
    q.bindValue(":tid", trackId);
    q.bindValue(":fp",  filePath);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] addTrack Locations:" << q.lastError().text();
        return false;
    }
    locationInserted = q.numRowsAffected() > 0;

    if (trackInserted || locationInserted)
        scheduleTableModelRefresh();

    emit trackAdded(trackId);
    scheduleBackupSync();
    return true;
}

void LibraryDatabase::updateAnalysisData(const QString& trackId,
                                         float newBpm,
                                         const QString& newKey,
                                         qint64 firstBeatSample,
                                         double sampleRate,
                                         const std::vector<TrackData::BeatMarker>& beatGrid)
{
    if (trackId.isEmpty())
        return;

    qDebug() << "[LibraryDatabase] updateAnalysisData:" << trackId.left(12)
             << "bpm=" << newBpm << "key=" << newKey
             << "firstBeat=" << firstBeatSample
             << "gridBeats=" << beatGrid.size();

    if (!m_db.transaction()) {
        qWarning() << "[LibraryDatabase] updateAnalysisData begin transaction:" << m_db.lastError().text();
    }

    QSqlQuery q(m_db);
    q.prepare(
        "UPDATE Tracks SET "
        "  bpm = CASE WHEN :bpm > 0 THEN :bpm ELSE bpm END,"
        "  key = CASE WHEN length(trim(:key)) > 0 THEN :key ELSE key END,"
        "  is_analyzed = CASE WHEN (:bpm > 0 OR length(trim(:key)) > 0) THEN 1 ELSE is_analyzed END,"
        "  first_beat_sample = CASE WHEN :firstBeatSample >= 0 THEN :firstBeatSample ELSE first_beat_sample END,"
        "  analysis_sample_rate = CASE WHEN :sampleRate > 0 THEN :sampleRate ELSE analysis_sample_rate END"
        " WHERE id = :id");
    q.bindValue(":bpm", static_cast<double>(newBpm));
    q.bindValue(":key", newKey);
    q.bindValue(":firstBeatSample", firstBeatSample);
    q.bindValue(":sampleRate", sampleRate);
    q.bindValue(":id",  trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] updateAnalysisData:" << q.lastError().text();
        m_db.rollback();
        return;
    }

    if (!beatGrid.empty()) {
        q.prepare("DELETE FROM BeatGridMarkers WHERE track_id = :id");
        q.bindValue(":id", trackId);
        if (!q.exec()) {
            qWarning() << "[LibraryDatabase] clear BeatGridMarkers:" << q.lastError().text();
            m_db.rollback();
            return;
        }

        q.prepare(
            "INSERT INTO BeatGridMarkers (track_id, beat_index, position_sec, is_downbeat, bar_number) "
            "VALUES (:trackId, :beatIndex, :positionSec, :isDownbeat, :barNumber)");

        for (int beatIndex = 0; beatIndex < static_cast<int>(beatGrid.size()); ++beatIndex) {
            const auto& marker = beatGrid[static_cast<size_t>(beatIndex)];
            q.bindValue(":trackId", trackId);
            q.bindValue(":beatIndex", beatIndex);
            q.bindValue(":positionSec", marker.positionSec);
            q.bindValue(":isDownbeat", marker.isDownbeat ? 1 : 0);
            q.bindValue(":barNumber", marker.barNumber);

            if (!q.exec()) {
                qWarning() << "[LibraryDatabase] insert BeatGridMarkers:" << q.lastError().text();
                m_db.rollback();
                return;
            }
        }
    }

    if (!m_db.commit()) {
        qWarning() << "[LibraryDatabase] updateAnalysisData commit:" << m_db.lastError().text();
        m_db.rollback();
        return;
    }

    if (m_tableModel != nullptr)
        m_tableModel->updateAnalysisForTrack(trackId,
                                             static_cast<double>(newBpm),
                                             newKey,
                                             newBpm > 0.0f || !newKey.trimmed().isEmpty());

    emit analysisUpdated(trackId);
    scheduleBackupSync();
}

bool LibraryDatabase::tryGetAnalysisData(const QString& trackId, AnalysisSnapshot* out) const
{
    if (!out || trackId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT bpm, key, is_analyzed, "
        "       COALESCE(first_beat_sample, 0), "
        "       COALESCE(analysis_sample_rate, 44100.0) "
        "FROM Tracks WHERE id = :id LIMIT 1");
    q.bindValue(":id", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] tryGetAnalysisData:" << q.lastError().text();
        return false;
    }

    if (!q.next())
        return false;

    AnalysisSnapshot snapshot;
    snapshot.bpm = q.value(0).toDouble();
    snapshot.key = q.value(1).toString();
    snapshot.isAnalyzed = q.value(2).toBool();
    snapshot.firstBeatSample = q.value(3).toLongLong();
    snapshot.sampleRate = q.value(4).toDouble();

    QSqlQuery beatsQuery(m_db);
    beatsQuery.prepare(
        "SELECT position_sec, is_downbeat, bar_number "
        "FROM BeatGridMarkers WHERE track_id = :id ORDER BY beat_index ASC");
    beatsQuery.bindValue(":id", trackId);

    if (!beatsQuery.exec()) {
        qWarning() << "[LibraryDatabase] tryGetAnalysisData beatgrid:" << beatsQuery.lastError().text();
        return false;
    }

    while (beatsQuery.next()) {
        TrackData::BeatMarker marker;
        marker.positionSec = beatsQuery.value(0).toDouble();
        marker.isDownbeat = beatsQuery.value(1).toInt() != 0;
        marker.barNumber = beatsQuery.value(2).toInt();
        snapshot.beatGrid.push_back(marker);
    }

    *out = std::move(snapshot);
    return true;
}

bool LibraryDatabase::trackExists(const QString& trackId) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM Tracks WHERE id = :id LIMIT 1");
    q.bindValue(":id", trackId);
    q.exec();
    return q.next();
}

bool LibraryDatabase::updateTrackSegments(const QString& trackId,
                                          const std::vector<TrackSegment>& segments)
{
    if (trackId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    if (!m_db.isOpen()) {
        qWarning() << "[LibraryDatabase] updateTrackSegments: database not open";
        return false;
    }

    q.prepare("UPDATE Tracks SET track_segments = :segments WHERE id = :id");
    q.bindValue(":segments", trackSegmentsToJson(segments));
    q.bindValue(":id", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] updateTrackSegments:" << q.lastError().text();
        if (q.lastError().text().contains("unable to open") || q.lastError().text().contains("disk image")) {
            QTimer::singleShot(100, this, &LibraryDatabase::performMirrorSelfCheck);
        }
        return false;
    }

    scheduleTableModelRefresh();

    scheduleBackupSync();

    return true;
}

QVariantList LibraryDatabase::trackSegmentsForTrack(const QString& trackId) const
{
    if (trackId.isEmpty())
        return {};

    QSqlQuery q(m_db);
    q.prepare("SELECT COALESCE(track_segments, '') FROM Tracks WHERE id = :id LIMIT 1");
    q.bindValue(":id", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] trackSegmentsForTrack:" << q.lastError().text();
        return {};
    }

    if (!q.next())
        return {};

    return trackSegmentsJsonToVariantList(q.value(0).toString());
}

bool LibraryDatabase::upsertCuePoint(const QString& trackId,
                                     int cueIndex,
                                     double positionSec,
                                     const QString& label,
                                     const QString& colorHex)
{
    if (trackId.isEmpty() || cueIndex < 0 || cueIndex >= 8 || positionSec < 0.0)
        return false;

    QSqlQuery q(m_db);
    q.prepare(
        "INSERT OR REPLACE INTO CuePoints (track_id, cue_index, position_sec, label, color) "
        "VALUES (:trackId, :cueIndex, :positionSec, :label, :color)");
    q.bindValue(":trackId", trackId);
    q.bindValue(":cueIndex", cueIndex);
    q.bindValue(":positionSec", positionSec);
    q.bindValue(":label", label);
    q.bindValue(":color", colorHex);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] upsertCuePoint:" << q.lastError().text();
        return false;
    }

    scheduleBackupSync();

    return true;
}

bool LibraryDatabase::deleteCuePoint(const QString& trackId, int cueIndex)
{
    if (trackId.isEmpty() || cueIndex < 0 || cueIndex >= 8)
        return false;

    QSqlQuery q(m_db);
    q.prepare("DELETE FROM CuePoints WHERE track_id = :trackId AND cue_index = :cueIndex");
    q.bindValue(":trackId", trackId);
    q.bindValue(":cueIndex", cueIndex);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] deleteCuePoint:" << q.lastError().text();
        return false;
    }

    scheduleBackupSync();

    return true;
}

QVariantList LibraryDatabase::cuePointsForTrack(const QString& trackId) const
{
    if (trackId.isEmpty())
        return {};

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT cue_index, position_sec, COALESCE(label, ''), COALESCE(color, '#FF0000') "
        "FROM CuePoints WHERE track_id = :trackId ORDER BY cue_index ASC");
    q.bindValue(":trackId", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] cuePointsForTrack:" << q.lastError().text();
        return {};
    }

    QVariantList cues;
    while (q.next()) {
        QVariantMap c;
        c.insert("index", q.value(0).toInt());
        c.insert("positionSec", q.value(1).toDouble());
        c.insert("label", q.value(2).toString());
        c.insert("color", q.value(3).toString());
        cues.push_back(c);
    }

    return cues;
}

bool LibraryDatabase::upsertMainCuePoint(const QString& trackId, double positionSec)
{
    if (trackId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("UPDATE Tracks SET main_cue_sec = :cueSec WHERE id = :trackId");
    q.bindValue(":cueSec", positionSec);
    q.bindValue(":trackId", trackId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] upsertMainCuePoint:" << q.lastError().text();
        return false;
    }

    scheduleBackupSync();

    return true;
}

double LibraryDatabase::mainCuePointForTrack(const QString& trackId) const
{
    if (trackId.isEmpty())
        return -1.0;

    QSqlQuery q(m_db);
    q.prepare("SELECT COALESCE(main_cue_sec, -1.0) FROM Tracks WHERE id = :trackId LIMIT 1");
    q.bindValue(":trackId", trackId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] mainCuePointForTrack:" << q.lastError().text();
        return -1.0;
    }

    if (!q.next())
        return -1.0;

    return q.value(0).toDouble();
}

QString LibraryDatabase::filePath(const QString& trackId) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT file_path FROM Locations WHERE track_id = :id LIMIT 1");
    q.bindValue(":id", trackId);
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}

QString LibraryDatabase::trackIdForFilePath(const QString& filePath) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT track_id FROM Locations WHERE file_path = :fp LIMIT 1");
    q.bindValue(":fp", filePath);
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}

QString LibraryDatabase::mirroredDatabaseStatus() const
{
    if (m_lastRecoveryEvent.isEmpty())
        return QStringLiteral("DB A: OK | DB B: OK");

    const auto describe = [this](const QString& path, bool degraded) -> QString {
        if (path.isEmpty())
            return QStringLiteral("unknown");

        const QFileInfo info(path);
        if (!info.exists())
            return QStringLiteral("missing");
        if (info.size() <= 0)
            return QStringLiteral("empty");

        if (degraded)
            return QStringLiteral("degraded");

        return isHealthyDatabaseFile(path) ? QStringLiteral("gut") : QStringLiteral("beschädigt");
    };

    const QString activeLabel = (m_activeDbPath == m_backupDbPath) ? QStringLiteral("B") : QStringLiteral("A");

    return QStringLiteral("DB A: %1 | DB B: %2 | Aktiv: %3 | Recovery: %4")
        .arg(describe(m_dbPath, m_primaryMirrorDegraded),
             describe(m_backupDbPath, m_backupMirrorDegraded),
             activeLabel,
             m_lastRecoveryEvent);
}

void LibraryDatabase::shutdown(bool syncBackup)
{
    if (!m_db.isValid() || !m_db.isOpen())
        return;

    const bool pendingBackupSync = m_backupSyncTimer.isActive() || m_backupSyncAgain;
    m_mirrorSelfCheckTimer.stop();
    m_backupSyncTimer.stop();

    {
        QSqlQuery q(m_db);
        q.exec("PRAGMA wal_checkpoint(FULL)");
    }

    if ((syncBackup || pendingBackupSync) && !m_backupSyncRunning) {
        if (!syncBackupFromPrimary())
            qWarning() << "[LibraryDatabase] Failed to sync backup DB during shutdown";
    }

    if (syncBackup) {
        const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
        if (!m_manualBackupDbPath.isEmpty() && !copyDatabaseFile(activePath, m_manualBackupDbPath))
            qWarning() << "[LibraryDatabase] Failed to write manual backup DB during shutdown";
    }

    clearDatabaseConnection();
}

void LibraryDatabase::setTableModel(LibraryTableModel* model)
{
    m_tableModel = model;
}

void LibraryDatabase::scheduleTableModelRefresh()
{
    if (m_tableModel == nullptr || m_tableModelRefreshPending)
        return;

    m_tableModelRefreshPending = true;

    // Use QueuedConnection (not a native timer) to avoid macOS CFRunLoop re-entrancy
    // that can destroy ListView delegates while signal handlers are still on the stack.
    QMetaObject::invokeMethod(this, [this]() {
        m_tableModelRefreshPending = false;

        if (m_tableModel != nullptr)
            m_tableModel->refresh();
    }, Qt::QueuedConnection);
}

void LibraryDatabase::scheduleBackupSync()
{
    if (!m_db.isValid() || !m_db.isOpen())
        return;

    m_backupSyncTimer.start();
}

void LibraryDatabase::startDeferredBackupSync()
{
    if (!m_db.isValid() || !m_db.isOpen())
        return;

    if (m_backupSyncRunning) {
        m_backupSyncAgain = true;
        return;
    }

    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    const QString mirrorPath = (activePath == m_dbPath) ? m_backupDbPath : m_dbPath;
    if (activePath.isEmpty() || mirrorPath.isEmpty())
        return;

    m_backupSyncRunning = true;
    QPointer<LibraryDatabase> self(this);
    std::thread([self, activePath, mirrorPath]() {
        const bool ok = syncBackupFromPath(activePath, mirrorPath);

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, ok]() {
            if (!self)
                return;

            self->m_backupSyncRunning = false;
            if (!ok)
                qWarning() << "[LibraryDatabase] Deferred backup DB sync failed";

            if (self->m_backupSyncAgain) {
                self->m_backupSyncAgain = false;
                self->scheduleBackupSync();
            }

            emit self->mirroredDatabaseStatusChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

// ── Playlist management ────────────────────────────────────────────────────

QString LibraryDatabase::createPlaylist(const QString& name, const QString& parentId)
{
    if (!m_db.isOpen() || name.trimmed().isEmpty())
        return {};

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlQuery q(m_db);
    // Get next sort_order among siblings.
    if (parentId.isEmpty()) {
        q.prepare("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM Playlists WHERE parent_id IS NULL");
    } else {
        q.prepare("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM Playlists WHERE parent_id = :pid");
        q.bindValue(":pid", parentId);
    }
    q.exec();
    const int sortOrder = q.next() ? q.value(0).toInt() : 0;

    q.prepare("INSERT INTO Playlists (id, name, parent_id, sort_order) VALUES (:id, :name, :pid, :so)");
    q.bindValue(":id",   id);
    q.bindValue(":name", name.trimmed());
    q.bindValue(":pid",  parentId.isEmpty() ? QVariant{} : QVariant{parentId});
    q.bindValue(":so",   sortOrder);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] createPlaylist:" << q.lastError().text();
        return {};
    }

    emit playlistsChanged();
    scheduleBackupSync();
    return id;
}

bool LibraryDatabase::deletePlaylist(const QString& playlistId)
{
    if (!m_db.isOpen() || playlistId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("DELETE FROM Playlists WHERE id = :id");
    q.bindValue(":id", playlistId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] deletePlaylist:" << q.lastError().text();
        return false;
    }

    emit playlistsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::renamePlaylist(const QString& playlistId, const QString& newName)
{
    if (!m_db.isOpen() || playlistId.isEmpty() || newName.trimmed().isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("UPDATE Playlists SET name = :name WHERE id = :id");
    q.bindValue(":name", newName.trimmed());
    q.bindValue(":id",   playlistId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] renamePlaylist:" << q.lastError().text();
        return false;
    }

    emit playlistsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::setPlaylistSortOrder(const QString& playlistId, int sortOrder)
{
    if (!m_db.isOpen() || playlistId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("UPDATE Playlists SET sort_order = :so WHERE id = :id");
    q.bindValue(":so", sortOrder);
    q.bindValue(":id", playlistId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] setPlaylistSortOrder:" << q.lastError().text();
        return false;
    }

    emit playlistsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::setPlaylistParent(const QString& playlistId, const QString& newParentId)
{
    if (!m_db.isOpen() || playlistId.isEmpty())
        return false;

    // Cycle guard: walk up from newParentId; reject if we reach playlistId.
    if (!newParentId.isEmpty()) {
        QSqlQuery check(m_db);
        QString cursor = newParentId;
        while (!cursor.isEmpty()) {
            if (cursor == playlistId)
                return false;
            check.prepare("SELECT COALESCE(parent_id, '') FROM Playlists WHERE id = :id");
            check.bindValue(":id", cursor);
            if (!check.exec() || !check.next())
                break;
            cursor = check.value(0).toString();
        }
    }

    QSqlQuery q(m_db);
    if (newParentId.isEmpty()) {
        q.prepare("UPDATE Playlists SET parent_id = NULL WHERE id = :id");
    } else {
        q.prepare("UPDATE Playlists SET parent_id = :parent WHERE id = :id");
        q.bindValue(":parent", newParentId);
    }
    q.bindValue(":id", playlistId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] setPlaylistParent:" << q.lastError().text();
        return false;
    }

    emit playlistsChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getAllPlaylists() const
{
    QVariantList result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery q(m_db);
    const bool ok = q.exec(
        "SELECT p.id, p.name, COALESCE(p.parent_id, '') AS parent_id, p.sort_order,"
        "  (SELECT COUNT(*) FROM PlaylistItems pi WHERE pi.playlist_id = p.id) AS track_count"
        " FROM Playlists p"
        " ORDER BY p.sort_order ASC, p.name ASC");
    if (!ok) {
        qWarning() << "[LibraryDatabase] getAllPlaylists:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m["id"]         = q.value(0).toString();
        m["name"]       = q.value(1).toString();
        m["parentId"]   = q.value(2).toString();
        m["sortOrder"]  = q.value(3).toInt();
        m["trackCount"] = q.value(4).toInt();
        result.push_back(m);
    }
    return result;
}

bool LibraryDatabase::addTrackToPlaylist(const QString& playlistId, const QString& trackId)
{
    if (!m_db.isOpen() || playlistId.isEmpty() || trackId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("SELECT COALESCE(MAX(position), -1) + 1 FROM PlaylistItems WHERE playlist_id = :pid");
    q.bindValue(":pid", playlistId);
    q.exec();
    const int pos = q.next() ? q.value(0).toInt() : 0;

    q.prepare(
        "INSERT OR IGNORE INTO PlaylistItems (playlist_id, track_id, position)"
        " VALUES (:pid, :tid, :pos)");
    q.bindValue(":pid", playlistId);
    q.bindValue(":tid", trackId);
    q.bindValue(":pos", pos);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] addTrackToPlaylist:" << q.lastError().text();
        return false;
    }

    if (q.numRowsAffected() > 0) {
        emit playlistsChanged();
        scheduleBackupSync();
    }
    return true;
}

bool LibraryDatabase::removeTrackFromPlaylist(const QString& playlistId, const QString& trackId)
{
    if (!m_db.isOpen() || playlistId.isEmpty() || trackId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("DELETE FROM PlaylistItems WHERE playlist_id = :pid AND track_id = :tid");
    q.bindValue(":pid", playlistId);
    q.bindValue(":tid", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] removeTrackFromPlaylist:" << q.lastError().text();
        return false;
    }

    if (q.numRowsAffected() > 0) {
        emit playlistsChanged();
        scheduleBackupSync();
    }
    return true;
}

bool LibraryDatabase::setPlaylistTrackPosition(const QString& playlistId, const QString& trackId, int newPosition)
{
    if (!m_db.isOpen() || playlistId.isEmpty() || trackId.isEmpty() || newPosition < 0)
        return false;

    QSqlQuery q(m_db);
    
    // First, get the current position to adjust other tracks
    q.prepare("SELECT position FROM PlaylistItems WHERE playlist_id = :pid AND track_id = :tid");
    q.bindValue(":pid", playlistId);
    q.bindValue(":tid", trackId);
    
    if (!q.exec() || !q.next()) {
        qWarning() << "[LibraryDatabase] setPlaylistTrackPosition (select):" << q.lastError().text();
        return false;
    }
    
    int oldPosition = q.value(0).toInt();
    if (oldPosition == newPosition)
        return true;  // No change needed
    
    // If moving down, shift others up
    if (newPosition > oldPosition) {
        q.prepare("UPDATE PlaylistItems SET position = position - 1 "
                  "WHERE playlist_id = :pid AND position > :old AND position <= :new");
        q.bindValue(":pid", playlistId);
        q.bindValue(":old", oldPosition);
        q.bindValue(":new", newPosition);
        if (!q.exec()) {
            qWarning() << "[LibraryDatabase] setPlaylistTrackPosition (shift down):" << q.lastError().text();
            return false;
        }
    }
    // If moving up, shift others down
    else {
        q.prepare("UPDATE PlaylistItems SET position = position + 1 "
                  "WHERE playlist_id = :pid AND position >= :new AND position < :old");
        q.bindValue(":pid", playlistId);
        q.bindValue(":new", newPosition);
        q.bindValue(":old", oldPosition);
        if (!q.exec()) {
            qWarning() << "[LibraryDatabase] setPlaylistTrackPosition (shift up):" << q.lastError().text();
            return false;
        }
    }
    
    // Update the track's position
    q.prepare("UPDATE PlaylistItems SET position = :pos WHERE playlist_id = :pid AND track_id = :tid");
    q.bindValue(":pos", newPosition);
    q.bindValue(":pid", playlistId);
    q.bindValue(":tid", trackId);
    
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] setPlaylistTrackPosition (update):" << q.lastError().text();
        return false;
    }
    
    emit playlistsChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getPlaylistTracks(const QString& playlistId) const
{
    QVariantList result;
    if (!m_db.isOpen() || playlistId.isEmpty())
        return result;

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path, '')"
        " FROM PlaylistItems pi"
        " JOIN Tracks t ON pi.track_id = t.id"
        " LEFT JOIN Locations l ON t.id = l.track_id"
        " WHERE pi.playlist_id = :pid"
        " ORDER BY pi.position ASC");
    q.bindValue(":pid", playlistId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] getPlaylistTracks:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m["trackId"]     = q.value(0).toString();
        m["title"]       = q.value(1).toString();
        m["artist"]      = q.value(2).toString();
        m["durationSec"] = q.value(3).toInt();
        m["bpm"]         = q.value(4).toDouble();
        m["key"]         = q.value(5).toString();
        m["bitrateKbps"] = q.value(6).toInt();
        m["isAnalyzed"]  = q.value(7).toBool();
        m["filePath"]    = q.value(8).toString();
        result.push_back(m);
    }
    return result;
}

QVariantList LibraryDatabase::getAllTrackAnalysisItems(bool includeAnalyzed) const
{
    QVariantList result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery q(m_db);
    QString sql =
        "SELECT Tracks.id, COALESCE(Locations.file_path, ''), "
        "       COALESCE(Tracks.title, ''), COALESCE(Tracks.is_analyzed, 0) "
        "FROM Tracks "
        "JOIN Locations ON Tracks.id = Locations.track_id ";
    if (!includeAnalyzed)
        sql += "WHERE COALESCE(Tracks.is_analyzed, 0) = 0 ";
    sql += "ORDER BY Tracks.title COLLATE NOCASE ASC";

    if (!q.exec(sql)) {
        qWarning() << "[LibraryDatabase] getAllTrackAnalysisItems:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m.insert("trackId", q.value(0).toString());
        m.insert("filePath", q.value(1).toString());
        m.insert("title", q.value(2).toString());
        m.insert("isAnalyzed", q.value(3).toBool());
        result.push_back(m);
    }

    return result;
}

QVariantList LibraryDatabase::getPlaylistAnalysisItems(const QString& playlistId, bool includeAnalyzed) const
{
    QVariantList result;
    if (!m_db.isOpen() || playlistId.isEmpty())
        return result;

    QSqlQuery q(m_db);
    QString sql =
        "SELECT Tracks.id, COALESCE(Locations.file_path, ''), "
        "       COALESCE(Tracks.title, ''), COALESCE(Tracks.is_analyzed, 0) "
        "FROM PlaylistItems "
        "JOIN Tracks ON PlaylistItems.track_id = Tracks.id "
        "JOIN Locations ON Tracks.id = Locations.track_id "
        "WHERE PlaylistItems.playlist_id = :pid ";
    if (!includeAnalyzed)
        sql += "AND COALESCE(Tracks.is_analyzed, 0) = 0 ";
    sql += "ORDER BY PlaylistItems.position ASC";

    q.prepare(sql);
    q.bindValue(":pid", playlistId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] getPlaylistAnalysisItems:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m.insert("trackId", q.value(0).toString());
        m.insert("filePath", q.value(1).toString());
        m.insert("title", q.value(2).toString());
        m.insert("isAnalyzed", q.value(3).toBool());
        result.push_back(m);
    }

    return result;
}

bool LibraryDatabase::isTrackInPlaylist(const QString& playlistId, const QString& trackId) const
{
    if (!m_db.isOpen() || playlistId.isEmpty() || trackId.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM PlaylistItems WHERE playlist_id = :pid AND track_id = :tid LIMIT 1");
    q.bindValue(":pid", playlistId);
    q.bindValue(":tid", trackId);
    q.exec();
    return q.next();
}

int LibraryDatabase::getPlaylistTrackCount(const QString& playlistId) const
{
    if (!m_db.isOpen() || playlistId.isEmpty())
        return 0;

    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM PlaylistItems WHERE playlist_id = :pid");
    q.bindValue(":pid", playlistId);
    q.exec();
    return q.next() ? q.value(0).toInt() : 0;
}

// ── Generic settings ──────────────────────────────────────────────────────────

QString LibraryDatabase::getSetting(const QString& key, const QString& defaultValue) const
{
    if (!m_db.isOpen() || key.isEmpty())
        return defaultValue;

    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM Meta WHERE key = :key LIMIT 1");
    q.bindValue(":key", key);
    if (!q.exec() || !q.next())
        return defaultValue;
    const QString v = q.value(0).toString();
    return v.isNull() ? defaultValue : v;
}

bool LibraryDatabase::setSetting(const QString& key, const QString& value)
{
    if (!m_db.isOpen() || key.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO Meta (key, value) VALUES (:key, :value)");
    q.bindValue(":key",   key);
    q.bindValue(":value", value);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] setSetting:" << q.lastError().text();
        return false;
    }
    return true;
}

// ── Remove track from library ─────────────────────────────────────────────────

bool LibraryDatabase::removeTrackFromLibrary(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;

    // Collect file paths before deletion so we can clean waveform cache.
    QStringList filePaths;
    {
        QSqlQuery q(m_db);
        q.prepare("SELECT file_path FROM Locations WHERE track_id = :id");
        q.bindValue(":id", trackId);
        if (q.exec()) {
            while (q.next())
                filePaths << q.value(0).toString();
        }
    }

    // Delete track — cascades to Locations, CuePoints, BeatGridMarkers, PlaylistItems.
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM Tracks WHERE id = :id");
    q.bindValue(":id", trackId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] removeTrackFromLibrary:" << q.lastError().text();
        return false;
    }

    // Delete waveform cache files for each known file path.
    constexpr int kPps = 600;
    for (const QString& fp : std::as_const(filePaths)) {
        const QString cachePath = WaveformCache::cachePathFor(fp, kPps);
        if (!cachePath.isEmpty() && QFile::exists(cachePath)) {
            if (!QFile::remove(cachePath))
                qWarning() << "[LibraryDatabase] Could not remove waveform cache:" << cachePath;
        }
    }

    scheduleTableModelRefresh();
    emit playlistsChanged();          // playlist track-counts may have changed
    emit trackRemovedFromLibrary(trackId);
    scheduleBackupSync();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

bool LibraryDatabase::isHealthyDatabaseFile(const QString& path) const
{
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || fileInfo.size() <= 0)
        return false;

    const QString connectionName = QStringLiteral("library_health_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    bool ok = false;
    {
        QSqlDatabase healthDb = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        healthDb.setDatabaseName(path);

        if (!healthDb.open()) {
            qWarning() << "[LibraryDatabase] Health check open failed for" << path << ':' << healthDb.lastError().text();
        } else {
            {
                QSqlQuery q(healthDb);
                if (!q.exec("PRAGMA integrity_check")) {
                    qWarning() << "[LibraryDatabase] integrity_check failed to run for" << path << ':' << q.lastError().text();
                } else {
                    ok = q.next() && q.value(0).toString().compare(QStringLiteral("ok"), Qt::CaseInsensitive) == 0;
                    if (!ok)
                        qWarning() << "[LibraryDatabase] integrity_check reported corruption for" << path;
                }
            }
            healthDb.close();
        }

        healthDb = QSqlDatabase();
    }

    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

bool LibraryDatabase::copyDatabaseFile(const QString& sourcePath, const QString& targetPath) const
{
    if (sourcePath.isEmpty() || targetPath.isEmpty() || sourcePath == targetPath)
        return false;

    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || sourceInfo.size() <= 0)
        return false;

    QFile::remove(targetPath);
    QFile::remove(targetPath + QStringLiteral("-wal"));
    QFile::remove(targetPath + QStringLiteral("-shm"));

    if (!QFile::copy(sourcePath, targetPath)) {
        qWarning() << "[LibraryDatabase] Failed to copy DB file from" << sourcePath << "to" << targetPath;
        return false;
    }

    return true;
}

bool LibraryDatabase::restorePrimaryFromBackup()
{
    if (!isHealthyDatabaseFile(m_backupDbPath))
        return false;

    return copyDatabaseFile(m_backupDbPath, m_dbPath);
}

bool LibraryDatabase::syncBackupFromPrimary()
{
    if (!m_db.isOpen())
        return false;

    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    const QString mirrorPath = (activePath == m_dbPath) ? m_backupDbPath : m_dbPath;

    {
        QSqlQuery q(m_db);
        if (!q.exec("PRAGMA wal_checkpoint(TRUNCATE)")) {
            qWarning() << "[LibraryDatabase] wal_checkpoint(TRUNCATE) failed:" << q.lastError().text();
            return false;
        }
    }

    return recreateDatabaseFileFromLiveConnection(mirrorPath);
}

void LibraryDatabase::clearDatabaseConnection()
{
    if (!m_db.isValid())
        return;

    const QString connectionName = m_db.connectionName();
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();

    if (!connectionName.isEmpty())
        QSqlDatabase::removeDatabase(connectionName);
}

bool LibraryDatabase::recreateDatabaseFileFromLiveConnection(const QString& targetPath)
{
    if (!m_db.isOpen() || targetPath.isEmpty())
        return false;

    QFile::remove(targetPath);
    QFile::remove(targetPath + QStringLiteral("-wal"));
    QFile::remove(targetPath + QStringLiteral("-shm"));

    const QString escapedTarget = targetPath;
    QString vacuumTarget = escapedTarget;
    vacuumTarget.replace(QLatin1Char('\''), QStringLiteral("''"));

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(vacuumTarget))) {
        qWarning() << "[LibraryDatabase] VACUUM INTO failed for" << targetPath << ':' << q.lastError().text();
        return false;
    }

    return true;
}

bool LibraryDatabase::reopenDatabaseConnection()
{
    clearDatabaseConnection();

    m_db = QSqlDatabase::addDatabase("QSQLITE", "library_conn");
    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    m_db.setDatabaseName(activePath);

    if (!m_db.open()) {
        qWarning() << "[LibraryDatabase] Failed to reopen:" << m_db.lastError().text();
        return false;
    }

    {
        QSqlQuery pragma(m_db);
        pragma.exec("PRAGMA journal_mode=WAL");
        pragma.exec("PRAGMA foreign_keys=ON");
    }

    if (!createSchema())
        return false;

    return true;
}

void LibraryDatabase::performMirrorSelfCheck()
{
    if (!m_db.isOpen())
        return;

    const bool primaryHealthy = isHealthyDatabaseFile(m_dbPath);
    const bool backupHealthy = isHealthyDatabaseFile(m_backupDbPath);
    bool repaired = false;
    bool switched = false;

    QString desiredActivePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    if (!primaryHealthy && !backupHealthy) {
        m_lastRecoveryEvent = QStringLiteral("both mirrors missing/corrupted — rebuilding from live session");
        if (m_db.isOpen()) {
            if (recreateDatabaseFileFromLiveConnection(m_dbPath)) {
                repaired = true;
                if (!recreateDatabaseFileFromLiveConnection(m_backupDbPath))
                    qWarning() << "[LibraryDatabase] Failed to recreate backup during both-files-missing recovery";
                m_lastRecoveryEvent = QStringLiteral("both mirrors restored from live session");
            } else {
                qWarning() << "[LibraryDatabase] Failed to recreate primary during both-files-missing recovery";
                m_lastRecoveryEvent = QStringLiteral("restore of both mirrors failed");
            }
        }
        m_primaryMirrorDegraded = true;
        m_backupMirrorDegraded = true;
        const QString currentStatus = mirroredDatabaseStatus();
        if (currentStatus != m_cachedMirrorStatus || repaired) {
            m_cachedMirrorStatus = currentStatus;
            emit mirroredDatabaseStatusChanged();
        }
        return;
    }

    if (desiredActivePath == m_dbPath && !primaryHealthy && backupHealthy) {
        desiredActivePath = m_backupDbPath;
        m_primaryMirrorDegraded = true;
        switched = true;
    } else if (desiredActivePath == m_backupDbPath && !backupHealthy && primaryHealthy) {
        desiredActivePath = m_dbPath;
        m_backupMirrorDegraded = true;
        switched = true;
    }

    if (desiredActivePath != m_activeDbPath) {
        const QString previousActivePath = m_activeDbPath;
        m_activeDbPath = desiredActivePath;
        const QString fromLabel = (previousActivePath == m_backupDbPath) ? QStringLiteral("B") : QStringLiteral("A");
        const QString toLabel = (m_activeDbPath == m_backupDbPath) ? QStringLiteral("B") : QStringLiteral("A");
        qWarning() << "[LibraryDatabase] Active mirror switched from" << fromLabel << "to" << toLabel;
        m_lastRecoveryEvent = QStringLiteral("Umschaltung %1 -> %2").arg(fromLabel, toLabel);
        if (!reopenDatabaseConnection())
            return;
    }

    if (!primaryHealthy && m_db.isOpen()) {
        if (recreateDatabaseFileFromLiveConnection(m_dbPath)) {
            m_primaryMirrorDegraded = true;
            repaired = true;
            m_lastRecoveryEvent = QStringLiteral("DB A restored from live session");
        } else {
            m_lastRecoveryEvent = QStringLiteral("DB A restore failed");
        }
    }

    if (!backupHealthy && m_db.isOpen()) {
        if (syncBackupFromPrimary()) {
            m_backupMirrorDegraded = true;
            repaired = true;
            m_lastRecoveryEvent = QStringLiteral("DB B restored from active mirror");
        } else {
            m_lastRecoveryEvent = QStringLiteral("DB B restore failed");
        }
    }

    const QString currentStatus = mirroredDatabaseStatus();
    if (currentStatus != m_cachedMirrorStatus || repaired || switched) {
        m_cachedMirrorStatus = currentStatus;
        emit mirroredDatabaseStatusChanged();
    }
}
