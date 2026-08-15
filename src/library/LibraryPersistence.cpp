#include "LibraryDatabase.h"
#include "analysis/AnalysisTypes.h"
#include "LibraryTableModel.h"
#include "waveform/WaveformCache.h"

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
    q.bindValue(":analysisVersion", analysis::kCurrentAnalysisVersion);
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

#include "LibraryDatabase.h"

#include <QDebug>
#include <QSqlQuery>
#include <QSqlError>

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

#include "LibraryDatabase.h"

#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>

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
