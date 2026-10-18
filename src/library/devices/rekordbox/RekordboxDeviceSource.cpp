#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUrl>

#include <limits>
#include <memory>

#ifdef BROCKDJ_HAVE_SQLCIPHER
#include <sqlite3.h>
#endif

namespace rekordbox {
namespace {

#ifdef BROCKDJ_HAVE_SQLCIPHER

// The device database of the modern export format (the one the exporting
// software calls OneLibrary; older releases and parts of this codebase still
// say Device Library Plus).
const QString& identityDatabaseRelativePath()
{
    static const QString path = QStringLiteral("PIONEER/rekordbox/exportLibrary.db");
    return path;
}

// The device database is encrypted with SQLCipher using a fixed key that is
// neither licence- nor machine-dependent — every device written by the
// exporting software uses the same one. It is public knowledge and is only
// needed to read the exported library and device identity.
// Applied as a pragma rather than through sqlite3_key(), because the latter is
// only declared when SQLCipher is built with the codec macro exposed.
const char* const kCipherKeyPragma =
    "PRAGMA key = 'r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls';";

// Guard against a device whose database claims to be something else. The
// identity is only trusted when the table exists with exactly the two columns
// we read, so an unrelated file that happens to sit at this path is ignored
// rather than misinterpreted.
constexpr const char* kIdentityQuery =
    "SELECT deviceName, backGroundColorType FROM property LIMIT 1";

// A file: URI that opens the database strictly for reading. `mode=ro` refuses
// any write, and `immutable` additionally promises SQLite the file will not
// change underneath it, which suppresses the -wal and -shm sidecar files it
// would otherwise want to create next to the database on the device.
QString readOnlyUri(const QString& path)
{
    const QByteArray escaped = QUrl::toPercentEncoding(path, QByteArrayLiteral("/"));
    return QStringLiteral("file:") + QString::fromLatin1(escaped)
         + QStringLiteral("?mode=ro&immutable=1");
}

bool applyKey(sqlite3* db)
{
    if (sqlite3_exec(db, kCipherKeyPragma, nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    // Belt and braces: even a mode=ro handle is told explicitly that no
    // statement may modify the database.
    return sqlite3_exec(db, "PRAGMA query_only = 1; PRAGMA temp_store = MEMORY;",
                        nullptr, nullptr, nullptr) == SQLITE_OK;
}

PdbReader::Result readModernIndex(const QString& path)
{
    PdbReader::Result result;
    // Immutable SQLite deliberately ignores recovery files. Never show a
    // potentially stale/inconsistent snapshot from an unfinished export.
    for (const auto& suffix : {QStringLiteral("-wal"), QStringLiteral("-journal")}) {
        if (QFileInfo(path + suffix).size() > 0) {
            result.error = QStringLiteral(
                "OneLibrary has pending database recovery data; finish exporting and safely eject first");
            return result;
        }
    }
    sqlite3* rawDb = nullptr;
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
    const int opened = sqlite3_open_v2(readOnlyUri(path).toUtf8().constData(),
                                     &rawDb, flags, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    if (opened != SQLITE_OK || !applyKey(db.get())) {
        result.error = QStringLiteral("Cannot open OneLibrary exportLibrary.db read-only");
        return result;
    }

    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    auto prepare = [&](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK) {
            result.error = QStringLiteral("Unsupported or unreadable OneLibrary schema: %1")
                               .arg(QString::fromUtf8(sqlite3_errmsg(db.get())));
        }
        return Statement(statement, sqlite3_finalize);
    };
    auto text = [](sqlite3_stmt* row, int column) {
        const auto* value = sqlite3_column_text(row, column);
        return value ? QString::fromUtf8(reinterpret_cast<const char*>(value),
                                         sqlite3_column_bytes(row, column)) : QString{};
    };
    auto id = [&](sqlite3_stmt* row, int column, quint32& value, bool allowZero = false) {
        const sqlite3_int64 number = sqlite3_column_int64(row, column);
        if ((sqlite3_column_type(row, column) != SQLITE_INTEGER
             && !(allowZero && sqlite3_column_type(row, column) == SQLITE_NULL))
            || number < (allowZero ? 0 : 1)
            || number > std::numeric_limits<quint32>::max()) {
            result.error = QStringLiteral("Invalid OneLibrary identifier or sequence number");
            return false;
        }
        value = static_cast<quint32>(number);
        return true;
    };
    auto completed = [&](int status) {
        if (status == SQLITE_DONE)
            return true;
        result.error = QStringLiteral("Error reading OneLibrary: %1")
                           .arg(QString::fromUtf8(sqlite3_errmsg(db.get())));
        return false;
    };

    // Map the documented OneLibrary schema explicitly, never master.db or a
    // guessed set of similarly named columns. Additional columns are harmless.
    auto tracks = prepare(
        "SELECT c.content_id,c.title,a.name,b.name,g.name,k.name,c.djComment,"
        "c.bpmx100,c.length,c.bitrate,c.rating,col.name,c.path,"
        "c.analysisDataFilePath,i.path FROM content c "
        "LEFT JOIN artist a ON a.artist_id=c.artist_id_artist "
        "LEFT JOIN album b ON b.album_id=c.album_id "
        "LEFT JOIN genre g ON g.genre_id=c.genre_id "
        "LEFT JOIN \"key\" k ON k.key_id=c.key_id "
        "LEFT JOIN color col ON col.color_id=c.color_id "
        "LEFT JOIN image i ON i.image_id=c.image_id ORDER BY c.content_id");
    auto playlists = prepare(
        "SELECT playlist_id,playlist_id_parent,sequenceNo,attribute,name "
        "FROM playlist ORDER BY sequenceNo,playlist_id");
    auto entries = prepare(
        "SELECT playlist_id,content_id,sequenceNo FROM playlist_content "
        "ORDER BY playlist_id,sequenceNo,content_id");
    if (!tracks || !playlists || !entries)
        return result;

    constexpr qsizetype maxRows = 1'000'000;
    QSet<quint32> trackIds;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(tracks.get())) == SQLITE_ROW) {
        Track track;
        if (!id(tracks.get(), 0, track.id))
            return result;
        if (trackIds.contains(track.id) || result.tracks.size() >= maxRows) {
            result.error = QStringLiteral("Duplicate tracks or excessive OneLibrary index size");
            return result;
        }
        trackIds.insert(track.id);
        track.title = text(tracks.get(), 1);
        track.artist = text(tracks.get(), 2);
        track.album = text(tracks.get(), 3);
        track.genre = text(tracks.get(), 4);
        track.key = text(tracks.get(), 5);
        track.comment = text(tracks.get(), 6);
        track.bpm = sqlite3_column_double(tracks.get(), 7) / 100.0;
        track.durationSec = sqlite3_column_double(tracks.get(), 8) / 1000.0;
        track.bitrateKbps = sqlite3_column_int(tracks.get(), 9);
        track.rating = sqlite3_column_int(tracks.get(), 10);
        track.color = text(tracks.get(), 11);
        track.relativeAudioPath = text(tracks.get(), 12);
        track.relativeAnalysisPath = text(tracks.get(), 13);
        track.relativeArtworkPath = text(tracks.get(), 14);
        if (track.relativeAudioPath.isEmpty()) {
            result.error = QStringLiteral("OneLibrary track %1 has no audio path").arg(track.id);
            return result;
        }
        result.tracks.append(std::move(track));
    }
    if (!completed(status))
        return result;

    QHash<quint32, qsizetype> playlistById;
    while ((status = sqlite3_step(playlists.get())) == SQLITE_ROW) {
        Playlist playlist;
        quint32 attribute = 0;
        if (!id(playlists.get(), 0, playlist.id)
            || !id(playlists.get(), 1, playlist.parentId, true)
            || !id(playlists.get(), 2, playlist.sortOrder, true)
            || !id(playlists.get(), 3, attribute, true))
            return result;
        // Smart playlists have their exported membership materialized too.
        if (attribute != 0 && attribute != 1 && attribute != 4) {
            result.error = QStringLiteral("Unsupported OneLibrary playlist attribute %1")
                               .arg(attribute);
            return result;
        }
        if (playlistById.contains(playlist.id) || result.playlists.size() >= maxRows) {
            result.error = QStringLiteral("Duplicate playlists or excessive OneLibrary index size");
            return result;
        }
        playlist.folder = attribute == 1;
        playlist.name = text(playlists.get(), 4);
        playlistById.insert(playlist.id, result.playlists.size());
        result.playlists.append(std::move(playlist));
    }
    if (!completed(status))
        return result;
    qsizetype entryCount = 0;
    while ((status = sqlite3_step(entries.get())) == SQLITE_ROW) {
        quint32 playlistId = 0, trackId = 0, sequence = 0;
        if (!id(entries.get(), 0, playlistId) || !id(entries.get(), 1, trackId)
            || !id(entries.get(), 2, sequence, true))
            return result;
        if (!playlistById.contains(playlistId) || !trackIds.contains(trackId)
            || result.playlists[playlistById.value(playlistId)].folder
            || ++entryCount > maxRows) {
            result.error = QStringLiteral("Invalid OneLibrary playlist membership");
            return result;
        }
        result.playlists[playlistById.value(playlistId)].trackIds.append(trackId);
    }
    if (!completed(status))
        return result;

    QHash<quint32, QVector<qsizetype>> children;
    for (qsizetype n = 0; n < result.playlists.size(); ++n) {
        const auto& playlist = result.playlists[n];
        if (playlist.parentId
            && (!playlistById.contains(playlist.parentId)
                || !result.playlists[playlistById.value(playlist.parentId)].folder)) {
            result.error = QStringLiteral("Invalid OneLibrary playlist parent");
            return result;
        }
        children[playlist.parentId].append(n);
    }
    QVector<Playlist> ordered;
    QVector<qsizetype> pending;
    auto pushChildren = [&](quint32 parent) {
        const auto siblings = children.value(parent);
        for (auto it = siblings.crbegin(); it != siblings.crend(); ++it)
            pending.append(*it);
    };
    pushChildren(0);
    while (!pending.isEmpty()) {
        const auto n = pending.takeLast();
        ordered.append(result.playlists[n]);
        pushChildren(result.playlists[n].id);
    }
    if (ordered.size() != result.playlists.size()) {
        result.error = QStringLiteral("Cyclic OneLibrary playlist hierarchy");
        return result;
    }
    result.playlists = std::move(ordered);
    result.ok = true;
    return result;
}

#endif // BROCKDJ_HAVE_SQLCIPHER

} // namespace

QString DeviceSource::resolveContainedPath(const QString& mountPath,
                                           const QString& devicePath,
                                           bool requireFile)
{
    const QFileInfo rootInfo(mountPath);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !rootInfo.isDir())
        return {};

    QString relative = QDir::fromNativeSeparators(devicePath);
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (relative.startsWith(QLatin1Char('/')))
        relative = relative.mid(1);
    relative = QDir::cleanPath(relative);
    if (relative.isEmpty() || relative == QStringLiteral(".")
        || relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../"))
        || QDir::isAbsolutePath(relative)) {
        return {};
    }

    const QFileInfo candidateInfo(QDir(canonicalRoot).filePath(relative));
    QString candidate = candidateInfo.canonicalFilePath();
    if (candidate.isEmpty()) {
        if (requireFile)
            return {};
        candidate = QDir::cleanPath(candidateInfo.absoluteFilePath());
    }
    const QString rootPrefix = canonicalRoot.endsWith(QLatin1Char('/'))
        ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    if (candidate != canonicalRoot && !candidate.startsWith(rootPrefix))
        return {};
    if (requireFile && (!candidateInfo.isFile() || !candidateInfo.isReadable()))
        return {};
    return candidate;
}

DeviceSource::Result DeviceSource::readIndexReadOnly(const QString& mountPath,
                                                      const QString& deviceId) const
{
    Result result;
    const QString modernPath = resolveContainedPath(
        mountPath, QStringLiteral("PIONEER/rekordbox/exportLibrary.db"));
    PdbReader::Result parsed;
    QString modernError;
    if (!modernPath.isEmpty()) {
#ifdef BROCKDJ_HAVE_SQLCIPHER
        parsed = readModernIndex(modernPath);
#else
        parsed.error = QStringLiteral("OneLibrary browsing requires SQLCipher support");
#endif
        if (!parsed.ok)
            modernError = parsed.error;
    } else if (QFileInfo::exists(QDir(mountPath).filePath(
                   QStringLiteral("PIONEER/rekordbox/exportLibrary.db")))) {
        modernError = QStringLiteral("OneLibrary exportLibrary.db is unreadable or outside the USB root");
    }
    const QString databasePath = resolveContainedPath(
        mountPath, QStringLiteral("PIONEER/rekordbox/export.pdb"));
    if (!parsed.ok && !databasePath.isEmpty()) {
        parsed = PdbReader{}.readReadOnly(databasePath);
        if (parsed.ok && !modernError.isEmpty())
            result.index.warning = modernError + QStringLiteral("; using legacy export.pdb");
    }
    if (!parsed.ok) {
        result.error = modernError.isEmpty() ? parsed.error : modernError;
        if (result.error.isEmpty())
            result.error = QStringLiteral("No readable Rekordbox export.pdb or exportLibrary.db");
        else if (!databasePath.isEmpty() && !modernError.isEmpty())
            result.error += QStringLiteral("; legacy export.pdb: ") + parsed.error;
        return result;
    }

    result.index.deviceId = deviceId;
    result.index.mountPath = QFileInfo(mountPath).canonicalFilePath();
    result.index.tracks = parsed.tracks;
    result.index.playlists = parsed.playlists;
    result.index.trackById.reserve(result.index.tracks.size());
    result.index.trackBySourceAwareId.reserve(result.index.tracks.size());
    for (qsizetype i = 0; i < result.index.tracks.size(); ++i) {
        auto& track = result.index.tracks[i];
        track.sourceAwareId = QStringLiteral("rekordbox:%1:%2")
                                  .arg(deviceId, QString::number(track.id));
        track.audioPath = resolveContainedPath(mountPath, track.relativeAudioPath);
        track.analysisPath = resolveContainedPath(mountPath, track.relativeAnalysisPath);
        if (!track.relativeArtworkPath.isEmpty())
            track.artworkPath = resolveContainedPath(mountPath, track.relativeArtworkPath);
        result.index.trackById.insert(track.id, i);
        result.index.trackBySourceAwareId.insert(track.sourceAwareId, i);
    }
    result.ok = true;
    return result;
}

AnalysisReader::Result DeviceSource::readAnalysisReadOnly(
    const DeviceIndex& index, const QString& sourceAwareTrackId) const
{
    const auto found = index.trackBySourceAwareId.constFind(sourceAwareTrackId);
    if (found == index.trackBySourceAwareId.cend()) {
        AnalysisReader::Result missing;
        missing.error = QStringLiteral("Rekordbox track is not indexed");
        return missing;
    }
    const Track& track = index.tracks.at(*found);
    if (track.analysisPath.isEmpty()) {
        AnalysisReader::Result missing;
        missing.error = QStringLiteral("Rekordbox analysis path is unavailable");
        return missing;
    }
    return AnalysisReader{}.readRelatedReadOnly(track.analysisPath, index.mountPath);
}

bool DeviceIdentityReader::isSupported()
{
#ifdef BROCKDJ_HAVE_SQLCIPHER
    return true;
#else
    return false;
#endif
}

DeviceColor DeviceIdentityReader::colorFromType(int rawColorType)
{
    // Verified against a device exported with the colour set to purple, which
    // stores 8, and against a device left on the default, which stores 0. The
    // numbering is the same one the exporting software uses for track colours.
    switch (rawColorType) {
    case 0: return DeviceColor::None;
    case 1: return DeviceColor::Pink;
    case 2: return DeviceColor::Red;
    case 3: return DeviceColor::Orange;
    case 4: return DeviceColor::Yellow;
    case 5: return DeviceColor::Green;
    case 6: return DeviceColor::Aqua;
    case 7: return DeviceColor::Blue;
    case 8: return DeviceColor::Purple;
    default: return DeviceColor::Unknown;
    }
}

QString DeviceIdentityReader::colorHex(DeviceColor color)
{
    switch (color) {
    case DeviceColor::Pink:   return QStringLiteral("#FF4FA3");
    case DeviceColor::Red:    return QStringLiteral("#FF3B30");
    case DeviceColor::Orange: return QStringLiteral("#FF9500");
    case DeviceColor::Yellow: return QStringLiteral("#FFD60A");
    case DeviceColor::Green:  return QStringLiteral("#34C759");
    case DeviceColor::Aqua:   return QStringLiteral("#32D6D6");
    case DeviceColor::Blue:   return QStringLiteral("#2E8BFF");
    case DeviceColor::Purple: return QStringLiteral("#AF52DE");
    case DeviceColor::None:
    case DeviceColor::Unknown:
        break;
    }
    return {};
}

DeviceIdentity DeviceIdentityReader::readReadOnly(const QString& mountPath) const
{
    DeviceIdentity identity;

#ifdef BROCKDJ_HAVE_SQLCIPHER
    const QString path =
        DeviceSource::resolveContainedPath(mountPath, identityDatabaseRelativePath());
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return identity;

    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(readOnlyUri(path).toUtf8().constData(), &db, flags, nullptr)
        != SQLITE_OK) {
        sqlite3_close(db);
        return identity;
    }

    if (applyKey(db)) {
        sqlite3_stmt* statement = nullptr;
        // A wrong key or an unexpected schema fails right here at prepare time,
        // which is exactly the "not a device database we understand" case.
        if (sqlite3_prepare_v2(db, kIdentityQuery, -1, &statement, nullptr) == SQLITE_OK) {
            if (sqlite3_step(statement) == SQLITE_ROW) {
                if (const unsigned char* name = sqlite3_column_text(statement, 0))
                    identity.name = QString::fromUtf8(reinterpret_cast<const char*>(name)).trimmed();
                identity.rawColorType = sqlite3_column_int(statement, 1);
                identity.color = colorFromType(identity.rawColorType);
            }
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(db);
#else
    Q_UNUSED(mountPath);
#endif

    return identity;
}

} // namespace rekordbox
