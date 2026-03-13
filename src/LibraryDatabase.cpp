#include "LibraryDatabase.h"
#include "LibraryTableModel.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
}

LibraryDatabase::~LibraryDatabase()
{
    if (m_db.isOpen())
        m_db.close();
}

bool LibraryDatabase::open()
{
    // ── Determine the database directory ─────────────────────────────────
    // Use QStandardPaths::AppConfigLocation which resolves to:
    //   Linux:   ~/.config/<AppName>
    //   macOS:   ~/Library/Preferences/<AppName>
    //   Windows: C:/Users/<USER>/AppData/Local/<AppName>
    // This matches the SettingsManager path (which also lands in ~/.config/RamsbrockDJ/).
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDir.isEmpty())
        configDir = QDir::homePath() + "/.config";

    QDir dbDir(configDir + "/db");
    if (!dbDir.exists())
        dbDir.mkpath(".");

    m_dbPath = dbDir.filePath("RamsbrockDJ_Library.db");

    qDebug() << "[LibraryDatabase] DB path:" << m_dbPath;

    // ── Open via QSqlDatabase ────────────────────────────────────────────
    m_db = QSqlDatabase::addDatabase("QSQLITE", "library_conn");
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "[LibraryDatabase] Failed to open:" << m_db.lastError().text();
        return false;
    }

    // Enable WAL mode for better concurrency and foreign keys.
    QSqlQuery pragma(m_db);
    pragma.exec("PRAGMA journal_mode=WAL");
    pragma.exec("PRAGMA foreign_keys=ON");

    return createSchema();
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

    // ── Stamp current version ────────────────────────────────────────────
    q.prepare("INSERT OR REPLACE INTO Meta (key, value) VALUES ('schema_version', :v)");
    q.bindValue(":v", kSchemaVersion);
    q.exec();

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
    qDebug() << "[LibraryDatabase] addTrack:" << trackId.left(12) << title << artist;
    QSqlQuery q(m_db);

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
        return false;
    }

    // Keep bitrate up to date if a previously known track is reloaded with new metadata.
    q.prepare("UPDATE Tracks SET bitrate_kbps = CASE WHEN :kbps > 0 THEN :kbps ELSE bitrate_kbps END "
              "WHERE id = :id");
    q.bindValue(":kbps", bitrateKbps);
    q.bindValue(":id", trackId);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] addTrack bitrate update:" << q.lastError().text();
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

    if (m_tableModel)
        m_tableModel->refresh();

    emit trackAdded(trackId);
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

    if (m_tableModel)
        m_tableModel->refresh();

    emit analysisUpdated(trackId);
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
    q.prepare("UPDATE Tracks SET track_segments = :segments WHERE id = :id");
    q.bindValue(":segments", trackSegmentsToJson(segments));
    q.bindValue(":id", trackId);

    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] updateTrackSegments:" << q.lastError().text();
        return false;
    }

    if (m_tableModel)
        m_tableModel->refresh();

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

QString LibraryDatabase::filePath(const QString& trackId) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT file_path FROM Locations WHERE track_id = :id LIMIT 1");
    q.bindValue(":id", trackId);
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}

void LibraryDatabase::setTableModel(LibraryTableModel* model)
{
    m_tableModel = model;
}
