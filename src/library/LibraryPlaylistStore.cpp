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
