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
