#include "LibraryDatabase.h"
#include "AnalysisCacheVersion.h"
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
#include <QDateTime>
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

QString gridTypeToString(TrackData::BeatGridType type)
{
    switch (type) {
    case TrackData::BeatGridType::DynamicTempo:
        return QStringLiteral("dynamic");
    case TrackData::BeatGridType::ConstantTempo:
        return QStringLiteral("constant");
    case TrackData::BeatGridType::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

TrackData::BeatGridType gridTypeFromString(const QString& value)
{
    if (value.compare(QStringLiteral("dynamic"), Qt::CaseInsensitive) == 0)
        return TrackData::BeatGridType::DynamicTempo;
    if (value.compare(QStringLiteral("constant"), Qt::CaseInsensitive) == 0)
        return TrackData::BeatGridType::ConstantTempo;
    return TrackData::BeatGridType::Unknown;
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
        obj.insert("confidence", s.confidence);
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
        m.insert("confidence", o.value("confidence").toDouble());
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

    const QString legacyDbPath   = dbDir.filePath("RamsbrockDJ_Library.db");
    const QString legacyPrimary  = dbDir.filePath("RamsbrockDJ_Library_A.db");
    const QString legacyBackup   = dbDir.filePath("RamsbrockDJ_Library_B.db");
    m_dbPath = dbDir.filePath("BrockDJ_Library_A.db");
    m_backupDbPath = dbDir.filePath("BrockDJ_Library_B.db");
    m_activeDbPath = m_dbPath;
    m_manualBackupDbPath = dbDir.filePath("BrockDJ_Library_backup_manual.db");

    // One-time migration: copy old RamsbrockDJ databases into new BrockDJ paths
    if (!QFile::exists(m_dbPath) && !QFile::exists(m_backupDbPath)) {
        if (isHealthyDatabaseFile(legacyPrimary)) {
            qWarning() << "[LibraryDatabase] Migrating RamsbrockDJ → BrockDJ (primary)";
            copyDatabaseFile(legacyPrimary, m_dbPath);
            if (isHealthyDatabaseFile(legacyBackup))
                copyDatabaseFile(legacyBackup, m_backupDbPath);
        } else if (isHealthyDatabaseFile(legacyBackup)) {
            qWarning() << "[LibraryDatabase] Migrating RamsbrockDJ → BrockDJ (backup only)";
            copyDatabaseFile(legacyBackup, m_dbPath);
        }
    }

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

    if (currentVersion < 9) {
        // Version 9: add per-track user metadata columns.
        const QStringList cols = {
            "ALTER TABLE Tracks ADD COLUMN genre       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN album       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN comment     TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN rating      INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN energy      INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN color       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN notes       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN play_count  INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN last_played INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN date_added  INTEGER DEFAULT 0",
        };
        for (const auto& sql : cols) {
            const QString col = sql.section(' ', 5, 5);
            if (!tableHasColumn(m_db, "Tracks", col))
                q.exec(sql);
        }
        // Indices for fast sorting / filtering on new columns.
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_rating ON Tracks(rating)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_energy ON Tracks(energy)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_genre  ON Tracks(genre COLLATE NOCASE)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_bpm    ON Tracks(bpm)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_date   ON Tracks(date_added)");
    }

    if (currentVersion < 10) {
        // Version 10: tag system.
        q.exec(
            "CREATE TABLE IF NOT EXISTS Tags ("
            "  id    TEXT PRIMARY KEY,"
            "  name  TEXT NOT NULL,"
            "  color TEXT DEFAULT '#888888'"
            ")");
        q.exec(
            "CREATE TABLE IF NOT EXISTS TrackTags ("
            "  track_id TEXT NOT NULL,"
            "  tag_id   TEXT NOT NULL,"
            "  PRIMARY KEY(track_id, tag_id),"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE,"
            "  FOREIGN KEY(tag_id)   REFERENCES Tags(id)   ON DELETE CASCADE"
            ")");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracktags_tag ON TrackTags(tag_id)");
    }

    if (currentVersion < 11) {
        // Version 11: play history.
        q.exec(
            "CREATE TABLE IF NOT EXISTS PlayHistory ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  track_id   TEXT NOT NULL,"
            "  played_at  INTEGER NOT NULL,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        q.exec("CREATE INDEX IF NOT EXISTS idx_history_track   ON PlayHistory(track_id)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_history_time    ON PlayHistory(played_at DESC)");
    }

    if (currentVersion < 12) {
        // Version 12: favorites.
        q.exec(
            "CREATE TABLE IF NOT EXISTS Favorites ("
            "  track_id TEXT PRIMARY KEY,"
            "  added_at INTEGER NOT NULL DEFAULT 0,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
    }

    if (currentVersion < 13) {
        // Version 13: prepare crate.
        q.exec(
            "CREATE TABLE IF NOT EXISTS PrepareCrate ("
            "  track_id TEXT PRIMARY KEY,"
            "  position INTEGER NOT NULL DEFAULT 0,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
    }

    if (currentVersion < 14) {
        // Version 14: track queue + smart collections.
        q.exec(
            "CREATE TABLE IF NOT EXISTS TrackQueue ("
            "  track_id TEXT PRIMARY KEY,"
            "  position INTEGER NOT NULL DEFAULT 0,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        q.exec(
            "CREATE TABLE IF NOT EXISTS SmartCollections ("
            "  id         TEXT PRIMARY KEY,"
            "  name       TEXT NOT NULL,"
            "  rules_json TEXT NOT NULL DEFAULT '[]',"
            "  sort_order INTEGER DEFAULT 0"
            ")");
    }

    if (currentVersion < 15) {
        struct ColumnAdd {
            const char* table;
            const char* column;
            const char* sql;
        };
        const ColumnAdd adds[] = {
            {"Tracks", "analysis_version", "ALTER TABLE Tracks ADD COLUMN analysis_version INTEGER DEFAULT 0"},
            {"Tracks", "bpm_confidence", "ALTER TABLE Tracks ADD COLUMN bpm_confidence REAL DEFAULT 0.0"},
            {"Tracks", "beat_confidence", "ALTER TABLE Tracks ADD COLUMN beat_confidence REAL DEFAULT 0.0"},
            {"Tracks", "downbeat_confidence", "ALTER TABLE Tracks ADD COLUMN downbeat_confidence REAL DEFAULT 0.0"},
            {"Tracks", "grid_confidence", "ALTER TABLE Tracks ADD COLUMN grid_confidence REAL DEFAULT 0.0"},
            {"Tracks", "beatgrid_type", "ALTER TABLE Tracks ADD COLUMN beatgrid_type TEXT DEFAULT 'constant'"},
            {"Tracks", "beatgrid_user_modified", "ALTER TABLE Tracks ADD COLUMN beatgrid_user_modified INTEGER DEFAULT 0"},
            {"Tracks", "beatgrid_locked_by_user", "ALTER TABLE Tracks ADD COLUMN beatgrid_locked_by_user INTEGER DEFAULT 0"},
            {"BeatGridMarkers", "beat_in_bar", "ALTER TABLE BeatGridMarkers ADD COLUMN beat_in_bar INTEGER DEFAULT 1"},
            {"BeatGridMarkers", "confidence", "ALTER TABLE BeatGridMarkers ADD COLUMN confidence REAL DEFAULT 0.0"},
            {"BeatGridMarkers", "user_modified", "ALTER TABLE BeatGridMarkers ADD COLUMN user_modified INTEGER DEFAULT 0"},
            {"BeatGridMarkers", "locked_by_user", "ALTER TABLE BeatGridMarkers ADD COLUMN locked_by_user INTEGER DEFAULT 0"},
        };
        for (const auto& add : adds) {
            if (!tableHasColumn(m_db, add.table, add.column) && !q.exec(add.sql))
                qWarning() << "[LibraryDatabase] schema v15 column" << add.column << q.lastError().text();
        }

        q.exec(
            "CREATE TABLE IF NOT EXISTS TempoNodes ("
            "  track_id     TEXT NOT NULL,"
            "  node_index   INTEGER NOT NULL,"
            "  position_sec REAL NOT NULL,"
            "  bpm          REAL NOT NULL,"
            "  confidence   REAL DEFAULT 0.0,"
            "  PRIMARY KEY(track_id, node_index),"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
    }

    if (currentVersion < 16) {
        if (!q.exec(
                "CREATE TABLE IF NOT EXISTS SavedLoops ("
                "  track_id   TEXT NOT NULL,"
                "  loop_index INTEGER NOT NULL,"
                "  in_sec     REAL NOT NULL,"
                "  out_sec    REAL NOT NULL,"
                "  label      TEXT DEFAULT '',"
                "  color      TEXT DEFAULT '#30b050',"
                "  PRIMARY KEY(track_id, loop_index),"
                "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
                ")")) {
            qWarning() << "[LibraryDatabase] SavedLoops:" << q.lastError().text();
        }
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
                               int bitrateKbps,
                               const QString& genre,
                               const QString& album,
                               const QString& comment,
                               qint64 dateAdded)
{
    if (!m_db.isOpen()) {
        qWarning() << "[LibraryDatabase] addTrack: database not open";
        return false;
    }

    const qint64 now = dateAdded > 0
        ? dateAdded
        : static_cast<qint64>(QDateTime::currentSecsSinceEpoch());

    qDebug() << "[LibraryDatabase] addTrack:" << trackId.left(12) << title << artist;
    QSqlQuery q(m_db);
    bool trackInserted = false;
    bool locationInserted = false;

    // INSERT OR IGNORE: don't overwrite existing metadata if re-added.
    q.prepare("INSERT OR IGNORE INTO Tracks"
              " (id, title, artist, duration_sec, bitrate_kbps, genre, album, comment, date_added)"
              " VALUES (:id, :title, :artist, :dur, :kbps, :genre, :album, :comment, :dateAdded)");
    q.bindValue(":id",        trackId);
    q.bindValue(":title",     title);
    q.bindValue(":artist",    artist);
    q.bindValue(":dur",       durationSec);
    q.bindValue(":kbps",      bitrateKbps);
    q.bindValue(":genre",     genre);
    q.bindValue(":album",     album);
    q.bindValue(":comment",   comment);
    q.bindValue(":dateAdded", now);

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
              "  bitrate_kbps = CASE WHEN :kbps   > 0   THEN :kbps   ELSE bitrate_kbps END,"
              "  title   = CASE WHEN :title  <> '' THEN :title  ELSE title  END,"
              "  artist  = CASE WHEN :artist <> '' THEN :artist ELSE artist END,"
              "  genre   = CASE WHEN :genre  <> '' THEN :genre  ELSE genre  END,"
              "  album   = CASE WHEN :album  <> '' THEN :album  ELSE album  END,"
              "  comment = CASE WHEN :comment<> '' THEN :comment ELSE comment END"
              " WHERE id = :id");
    q.bindValue(":kbps",    bitrateKbps);
    q.bindValue(":title",   title);
    q.bindValue(":artist",  artist);
    q.bindValue(":genre",   genre);
    q.bindValue(":album",   album);
    q.bindValue(":comment", comment);
    q.bindValue(":id",      trackId);
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
                                         const std::vector<TrackData::BeatMarker>& beatGrid,
                                         TrackData::ConfidenceInfo confidence,
                                         TrackData::BeatGridInfo beatGridInfo)
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

    bool lockedGridInDbBeforeUpdate = false;
    {
        QSqlQuery lockQuery(m_db);
        lockQuery.prepare("SELECT COALESCE(beatgrid_locked_by_user, 0) FROM Tracks WHERE id = :id LIMIT 1");
        lockQuery.bindValue(":id", trackId);
        if (lockQuery.exec() && lockQuery.next())
            lockedGridInDbBeforeUpdate = lockQuery.value(0).toInt() != 0;
    }

    QSqlQuery q(m_db);
    q.prepare(
        "UPDATE Tracks SET "
        "  bpm = CASE WHEN :bpm > 0 THEN :bpm ELSE bpm END,"
        "  key = CASE WHEN length(trim(:key)) > 0 THEN :key ELSE key END,"
        "  is_analyzed = CASE WHEN (:bpm > 0 OR length(trim(:key)) > 0) THEN 1 ELSE is_analyzed END,"
        "  first_beat_sample = CASE WHEN :firstBeatSample >= 0 THEN :firstBeatSample ELSE first_beat_sample END,"
        "  analysis_sample_rate = CASE WHEN :sampleRate > 0 THEN :sampleRate ELSE analysis_sample_rate END,"
        "  analysis_version = :analysisVersion,"
        "  bpm_confidence = :bpmConfidence,"
        "  beat_confidence = :beatConfidence,"
        "  downbeat_confidence = :downbeatConfidence,"
        "  grid_confidence = :gridConfidence,"
        "  beatgrid_type = CASE WHEN length(trim(:gridType)) > 0 THEN :gridType ELSE beatgrid_type END,"
        "  beatgrid_user_modified = :gridUserModified,"
        "  beatgrid_locked_by_user = CASE WHEN beatgrid_locked_by_user != 0 THEN beatgrid_locked_by_user ELSE :gridLocked END"
        " WHERE id = :id");
    q.bindValue(":bpm", static_cast<double>(newBpm));
    q.bindValue(":key", newKey);
    q.bindValue(":firstBeatSample", firstBeatSample);
    q.bindValue(":sampleRate", sampleRate);
    q.bindValue(":analysisVersion", analysis::kAnalysisVersion);
    q.bindValue(":bpmConfidence", confidence.bpmConfidence);
    q.bindValue(":beatConfidence", confidence.beatConfidence);
    q.bindValue(":downbeatConfidence", confidence.downbeatConfidence);
    q.bindValue(":gridConfidence", confidence.gridConfidence);
    q.bindValue(":gridType", gridTypeToString(beatGridInfo.type));
    q.bindValue(":gridUserModified", beatGridInfo.userModified ? 1 : 0);
    q.bindValue(":gridLocked", beatGridInfo.lockedByUser ? 1 : 0);
    q.bindValue(":id",  trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] updateAnalysisData:" << q.lastError().text();
        m_db.rollback();
        return;
    }

    const bool incomingManualGrid = beatGridInfo.userModified || beatGridInfo.lockedByUser;
    const bool preserveExistingLockedGrid = lockedGridInDbBeforeUpdate && !incomingManualGrid;

    if (!beatGrid.empty() && !preserveExistingLockedGrid) {
        q.prepare("DELETE FROM BeatGridMarkers WHERE track_id = :id");
        q.bindValue(":id", trackId);
        if (!q.exec()) {
            qWarning() << "[LibraryDatabase] clear BeatGridMarkers:" << q.lastError().text();
            m_db.rollback();
            return;
        }

        q.prepare(
            "INSERT INTO BeatGridMarkers (track_id, beat_index, position_sec, is_downbeat, bar_number, "
            "beat_in_bar, confidence, user_modified, locked_by_user) "
            "VALUES (:trackId, :beatIndex, :positionSec, :isDownbeat, :barNumber, "
            ":beatInBar, :confidence, :userModified, :lockedByUser)");

        for (int beatIndex = 0; beatIndex < static_cast<int>(beatGrid.size()); ++beatIndex) {
            const auto& marker = beatGrid[static_cast<size_t>(beatIndex)];
            q.bindValue(":trackId", trackId);
            q.bindValue(":beatIndex", beatIndex);
            q.bindValue(":positionSec", marker.positionSec);
            q.bindValue(":isDownbeat", marker.isDownbeat ? 1 : 0);
            q.bindValue(":barNumber", marker.barNumber);
            q.bindValue(":beatInBar", marker.beatInBar);
            q.bindValue(":confidence", marker.confidence);
            q.bindValue(":userModified", marker.userModified ? 1 : 0);
            q.bindValue(":lockedByUser", marker.lockedByUser ? 1 : 0);

            if (!q.exec()) {
                qWarning() << "[LibraryDatabase] insert BeatGridMarkers:" << q.lastError().text();
                m_db.rollback();
                return;
            }
        }

        q.prepare("DELETE FROM TempoNodes WHERE track_id = :id");
        q.bindValue(":id", trackId);
        if (!q.exec()) {
            qWarning() << "[LibraryDatabase] clear TempoNodes:" << q.lastError().text();
            m_db.rollback();
            return;
        }

        q.prepare(
            "INSERT INTO TempoNodes (track_id, node_index, position_sec, bpm, confidence) "
            "VALUES (:trackId, :nodeIndex, :positionSec, :bpm, :confidence)");
        for (int nodeIndex = 0; nodeIndex < static_cast<int>(beatGridInfo.tempoNodes.size()); ++nodeIndex) {
            const auto& node = beatGridInfo.tempoNodes[static_cast<size_t>(nodeIndex)];
            q.bindValue(":trackId", trackId);
            q.bindValue(":nodeIndex", nodeIndex);
            q.bindValue(":positionSec", node.positionSec);
            q.bindValue(":bpm", node.bpm);
            q.bindValue(":confidence", node.confidence);
            if (!q.exec()) {
                qWarning() << "[LibraryDatabase] insert TempoNodes:" << q.lastError().text();
                m_db.rollback();
                return;
            }
        }
    } else if (!beatGrid.empty() && preserveExistingLockedGrid) {
        qDebug() << "[LibraryDatabase] Beatgrid locked by user; preserving existing markers for"
                 << trackId.left(12);
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
        "       COALESCE(analysis_sample_rate, 44100.0), "
        "       COALESCE(analysis_version, 0), "
        "       COALESCE(bpm_confidence, 0.0), "
        "       COALESCE(beat_confidence, 0.0), "
        "       COALESCE(downbeat_confidence, 0.0), "
        "       COALESCE(grid_confidence, 0.0), "
        "       COALESCE(beatgrid_type, 'unknown'), "
        "       COALESCE(beatgrid_user_modified, 0), "
        "       COALESCE(beatgrid_locked_by_user, 0) "
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
    snapshot.analysisVersion = q.value(5).toInt();
    snapshot.confidence.bpmConfidence = q.value(6).toFloat();
    snapshot.confidence.beatConfidence = q.value(7).toFloat();
    snapshot.confidence.downbeatConfidence = q.value(8).toFloat();
    snapshot.confidence.gridConfidence = q.value(9).toFloat();
    snapshot.beatGridInfo.type = gridTypeFromString(q.value(10).toString());
    snapshot.beatGridInfo.userModified = q.value(11).toInt() != 0;
    snapshot.beatGridInfo.lockedByUser = q.value(12).toInt() != 0;

    QSqlQuery beatsQuery(m_db);
    beatsQuery.prepare(
        "SELECT position_sec, is_downbeat, bar_number, "
        "       COALESCE(beat_in_bar, 1), COALESCE(confidence, 0.0), "
        "       COALESCE(user_modified, 0), COALESCE(locked_by_user, 0) "
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
        marker.barIndex = marker.barNumber - 1;
        marker.beatInBar = beatsQuery.value(3).toInt();
        marker.confidence = beatsQuery.value(4).toFloat();
        marker.userModified = beatsQuery.value(5).toInt() != 0;
        marker.lockedByUser = beatsQuery.value(6).toInt() != 0;
        snapshot.beatGrid.push_back(marker);
    }

    QSqlQuery nodeQuery(m_db);
    nodeQuery.prepare(
        "SELECT position_sec, bpm, COALESCE(confidence, 0.0) "
        "FROM TempoNodes WHERE track_id = :id ORDER BY node_index ASC");
    nodeQuery.bindValue(":id", trackId);
    if (nodeQuery.exec()) {
        while (nodeQuery.next()) {
            TrackData::TempoNode node;
            node.positionSec = nodeQuery.value(0).toDouble();
            node.bpm = nodeQuery.value(1).toDouble();
            node.confidence = nodeQuery.value(2).toFloat();
            snapshot.beatGridInfo.tempoNodes.push_back(node);
        }
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

bool LibraryDatabase::upsertSavedLoop(const QString& trackId,
                                      int loopIndex,
                                      double inSec,
                                      double outSec,
                                      const QString& label,
                                      const QString& colorHex)
{
    if (trackId.isEmpty() || loopIndex < 0 || loopIndex >= 8 || outSec <= inSec + 0.001)
        return false;

    QSqlQuery q(m_db);
    q.prepare(
        "INSERT OR REPLACE INTO SavedLoops (track_id, loop_index, in_sec, out_sec, label, color) "
        "VALUES (:trackId, :loopIndex, :inSec, :outSec, :label, :color)");
    q.bindValue(":trackId", trackId);
    q.bindValue(":loopIndex", loopIndex);
    q.bindValue(":inSec", inSec);
    q.bindValue(":outSec", outSec);
    q.bindValue(":label", label);
    q.bindValue(":color", colorHex);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] upsertSavedLoop:" << q.lastError().text();
        return false;
    }

    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::deleteSavedLoop(const QString& trackId, int loopIndex)
{
    if (trackId.isEmpty() || loopIndex < 0 || loopIndex >= 8)
        return false;

    QSqlQuery q(m_db);
    q.prepare("DELETE FROM SavedLoops WHERE track_id = :trackId AND loop_index = :loopIndex");
    q.bindValue(":trackId", trackId);
    q.bindValue(":loopIndex", loopIndex);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] deleteSavedLoop:" << q.lastError().text();
        return false;
    }

    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::savedLoopsForTrack(const QString& trackId) const
{
    if (trackId.isEmpty())
        return {};

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT loop_index, in_sec, out_sec, COALESCE(label, ''), COALESCE(color, '#30b050') "
        "FROM SavedLoops WHERE track_id = :trackId ORDER BY loop_index ASC");
    q.bindValue(":trackId", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] savedLoopsForTrack:" << q.lastError().text();
        return {};
    }

    QVariantList loops;
    while (q.next()) {
        QVariantMap entry;
        entry.insert("index", q.value(0).toInt());
        entry.insert("inSec", q.value(1).toDouble());
        entry.insert("outSec", q.value(2).toDouble());
        entry.insert("label", q.value(3).toString());
        entry.insert("color", q.value(4).toString());
        loops.push_back(entry);
    }

    return loops;
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
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
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

    while (q.next())
        result << buildTrackMap(q);
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

// ── buildTrackMap ─────────────────────────────────────────────────────────────
// Columns 0-18 expected by all full-track SELECT queries:
//  0 id  1 title  2 artist  3 duration_sec  4 bpm  5 key
//  6 bitrate_kbps  7 is_analyzed  8 file_path
//  9 genre  10 album  11 comment  12 rating  13 energy  14 color
//  15 notes  16 play_count  17 last_played  18 date_added
QVariantMap LibraryDatabase::buildTrackMap(const QSqlQuery& q) const
{
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
    m["genre"]       = q.value(9).toString();
    m["album"]       = q.value(10).toString();
    m["comment"]     = q.value(11).toString();
    m["rating"]      = q.value(12).toInt();
    m["energy"]      = q.value(13).toInt();
    m["color"]       = q.value(14).toString();
    m["notes"]       = q.value(15).toString();
    m["playCount"]   = q.value(16).toInt();
    m["lastPlayed"]  = q.value(17).toLongLong();
    m["dateAdded"]   = q.value(18).toLongLong();
    return m;
}

// ── Per-track user metadata ────────────────────────────────────────────────────

bool LibraryDatabase::setTrackRating(const QString& trackId, int rating)
{
    if (!m_db.isOpen() || trackId.isEmpty() || rating < 0 || rating > 5)
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE Tracks SET rating = :r WHERE id = :id");
    q.bindValue(":r",  rating);
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setTrackRating:" << q.lastError().text(); return false; }
    emit trackMetaChanged(trackId);
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::setTrackEnergy(const QString& trackId, int energy)
{
    if (!m_db.isOpen() || trackId.isEmpty() || energy < 0 || energy > 5)
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE Tracks SET energy = :e WHERE id = :id");
    q.bindValue(":e",  energy);
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setTrackEnergy:" << q.lastError().text(); return false; }
    emit trackMetaChanged(trackId);
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::setTrackColor(const QString& trackId, const QString& colorHex)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE Tracks SET color = :c WHERE id = :id");
    q.bindValue(":c",  colorHex);
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setTrackColor:" << q.lastError().text(); return false; }
    emit trackMetaChanged(trackId);
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::setTrackNotes(const QString& trackId, const QString& notes)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE Tracks SET notes = :n WHERE id = :id");
    q.bindValue(":n",  notes);
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setTrackNotes:" << q.lastError().text(); return false; }
    emit trackMetaChanged(trackId);
    scheduleBackupSync();
    return true;
}

QVariantMap LibraryDatabase::getTrackMeta(const QString& trackId) const
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return {};
    QSqlQuery q(m_db);
    q.prepare(
        "SELECT COALESCE(rating,0), COALESCE(energy,0), COALESCE(color,''),"
        "       COALESCE(notes,''), COALESCE(genre,''), COALESCE(album,''),"
        "       COALESCE(comment,''), COALESCE(play_count,0),"
        "       COALESCE(last_played,0), COALESCE(date_added,0)"
        " FROM Tracks WHERE id = :id LIMIT 1");
    q.bindValue(":id", trackId);
    if (!q.exec() || !q.next())
        return {};
    QVariantMap m;
    m["rating"]     = q.value(0).toInt();
    m["energy"]     = q.value(1).toInt();
    m["color"]      = q.value(2).toString();
    m["notes"]      = q.value(3).toString();
    m["genre"]      = q.value(4).toString();
    m["album"]      = q.value(5).toString();
    m["comment"]    = q.value(6).toString();
    m["playCount"]  = q.value(7).toInt();
    m["lastPlayed"] = q.value(8).toLongLong();
    m["dateAdded"]  = q.value(9).toLongLong();
    return m;
}

// ── Tag system ────────────────────────────────────────────────────────────────

QString LibraryDatabase::createTag(const QString& name, const QString& colorHex)
{
    if (!m_db.isOpen() || name.trimmed().isEmpty())
        return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO Tags (id, name, color) VALUES (:id, :name, :color)");
    q.bindValue(":id",    id);
    q.bindValue(":name",  name.trimmed());
    q.bindValue(":color", colorHex.isEmpty() ? QStringLiteral("#888888") : colorHex);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] createTag:" << q.lastError().text(); return {}; }
    emit tagsChanged();
    scheduleBackupSync();
    return id;
}

bool LibraryDatabase::deleteTag(const QString& tagId)
{
    if (!m_db.isOpen() || tagId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM Tags WHERE id = :id");
    q.bindValue(":id", tagId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] deleteTag:" << q.lastError().text(); return false; }
    emit tagsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::renameTag(const QString& tagId, const QString& newName)
{
    if (!m_db.isOpen() || tagId.isEmpty() || newName.trimmed().isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE Tags SET name = :name WHERE id = :id");
    q.bindValue(":name", newName.trimmed());
    q.bindValue(":id",   tagId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] renameTag:" << q.lastError().text(); return false; }
    emit tagsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::setTagColor(const QString& tagId, const QString& colorHex)
{
    if (!m_db.isOpen() || tagId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE Tags SET color = :color WHERE id = :id");
    q.bindValue(":color", colorHex);
    q.bindValue(":id",    tagId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setTagColor:" << q.lastError().text(); return false; }
    emit tagsChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getAllTags() const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    if (!q.exec("SELECT id, name, color FROM Tags ORDER BY name COLLATE NOCASE ASC")) {
        qWarning() << "[LibraryDatabase] getAllTags:" << q.lastError().text();
        return {};
    }
    QVariantList result;
    while (q.next()) {
        QVariantMap m;
        m["id"]    = q.value(0).toString();
        m["name"]  = q.value(1).toString();
        m["color"] = q.value(2).toString();
        result << m;
    }
    return result;
}

bool LibraryDatabase::addTagToTrack(const QString& trackId, const QString& tagId)
{
    if (!m_db.isOpen() || trackId.isEmpty() || tagId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT OR IGNORE INTO TrackTags (track_id, tag_id) VALUES (:tid, :gid)");
    q.bindValue(":tid", trackId);
    q.bindValue(":gid", tagId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] addTagToTrack:" << q.lastError().text(); return false; }
    emit tagsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::removeTagFromTrack(const QString& trackId, const QString& tagId)
{
    if (!m_db.isOpen() || trackId.isEmpty() || tagId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM TrackTags WHERE track_id = :tid AND tag_id = :gid");
    q.bindValue(":tid", trackId);
    q.bindValue(":gid", tagId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] removeTagFromTrack:" << q.lastError().text(); return false; }
    emit tagsChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getTagsForTrack(const QString& trackId) const
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return {};
    QSqlQuery q(m_db);
    q.prepare(
        "SELECT tg.id, tg.name, tg.color"
        " FROM TrackTags tt JOIN Tags tg ON tt.tag_id = tg.id"
        " WHERE tt.track_id = :tid"
        " ORDER BY tg.name COLLATE NOCASE ASC");
    q.bindValue(":tid", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] getTagsForTrack:" << q.lastError().text(); return {}; }
    QVariantList result;
    while (q.next()) {
        QVariantMap m;
        m["id"]    = q.value(0).toString();
        m["name"]  = q.value(1).toString();
        m["color"] = q.value(2).toString();
        result << m;
    }
    return result;
}

QVariantList LibraryDatabase::getTracksForTag(const QString& tagId) const
{
    if (!m_db.isOpen() || tagId.isEmpty())
        return {};
    QSqlQuery q(m_db);
    q.prepare(
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
        " FROM TrackTags tt"
        " JOIN Tracks t ON tt.track_id = t.id"
        " LEFT JOIN Locations l ON t.id = l.track_id"
        " WHERE tt.tag_id = :gid"
        " ORDER BY t.title COLLATE NOCASE ASC");
    q.bindValue(":gid", tagId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] getTracksForTag:" << q.lastError().text(); return {}; }
    QVariantList result;
    while (q.next())
        result << buildTrackMap(q);
    return result;
}

bool LibraryDatabase::isTagOnTrack(const QString& trackId, const QString& tagId) const
{
    if (!m_db.isOpen() || trackId.isEmpty() || tagId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM TrackTags WHERE track_id = :tid AND tag_id = :gid LIMIT 1");
    q.bindValue(":tid", trackId);
    q.bindValue(":gid", tagId);
    q.exec();
    return q.next();
}

// ── Favorites ─────────────────────────────────────────────────────────────────

bool LibraryDatabase::addToFavorites(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT OR IGNORE INTO Favorites (track_id, added_at) VALUES (:id, :at)");
    q.bindValue(":id", trackId);
    q.bindValue(":at", QDateTime::currentSecsSinceEpoch());
    if (!q.exec()) { qWarning() << "[LibraryDatabase] addToFavorites:" << q.lastError().text(); return false; }
    emit favoritesChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::removeFromFavorites(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM Favorites WHERE track_id = :id");
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] removeFromFavorites:" << q.lastError().text(); return false; }
    emit favoritesChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::isFavorite(const QString& trackId) const
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM Favorites WHERE track_id = :id LIMIT 1");
    q.bindValue(":id", trackId);
    q.exec();
    return q.next();
}

QVariantList LibraryDatabase::getFavoriteTracks() const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    if (!q.exec(
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
        " FROM Favorites f"
        " JOIN Tracks t ON f.track_id = t.id"
        " LEFT JOIN Locations l ON t.id = l.track_id"
        " ORDER BY f.added_at DESC")) {
        qWarning() << "[LibraryDatabase] getFavoriteTracks:" << q.lastError().text();
        return {};
    }
    QVariantList result;
    while (q.next())
        result << buildTrackMap(q);
    return result;
}

// ── Prepare Crate ─────────────────────────────────────────────────────────────

bool LibraryDatabase::addToPrepareCrate(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.exec("SELECT COALESCE(MAX(position), -1) + 1 FROM PrepareCrate");
    const int pos = q.next() ? q.value(0).toInt() : 0;
    q.prepare("INSERT OR IGNORE INTO PrepareCrate (track_id, position) VALUES (:id, :pos)");
    q.bindValue(":id",  trackId);
    q.bindValue(":pos", pos);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] addToPrepareCrate:" << q.lastError().text(); return false; }
    emit crateChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::removeFromPrepareCrate(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM PrepareCrate WHERE track_id = :id");
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] removeFromPrepareCrate:" << q.lastError().text(); return false; }
    emit crateChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::clearPrepareCrate()
{
    if (!m_db.isOpen())
        return false;
    QSqlQuery q(m_db);
    if (!q.exec("DELETE FROM PrepareCrate")) {
        qWarning() << "[LibraryDatabase] clearPrepareCrate:" << q.lastError().text();
        return false;
    }
    emit crateChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getPrepareCrateTracks() const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    if (!q.exec(
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
        " FROM PrepareCrate pc"
        " JOIN Tracks t ON pc.track_id = t.id"
        " LEFT JOIN Locations l ON t.id = l.track_id"
        " ORDER BY pc.position ASC")) {
        qWarning() << "[LibraryDatabase] getPrepareCrateTracks:" << q.lastError().text();
        return {};
    }
    QVariantList result;
    while (q.next())
        result << buildTrackMap(q);
    return result;
}

bool LibraryDatabase::savePrepareCrateAsPlaylist(const QString& name)
{
    if (!m_db.isOpen() || name.trimmed().isEmpty())
        return false;
    const QString playlistId = createPlaylist(name.trimmed());
    if (playlistId.isEmpty())
        return false;
    QStringList trackIds;
    {
        QSqlQuery q(m_db);
        if (!q.exec("SELECT track_id FROM PrepareCrate ORDER BY position ASC")) {
            qWarning() << "[LibraryDatabase] savePrepareCrateAsPlaylist:" << q.lastError().text();
            return false;
        }
        while (q.next())
            trackIds << q.value(0).toString();
    }
    for (const QString& tid : std::as_const(trackIds))
        addTrackToPlaylist(playlistId, tid);
    return true;
}

bool LibraryDatabase::setPrepareCratePosition(const QString& trackId, int position)
{
    if (!m_db.isOpen() || trackId.isEmpty() || position < 0)
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE PrepareCrate SET position = :pos WHERE track_id = :id");
    q.bindValue(":pos", position);
    q.bindValue(":id",  trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setPrepareCratePosition:" << q.lastError().text(); return false; }
    emit crateChanged();
    scheduleBackupSync();
    return true;
}

// ── Track Queue ───────────────────────────────────────────────────────────────

bool LibraryDatabase::enqueueTrack(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.exec("SELECT COALESCE(MAX(position), -1) + 1 FROM TrackQueue");
    const int pos = q.next() ? q.value(0).toInt() : 0;
    q.prepare("INSERT OR IGNORE INTO TrackQueue (track_id, position) VALUES (:id, :pos)");
    q.bindValue(":id",  trackId);
    q.bindValue(":pos", pos);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] enqueueTrack:" << q.lastError().text(); return false; }
    emit queueChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::dequeueTrack(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM TrackQueue WHERE track_id = :id");
    q.bindValue(":id", trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] dequeueTrack:" << q.lastError().text(); return false; }
    emit queueChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::clearQueue()
{
    if (!m_db.isOpen())
        return false;
    QSqlQuery q(m_db);
    if (!q.exec("DELETE FROM TrackQueue")) {
        qWarning() << "[LibraryDatabase] clearQueue:" << q.lastError().text();
        return false;
    }
    emit queueChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getQueueTracks() const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    if (!q.exec(
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
        " FROM TrackQueue tq"
        " JOIN Tracks t ON tq.track_id = t.id"
        " LEFT JOIN Locations l ON t.id = l.track_id"
        " ORDER BY tq.position ASC")) {
        qWarning() << "[LibraryDatabase] getQueueTracks:" << q.lastError().text();
        return {};
    }
    QVariantList result;
    while (q.next())
        result << buildTrackMap(q);
    return result;
}

bool LibraryDatabase::setQueuePosition(const QString& trackId, int position)
{
    if (!m_db.isOpen() || trackId.isEmpty() || position < 0)
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE TrackQueue SET position = :pos WHERE track_id = :id");
    q.bindValue(":pos", position);
    q.bindValue(":id",  trackId);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] setQueuePosition:" << q.lastError().text(); return false; }
    emit queueChanged();
    scheduleBackupSync();
    return true;
}

// ── Play History ──────────────────────────────────────────────────────────────

bool LibraryDatabase::logPlay(const QString& trackId)
{
    if (!m_db.isOpen() || trackId.isEmpty())
        return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!m_db.transaction())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO PlayHistory (track_id, played_at) VALUES (:id, :at)");
    q.bindValue(":id", trackId);
    q.bindValue(":at", now);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] logPlay (insert):" << q.lastError().text();
        m_db.rollback();
        return false;
    }
    q.prepare("UPDATE Tracks SET play_count = play_count + 1, last_played = :at WHERE id = :id");
    q.bindValue(":at", now);
    q.bindValue(":id", trackId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] logPlay (update):" << q.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) { m_db.rollback(); return false; }
    emit historyChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getPlayHistory(const QString& period) const
{
    if (!m_db.isOpen())
        return {};

    qint64 since = 0;
    if (period == QStringLiteral("today")) {
        since = QDateTime(QDate::currentDate(), QTime(0, 0, 0)).toSecsSinceEpoch();
    } else if (period == QStringLiteral("week")) {
        since = QDateTime::currentSecsSinceEpoch() - 7LL * 86400;
    } else if (period == QStringLiteral("month")) {
        since = QDateTime::currentSecsSinceEpoch() - 30LL * 86400;
    }

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       ph.played_at, COALESCE(t.date_added,0), ph.id"
        " FROM PlayHistory ph"
        " JOIN Tracks t ON ph.track_id = t.id"
        " LEFT JOIN Locations l ON t.id = l.track_id"
        " WHERE ph.played_at >= :since"
        " ORDER BY ph.played_at DESC, ph.id DESC");
    q.bindValue(":since", since);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] getPlayHistory:" << q.lastError().text();
        return {};
    }
    QVariantList result;
    int eventIndex = 0;
    while (q.next()) {
        QVariantMap map = buildTrackMap(q);
        const qint64 playedAt = q.value(17).toLongLong();
        map[QStringLiteral("historyId")] = q.value(19).toLongLong();
        map[QStringLiteral("playedAt")] = playedAt;
        map[QStringLiteral("playEventIndex")] = ++eventIndex;
        map[QStringLiteral("playCountAtTrack")] = q.value(16).toInt();
        result << map;
    }
    return result;
}

// ── Smart Collections ─────────────────────────────────────────────────────────

QString LibraryDatabase::createSmartCollection(const QString& name, const QString& rulesJson)
{
    if (!m_db.isOpen() || name.trimmed().isEmpty())
        return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery q(m_db);
    q.exec("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM SmartCollections");
    const int so = q.next() ? q.value(0).toInt() : 0;
    q.prepare("INSERT INTO SmartCollections (id, name, rules_json, sort_order) VALUES (:id, :name, :rules, :so)");
    q.bindValue(":id",    id);
    q.bindValue(":name",  name.trimmed());
    q.bindValue(":rules", rulesJson.isEmpty() ? QStringLiteral("[]") : rulesJson);
    q.bindValue(":so",    so);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] createSmartCollection:" << q.lastError().text(); return {}; }
    emit smartCollectionsChanged();
    scheduleBackupSync();
    return id;
}

bool LibraryDatabase::deleteSmartCollection(const QString& id)
{
    if (!m_db.isOpen() || id.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM SmartCollections WHERE id = :id");
    q.bindValue(":id", id);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] deleteSmartCollection:" << q.lastError().text(); return false; }
    emit smartCollectionsChanged();
    scheduleBackupSync();
    return true;
}

bool LibraryDatabase::updateSmartCollection(const QString& id, const QString& name, const QString& rulesJson)
{
    if (!m_db.isOpen() || id.isEmpty() || name.trimmed().isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE SmartCollections SET name = :name, rules_json = :rules WHERE id = :id");
    q.bindValue(":name",  name.trimmed());
    q.bindValue(":rules", rulesJson.isEmpty() ? QStringLiteral("[]") : rulesJson);
    q.bindValue(":id",    id);
    if (!q.exec()) { qWarning() << "[LibraryDatabase] updateSmartCollection:" << q.lastError().text(); return false; }
    emit smartCollectionsChanged();
    scheduleBackupSync();
    return true;
}

QVariantList LibraryDatabase::getAllSmartCollections() const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    if (!q.exec("SELECT id, name, rules_json FROM SmartCollections ORDER BY sort_order ASC, name COLLATE NOCASE ASC")) {
        qWarning() << "[LibraryDatabase] getAllSmartCollections:" << q.lastError().text();
        return {};
    }
    QVariantList result;
    while (q.next()) {
        QVariantMap m;
        m["id"]        = q.value(0).toString();
        m["name"]      = q.value(1).toString();
        m["rulesJson"] = q.value(2).toString();
        result << m;
    }
    return result;
}

QVariantList LibraryDatabase::evaluateSmartCollection(const QString& rulesJson) const
{
    if (!m_db.isOpen())
        return {};

    static const QHash<QString, QString> fieldMap {
        {QStringLiteral("bpm"),        QStringLiteral("t.bpm")},
        {QStringLiteral("key"),        QStringLiteral("t.key")},
        {QStringLiteral("genre"),      QStringLiteral("t.genre")},
        {QStringLiteral("rating"),     QStringLiteral("t.rating")},
        {QStringLiteral("energy"),     QStringLiteral("t.energy")},
        {QStringLiteral("playCount"),  QStringLiteral("t.play_count")},
        {QStringLiteral("dateAdded"),  QStringLiteral("t.date_added")},
        {QStringLiteral("title"),      QStringLiteral("t.title")},
        {QStringLiteral("artist"),     QStringLiteral("t.artist")},
        {QStringLiteral("album"),      QStringLiteral("t.album")},
        {QStringLiteral("isAnalyzed"), QStringLiteral("t.is_analyzed")},
    };

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(rulesJson.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isArray())
        return {};

    QStringList whereClauses;
    QVariantList bindValues;

    for (const QJsonValue& rv : doc.array()) {
        if (!rv.isObject()) continue;
        const QJsonObject rule = rv.toObject();
        const QString field = rule[QStringLiteral("field")].toString();
        const QString op    = rule[QStringLiteral("op")].toString();
        const QJsonValue  val = rule[QStringLiteral("value")];

        if (!fieldMap.contains(field)) continue;
        const QString col = fieldMap[field];

        if (op == QStringLiteral("eq")) {
            whereClauses << QStringLiteral("%1 = ?").arg(col);
            bindValues << val.toVariant();
        } else if (op == QStringLiteral("ne")) {
            whereClauses << QStringLiteral("%1 != ?").arg(col);
            bindValues << val.toVariant();
        } else if (op == QStringLiteral("gt")) {
            whereClauses << QStringLiteral("%1 > ?").arg(col);
            bindValues << val.toVariant();
        } else if (op == QStringLiteral("lt")) {
            whereClauses << QStringLiteral("%1 < ?").arg(col);
            bindValues << val.toVariant();
        } else if (op == QStringLiteral("gte")) {
            whereClauses << QStringLiteral("%1 >= ?").arg(col);
            bindValues << val.toVariant();
        } else if (op == QStringLiteral("lte")) {
            whereClauses << QStringLiteral("%1 <= ?").arg(col);
            bindValues << val.toVariant();
        } else if (op == QStringLiteral("contains")) {
            whereClauses << QStringLiteral("%1 LIKE ?").arg(col);
            bindValues << (QStringLiteral("%") + val.toString() + QStringLiteral("%"));
        } else if (op == QStringLiteral("not_contains")) {
            whereClauses << QStringLiteral("%1 NOT LIKE ?").arg(col);
            bindValues << (QStringLiteral("%") + val.toString() + QStringLiteral("%"));
        } else if (op == QStringLiteral("is_empty")) {
            whereClauses << QStringLiteral("(COALESCE(%1,'') = '')").arg(col);
        } else if (op == QStringLiteral("is_not_empty")) {
            whereClauses << QStringLiteral("(COALESCE(%1,'') != '')").arg(col);
        }
    }

    QString sql =
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
        " FROM Tracks t"
        " LEFT JOIN Locations l ON t.id = l.track_id";
    if (!whereClauses.isEmpty())
        sql += QStringLiteral(" WHERE ") + whereClauses.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY t.title COLLATE NOCASE ASC");

    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const QVariant& v : std::as_const(bindValues))
        q.addBindValue(v);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] evaluateSmartCollection:" << q.lastError().text();
        return {};
    }

    QVariantList result;
    while (q.next())
        result << buildTrackMap(q);
    return result;
}
