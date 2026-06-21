#include "LibraryDatabase.h"

#include <QDebug>
#include <QSqlQuery>
#include <QSqlError>

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

} // namespace

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
