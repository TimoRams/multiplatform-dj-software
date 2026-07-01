#include "LibraryDatabase.h"
#include "AnalysisCacheVersion.h"
#include "LibraryTableModel.h"
#include "rendering/WaveformCache.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>

namespace {

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

QVariantList LibraryDatabase::getDistinctArtists() const
{
    QVariantList result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery q(m_db);
    if (!q.exec(
            "SELECT COALESCE(NULLIF(TRIM(t.artist), ''), '(Unknown)') AS name, COUNT(*) AS cnt"
            " FROM Tracks t JOIN Locations l ON t.id = l.track_id"
            " GROUP BY LOWER(name)"
            " ORDER BY LOWER(name) COLLATE NOCASE ASC")) {
        qWarning() << "[LibraryDatabase] getDistinctArtists:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m.insert("name", q.value(0).toString());
        m.insert("trackCount", q.value(1).toInt());
        result.push_back(m);
    }
    return result;
}

QVariantList LibraryDatabase::getDistinctAlbums() const
{
    QVariantList result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery q(m_db);
    if (!q.exec(
            "SELECT COALESCE(NULLIF(TRIM(t.album), ''), '(Unknown)') AS name,"
            "       COALESCE(NULLIF(TRIM(t.artist), ''), '') AS artist,"
            "       COUNT(*) AS cnt"
            " FROM Tracks t JOIN Locations l ON t.id = l.track_id"
            " GROUP BY LOWER(name), LOWER(artist)"
            " ORDER BY LOWER(name) COLLATE NOCASE ASC")) {
        qWarning() << "[LibraryDatabase] getDistinctAlbums:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m.insert("name", q.value(0).toString());
        m.insert("artist", q.value(1).toString());
        m.insert("trackCount", q.value(2).toInt());
        result.push_back(m);
    }
    return result;
}

QVariantList LibraryDatabase::getDistinctKeys() const
{
    QVariantList result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery q(m_db);
    if (!q.exec(
            "SELECT COALESCE(NULLIF(TRIM(t.key), ''), '?') AS name, COUNT(*) AS cnt"
            " FROM Tracks t JOIN Locations l ON t.id = l.track_id"
            " WHERE t.key IS NOT NULL AND TRIM(t.key) != ''"
            " GROUP BY LOWER(name)"
            " ORDER BY name COLLATE NOCASE ASC")) {
        qWarning() << "[LibraryDatabase] getDistinctKeys:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QVariantMap m;
        m.insert("name", q.value(0).toString());
        m.insert("trackCount", q.value(1).toInt());
        result.push_back(m);
    }
    return result;
}

QVariantList LibraryDatabase::getLibrarySourceRoots() const
{
    QVariantList result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery q(m_db);
    if (!q.exec("SELECT l.file_path FROM Locations l")) {
        qWarning() << "[LibraryDatabase] getLibrarySourceRoots:" << q.lastError().text();
        return result;
    }

    QHash<QString, int> counts;
    while (q.next()) {
        const QFileInfo fi(q.value(0).toString());
        const QString dir = fi.absolutePath();
        if (!dir.isEmpty())
            counts[dir]++;
    }

    QStringList dirs = counts.keys();
    std::ranges::sort(dirs, [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    for (const QString& dir : dirs) {
        const QFileInfo fi(dir);
        QVariantMap m;
        m.insert("path", dir);
        m.insert("label", fi.fileName().isEmpty() ? dir : fi.fileName());
        m.insert("trackCount", counts.value(dir));
        result.push_back(m);
    }
    return result;
}
