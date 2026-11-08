#include "LibraryDatabase.h"
#include "PlaylistFiles.h"
#include "RekordboxXml.h"
#include "TrackIdentity.h"

#include <QDebug>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <functional>

using namespace brockdj::library;

namespace {

struct ParsedRekordbox {
    bool ok = false;
    QString error;
    RekordboxXmlLibrary library;
};

constexpr double kGridSampleRate = 44100.0;

} // namespace

// Writes the Rekordbox grid as a locked grid without flagging the track as
// analysed: the batch analyser still produces waveform, key and phrases, and
// its persistence path leaves a locked grid untouched.
static bool importRekordboxGrid(QSqlDatabase& db, const QString& trackId, const RekordboxXmlTrack& track)
{
    const QList<RekordboxBeat> beats = expandRekordboxBeats(track.tempos, track.durationSec);
    if (beats.size() < 2)
        return false;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT COALESCE(is_analyzed,0), COALESCE(beatgrid_locked_by_user,0), COALESCE(beatgrid_user_modified,0)"
        " FROM Tracks WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), trackId);
    if (!q.exec() || !q.next() || q.value(0).toInt() || q.value(1).toInt() || q.value(2).toInt())
        return false;

    bool dynamic = false;
    for (const RekordboxXmlTempo& node : track.tempos)
        dynamic |= std::abs(node.bpm - track.tempos.first().bpm) > 0.01;
    const double bpm = track.bpm > 0.0 ? track.bpm : beats.first().bpm;

    q.prepare(QStringLiteral(
        "UPDATE Tracks SET bpm = :bpm, first_beat_sample = :first, analysis_sample_rate = :rate,"
        " beatgrid_type = :type, beatgrid_locked_by_user = 1, beatgrid_user_modified = 0 WHERE id = :id"));
    q.bindValue(QStringLiteral(":bpm"), bpm);
    q.bindValue(QStringLiteral(":first"), static_cast<qint64>(std::llround(beats.first().positionSec * kGridSampleRate)));
    q.bindValue(QStringLiteral(":rate"), kGridSampleRate);
    q.bindValue(QStringLiteral(":type"), dynamic ? QStringLiteral("dynamic") : QStringLiteral("constant"));
    q.bindValue(QStringLiteral(":id"), trackId);
    if (!q.exec())
        return false;

    for (const char* table : {"BeatGridMarkers", "TempoNodes"}) {
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE track_id = :id").arg(QLatin1String(table)));
        q.bindValue(QStringLiteral(":id"), trackId);
        q.exec();
    }

    q.prepare(QStringLiteral(
        "INSERT INTO BeatGridMarkers(track_id,beat_index,position_sec,is_downbeat,bar_number,beat_in_bar,"
        "confidence,user_modified,locked_by_user) VALUES (:id,:idx,:pos,:down,:bar,:inbar,1.0,0,1)"));
    int bar = 0;
    for (int i = 0; i < beats.size(); ++i) {
        if (beats[i].beatInBar == 1)
            ++bar;
        q.bindValue(QStringLiteral(":id"), trackId);
        q.bindValue(QStringLiteral(":idx"), i);
        q.bindValue(QStringLiteral(":pos"), beats[i].positionSec);
        q.bindValue(QStringLiteral(":down"), beats[i].beatInBar == 1 ? 1 : 0);
        q.bindValue(QStringLiteral(":bar"), bar);
        q.bindValue(QStringLiteral(":inbar"), beats[i].beatInBar);
        q.exec();
    }

    q.prepare(QStringLiteral(
        "INSERT INTO TempoNodes(track_id,node_index,position_sec,bpm,confidence) VALUES (:id,:idx,:pos,:bpm,1.0)"));
    for (int i = 0; i < track.tempos.size(); ++i) {
        q.bindValue(QStringLiteral(":id"), trackId);
        q.bindValue(QStringLiteral(":idx"), i);
        q.bindValue(QStringLiteral(":pos"), track.tempos[i].positionSec);
        q.bindValue(QStringLiteral(":bpm"), track.tempos[i].bpm);
        q.exec();
    }
    return true;
}

bool LibraryDatabase::importRekordboxXml(const QString& fileUrlOrPath)
{
    const QString path = localPathFromUrlOrPath(fileUrlOrPath);
    if (path.isEmpty() || !beginHealthTask(QStringLiteral("rekordbox")))
        return false;

    auto* watcher = new QFutureWatcher<ParsedRekordbox>(this);
    connect(watcher, &QFutureWatcher<ParsedRekordbox>::finished, this, [this, watcher] {
        const ParsedRekordbox parsed = watcher->result();
        watcher->deleteLater();
        if (!parsed.ok || !m_db.isOpen()) {
            finishHealthTask({{QStringLiteral("ok"), false},
                              {QStringLiteral("error"), parsed.error.isEmpty()
                                   ? QStringLiteral("Bibliothek nicht verfügbar") : parsed.error}});
            return;
        }
        finishHealthTask(applyRekordboxLibrary(parsed.library));
    });
    watcher->setFuture(QtConcurrent::run([path] {
        ParsedRekordbox parsed;
        parsed.ok = readRekordboxXml(path, parsed.library, &parsed.error);
        return parsed;
    }));
    return true;
}

QVariantMap LibraryDatabase::applyRekordboxLibrary(const RekordboxXmlLibrary& library)
{
    int added = 0, updated = 0, cues = 0, loops = 0, grids = 0, playlists = 0, missingFiles = 0;
    QHash<QString, QString> idByKey;    // Rekordbox TrackID / path -> BrockDJ track id

    m_db.transaction();
    for (const RekordboxXmlTrack& track : library.tracks) {
        if (track.path.isEmpty() || track.path.contains(QStringLiteral("://")))
            continue;
        if (!QFileInfo(track.path).isFile())
            ++missingFiles;

        QString trackId = trackIdForFilePath(track.path);
        if (trackId.isEmpty()) {
            const QString title = track.title.isEmpty() ? QFileInfo(track.path).completeBaseName() : track.title;
            trackId = generateTrackId(track.artist, title, track.durationSec, track.path);
            if (!addTrack(trackId, title, track.artist, track.durationSec, track.path, track.bitrateKbps,
                          track.genre, track.album, track.comment, track.dateAdded))
                continue;
            ++added;
        } else {
            ++updated;
        }
        idByKey.insert(track.key, trackId);
        idByKey.insert(track.path, trackId);

        // Only fill what BrockDJ does not know yet; never override user edits.
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "UPDATE Tracks SET"
            " rating = CASE WHEN COALESCE(rating,0) = 0 THEN :rating ELSE rating END,"
            " color = CASE WHEN COALESCE(color,'') = '' THEN :color ELSE color END,"
            " play_count = MAX(COALESCE(play_count,0), :plays),"
            " genre = CASE WHEN COALESCE(genre,'') = '' THEN :genre ELSE genre END,"
            " album = CASE WHEN COALESCE(album,'') = '' THEN :album ELSE album END,"
            " comment = CASE WHEN COALESCE(comment,'') = '' THEN :comment ELSE comment END,"
            " key = CASE WHEN COALESCE(key,'') = '' THEN :key ELSE key END,"
            " bpm = CASE WHEN COALESCE(bpm,0) <= 0 THEN :bpm ELSE bpm END"
            " WHERE id = :id"));
        q.bindValue(QStringLiteral(":rating"), track.rating);
        q.bindValue(QStringLiteral(":color"), track.color);
        q.bindValue(QStringLiteral(":plays"), track.playCount);
        q.bindValue(QStringLiteral(":genre"), track.genre);
        q.bindValue(QStringLiteral(":album"), track.album);
        q.bindValue(QStringLiteral(":comment"), track.comment);
        q.bindValue(QStringLiteral(":key"), track.musicalKey);
        q.bindValue(QStringLiteral(":bpm"), track.bpm);
        q.bindValue(QStringLiteral(":id"), trackId);
        if (!q.exec())
            qWarning() << "[LibraryDatabase] Rekordbox meta:" << q.lastError().text();

        QSet<int> usedCues;
        for (const QVariant& cue : cuePointsForTrack(trackId))
            usedCues.insert(cue.toMap().value(QStringLiteral("index")).toInt());
        QSet<int> usedLoops;
        for (const QVariant& loop : savedLoopsForTrack(trackId))
            usedLoops.insert(loop.toMap().value(QStringLiteral("index")).toInt());
        const auto freeSlot = [](QSet<int>& used, int preferred) {
            int slot = preferred >= 0 && !used.contains(preferred) ? preferred : -1;
            for (int i = 0; slot < 0 && i < 8; ++i)
                if (!used.contains(i))
                    slot = i;
            if (slot >= 0)
                used.insert(slot);
            return slot;
        };

        bool mainCueSet = mainCuePointForTrack(trackId) >= 0.0;
        for (const RekordboxXmlMark& mark : track.marks) {
            const QString color = mark.color.isEmpty() ? QStringLiteral("#2ECC71") : mark.color;
            if (mark.loop) {
                const int slot = freeSlot(usedLoops, mark.hotCueIndex);
                if (slot >= 0 && upsertSavedLoop(trackId, slot, mark.startSec, mark.endSec, mark.name, color))
                    ++loops;
            } else if (mark.hotCueIndex >= 0) {
                if (!usedCues.contains(mark.hotCueIndex)) {
                    usedCues.insert(mark.hotCueIndex);
                    if (upsertCuePoint(trackId, mark.hotCueIndex, mark.startSec, mark.name, color))
                        ++cues;
                }
            } else if (!mainCueSet) {
                mainCueSet = upsertMainCuePoint(trackId, mark.startSec);
            }
        }
        if (importRekordboxGrid(m_db, trackId, track))
            ++grids;
        emit trackMetaChanged(trackId);
    }

    // Playlists land below one "Rekordbox" folder so a re-import is easy to spot.
    if (!library.playlists.isEmpty()) {
        const QString root = createPlaylist(QStringLiteral("Rekordbox"), QString());
        std::function<void(const RekordboxXmlNode&, const QString&)> addNode;
        addNode = [&](const RekordboxXmlNode& node, const QString& parentId) {
            const QString id = createPlaylist(node.name.isEmpty() ? QStringLiteral("Playlist") : node.name, parentId);
            if (id.isEmpty())
                return;
            ++playlists;
            for (const RekordboxXmlNode& child : node.children)
                addNode(child, id);
            for (const QString& key : node.trackKeys)
                if (const QString trackId = idByKey.value(key); !trackId.isEmpty())
                    addTrackToPlaylist(id, trackId);
        };
        if (!root.isEmpty())
            for (const RekordboxXmlNode& node : library.playlists)
                addNode(node, root);
    }
    m_db.commit();

    if (missingFiles > 0)
        QMetaObject::invokeMethod(this, [this] { scanMissingFiles(); }, Qt::QueuedConnection);
    emit playlistsChanged();

    return {{QStringLiteral("ok"), true},
            {QStringLiteral("tracks"), library.tracks.size()},
            {QStringLiteral("added"), added},
            {QStringLiteral("updated"), updated},
            {QStringLiteral("cues"), cues},
            {QStringLiteral("loops"), loops},
            {QStringLiteral("grids"), grids},
            {QStringLiteral("playlists"), playlists},
            {QStringLiteral("missingFiles"), missingFiles}};
}
