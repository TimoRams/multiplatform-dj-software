#include "LibraryDatabase.h"
#include "PlaylistFiles.h"
#include "TrackIdentity.h"

#include <QDebug>
#include <QFileInfo>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>

using namespace brockdj::library;

namespace {

struct ImportedMetadata {
    QString title;
    QString artist;
    QString album;
    QString genre;
    int durationSec = 0;
    int bitrateKbps = 0;
};

QString fromTagString(const TagLib::String& value)
{
    return QString::fromStdWString(value.toWString()).trimmed();
}

ImportedMetadata readMetadata(const PlaylistFileEntry& entry)
{
    ImportedMetadata meta;
#ifdef Q_OS_WIN
    TagLib::FileRef file(reinterpret_cast<const wchar_t*>(entry.path.utf16()));
#else
    TagLib::FileRef file(entry.path.toUtf8().constData());
#endif
    if (!file.isNull()) {
        if (const TagLib::Tag* tag = file.tag()) {
            meta.title = fromTagString(tag->title());
            meta.artist = fromTagString(tag->artist());
            meta.album = fromTagString(tag->album());
            meta.genre = fromTagString(tag->genre());
        }
        if (const TagLib::AudioProperties* audio = file.audioProperties()) {
            meta.durationSec = audio->lengthInSeconds();
            meta.bitrateKbps = audio->bitrate();
        }
    }
    // Playlist info fills what the tags leave empty.
    if (meta.title.isEmpty())
        meta.title = entry.title.isEmpty() ? QFileInfo(entry.path).completeBaseName() : entry.title;
    if (meta.artist.isEmpty())
        meta.artist = entry.artist;
    if (meta.durationSec <= 0 && entry.durationSec > 0)
        meta.durationSec = entry.durationSec;
    return meta;
}

} // namespace

QString LibraryDatabase::exportPlaylistToFile(const QString& playlistId, const QString& fileUrlOrPath)
{
    if (!m_db.isOpen() || playlistId.isEmpty())
        return QStringLiteral("Bibliothek nicht verfügbar");
    QString path = localPathFromUrlOrPath(fileUrlOrPath);
    if (path.isEmpty())
        return QStringLiteral("Kein Dateiname");
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".m3u8");

    QList<PlaylistFileEntry> entries;
    for (const QVariant& value : getPlaylistTracks(playlistId)) {
        const QVariantMap track = value.toMap();
        PlaylistFileEntry entry;
        entry.path = track.value(QStringLiteral("filePath")).toString();
        if (entry.path.isEmpty())
            continue;
        entry.title = track.value(QStringLiteral("title")).toString();
        entry.artist = track.value(QStringLiteral("artist")).toString();
        entry.durationSec = track.value(QStringLiteral("durationSec")).toInt();
        entries.append(entry);
    }

    QString error;
    if (!writePlaylistFile(path, entries, &error)) {
        qWarning() << "[LibraryDatabase] exportPlaylistToFile:" << path << error;
        return error.isEmpty() ? QStringLiteral("Schreiben fehlgeschlagen") : error;
    }
    return {};
}

QVariantMap LibraryDatabase::importPlaylistFile(const QString& fileUrlOrPath, const QString& parentId)
{
    QVariantMap result{{QStringLiteral("ok"), false}};
    const auto fail = [&result](const QString& error) {
        result.insert(QStringLiteral("error"), error);
        return result;
    };
    if (!m_db.isOpen())
        return fail(QStringLiteral("Bibliothek nicht verfügbar"));

    const QString path = localPathFromUrlOrPath(fileUrlOrPath);
    QList<PlaylistFileEntry> entries;
    QString error;
    if (!readPlaylistFile(path, entries, &error))
        return fail(error);

    const QString name = QFileInfo(path).completeBaseName();
    const QString playlistId = createPlaylist(name.isEmpty() ? QStringLiteral("Import") : name, parentId);
    if (playlistId.isEmpty())
        return fail(QStringLiteral("Playlist konnte nicht angelegt werden"));

    int imported = 0;
    int added = 0;
    QStringList missing;
    for (const PlaylistFileEntry& entry : std::as_const(entries)) {
        const QFileInfo file(entry.path);
        QString trackId = trackIdForFilePath(entry.path);
        if (trackId.isEmpty()) {
            if (!file.isFile()) {
                missing.append(entry.path);
                continue;
            }
            const ImportedMetadata meta = readMetadata(entry);
            trackId = generateTrackId(meta.artist, meta.title, meta.durationSec, entry.path);
            if (!addTrack(trackId, meta.title, meta.artist, meta.durationSec, entry.path,
                          meta.bitrateKbps, meta.genre, meta.album)) {
                missing.append(entry.path);
                continue;
            }
            ++added;
        }
        if (addTrackToPlaylist(playlistId, trackId))
            ++imported;
    }

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("playlistId"), playlistId);
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("imported"), imported);
    result.insert(QStringLiteral("added"), added);
    result.insert(QStringLiteral("missing"), missing);
    return result;
}
