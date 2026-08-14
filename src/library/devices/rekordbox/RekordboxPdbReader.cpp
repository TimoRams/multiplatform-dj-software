#include "library/devices/rekordbox/RekordboxPdbReader.h"

#include <QFile>
#include <QSet>

#include <algorithm>
#include <functional>
#include <limits>

namespace rekordbox {
namespace {

constexpr qsizetype kMaxDatabaseBytes = 512 * 1024 * 1024;
constexpr quint32 kMinPageSize = 512;
constexpr quint32 kMaxPageSize = 64 * 1024;
constexpr quint32 kMaxTables = 64;
constexpr quint32 kMaxPages = 1'000'000;
constexpr quint32 kMaxRows = 2'000'000;
constexpr qsizetype kMaxStringBytes = 1024 * 1024;
constexpr qsizetype kPageHeaderSize = 40;

bool rangeFits(qsizetype offset, qsizetype length, qsizetype size)
{
    return offset >= 0 && length >= 0 && offset <= size && length <= size - offset;
}

bool readU8(const QByteArray& data, qsizetype offset, quint8* value)
{
    if (!value || !rangeFits(offset, 1, data.size()))
        return false;
    *value = static_cast<quint8>(data.at(offset));
    return true;
}

bool readLe16(const QByteArray& data, qsizetype offset, quint16* value)
{
    if (!value || !rangeFits(offset, 2, data.size()))
        return false;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    *value = static_cast<quint16>(p[0] | (static_cast<quint16>(p[1]) << 8));
    return true;
}

bool readLe24(const QByteArray& data, qsizetype offset, quint32* value)
{
    if (!value || !rangeFits(offset, 3, data.size()))
        return false;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    *value = static_cast<quint32>(p[0])
        | (static_cast<quint32>(p[1]) << 8)
        | (static_cast<quint32>(p[2]) << 16);
    return true;
}

bool readLe32(const QByteArray& data, qsizetype offset, quint32* value)
{
    if (!value || !rangeFits(offset, 4, data.size()))
        return false;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    *value = static_cast<quint32>(p[0])
        | (static_cast<quint32>(p[1]) << 8)
        | (static_cast<quint32>(p[2]) << 16)
        | (static_cast<quint32>(p[3]) << 24);
    return true;
}

QString decodeDeviceSqlString(const QByteArray& data, qsizetype offset,
                              qsizetype pageEnd, bool* valid)
{
    if (valid)
        *valid = false;
    quint8 marker = 0;
    if (!readU8(data, offset, &marker) || offset >= pageEnd)
        return {};

    if (marker != 0x40 && marker != 0x90) {
        if ((marker & 1U) == 0)
            return {};
        const qsizetype encodedLength = marker >> 1;
        if (encodedLength < 1 || encodedLength - 1 > kMaxStringBytes
            || !rangeFits(offset + 1, encodedLength - 1, pageEnd)) {
            return {};
        }
        if (valid)
            *valid = true;
        return QString::fromLatin1(data.constData() + offset + 1,
                                   encodedLength - 1);
    }

    quint16 encodedLength = 0;
    if (!readLe16(data, offset + 1, &encodedLength) || encodedLength < 4
        || encodedLength - 4 > kMaxStringBytes
        || !rangeFits(offset, encodedLength, pageEnd)) {
        return {};
    }
    const qsizetype payloadOffset = offset + 4;
    const qsizetype payloadLength = encodedLength - 4;
    if (marker == 0x40) {
        if (valid)
            *valid = true;
        return QString::fromLatin1(data.constData() + payloadOffset, payloadLength);
    }

    if ((payloadLength & 1) != 0)
        return {};
    QString decoded;
    decoded.reserve(payloadLength / 2);
    for (qsizetype i = 0; i < payloadLength; i += 2) {
        quint16 codeUnit = 0;
        if (!readLe16(data, payloadOffset + i, &codeUnit))
            return {};
        if (codeUnit == 0)
            break;
        decoded.append(QChar(codeUnit));
    }
    if (valid)
        *valid = true;
    return decoded;
}

struct NameRow {
    quint32 id = 0;
    QString name;
};

struct ColorRow {
    quint32 id = 0;
    QString name;
};

struct TrackRow {
    Track track;
    quint32 artistId = 0;
    quint32 albumId = 0;
    quint32 genreId = 0;
    quint32 keyId = 0;
    quint32 artworkId = 0;
    quint32 colorId = 0;
};

struct PlaylistEntry {
    quint32 index = 0;
    quint32 trackId = 0;
    quint32 playlistId = 0;
};

struct ParsedRows {
    QVector<TrackRow> tracks;
    QVector<NameRow> artists;
    QVector<NameRow> albums;
    QVector<NameRow> genres;
    QVector<NameRow> keys;
    QVector<NameRow> artwork;
    QVector<ColorRow> colors;
    QVector<Playlist> playlists;
    QVector<PlaylistEntry> playlistEntries;
    quint32 total = 0;
};

bool readStringAt(const QByteArray& data, qsizetype rowBase, quint16 relativeOffset,
                  qsizetype pageEnd, QString* output)
{
    if (!output || relativeOffset == 0 || rowBase > pageEnd - relativeOffset)
        return false;
    bool valid = false;
    const QString value = decodeDeviceSqlString(data, rowBase + relativeOffset,
                                                pageEnd, &valid);
    if (!valid)
        return false;
    *output = value;
    return true;
}

bool parseNameRow(const QByteArray& data, quint32 type, qsizetype rowBase,
                  qsizetype pageEnd, ParsedRows* rows)
{
    if (!rows)
        return false;
    NameRow result;
    quint16 subtype = 0;
    quint8 nearOffset = 0;
    quint16 farOffset = 0;
    quint16 nameOffset = 0;

    if (type == 1) { // genre: id + inline DeviceSQL string
        if (!readLe32(data, rowBase, &result.id))
            return false;
        bool valid = false;
        result.name = decodeDeviceSqlString(data, rowBase + 4, pageEnd, &valid);
        if (!valid)
            return false;
        rows->genres.append(std::move(result));
        return true;
    }
    if (type == 5) { // key: id, duplicate id, inline string
        if (!readLe32(data, rowBase, &result.id))
            return false;
        bool valid = false;
        result.name = decodeDeviceSqlString(data, rowBase + 8, pageEnd, &valid);
        if (!valid)
            return false;
        rows->keys.append(std::move(result));
        return true;
    }
    if (type == 13) { // artwork: id + inline path
        if (!readLe32(data, rowBase, &result.id))
            return false;
        bool valid = false;
        result.name = decodeDeviceSqlString(data, rowBase + 4, pageEnd, &valid);
        if (!valid)
            return false;
        rows->artwork.append(std::move(result));
        return true;
    }
    if (type == 2) { // artist
        if (!readLe16(data, rowBase, &subtype)
            || !readLe32(data, rowBase + 4, &result.id)
            || !readU8(data, rowBase + 9, &nearOffset)) {
            return false;
        }
        nameOffset = nearOffset;
        if ((subtype & 0x04) != 0) {
            if (!readLe16(data, rowBase + 0x0a, &farOffset))
                return false;
            nameOffset = farOffset;
        }
        if (!readStringAt(data, rowBase, nameOffset, pageEnd, &result.name))
            return false;
        rows->artists.append(std::move(result));
        return true;
    }
    if (type == 3) { // album
        if (!readLe16(data, rowBase, &subtype)
            || !readLe32(data, rowBase + 12, &result.id)
            || !readU8(data, rowBase + 21, &nearOffset)) {
            return false;
        }
        nameOffset = nearOffset;
        if ((subtype & 0x04) != 0) {
            if (!readLe16(data, rowBase + 0x16, &farOffset))
                return false;
            nameOffset = farOffset;
        }
        if (!readStringAt(data, rowBase, nameOffset, pageEnd, &result.name))
            return false;
        rows->albums.append(std::move(result));
        return true;
    }
    return false;
}

bool parseTrackRow(const QByteArray& data, qsizetype rowBase, qsizetype pageEnd,
                   ParsedRows* rows)
{
    constexpr qsizetype kTrackFixedSize = 136;
    if (!rows || !rangeFits(rowBase, kTrackFixedSize, pageEnd))
        return false;

    TrackRow result;
    quint32 tempo = 0;
    quint16 duration = 0;
    quint8 color = 0;
    quint8 rating = 0;
    quint32 bitrate = 0;
    if (!readLe32(data, rowBase + 28, &result.artworkId)
        || !readLe32(data, rowBase + 32, &result.keyId)
        || !readLe32(data, rowBase + 48, &bitrate)
        || !readLe32(data, rowBase + 56, &tempo)
        || !readLe32(data, rowBase + 60, &result.genreId)
        || !readLe32(data, rowBase + 64, &result.albumId)
        || !readLe32(data, rowBase + 68, &result.artistId)
        || !readLe32(data, rowBase + 72, &result.track.id)
        || !readLe16(data, rowBase + 84, &duration)
        || !readU8(data, rowBase + 88, &color)
        || !readU8(data, rowBase + 89, &rating)) {
        return false;
    }
    result.track.bpm = static_cast<double>(tempo) / 100.0;
    result.track.durationSec = duration;
    result.track.bitrateKbps = bitrate > static_cast<quint32>(std::numeric_limits<int>::max())
        ? 0 : static_cast<int>(bitrate);
    result.track.rating = std::min<int>(rating, 5);
    result.colorId = color;

    quint16 offsets[21]{};
    for (int i = 0; i < 21; ++i) {
        if (!readLe16(data, rowBase + 94 + i * 2, &offsets[i]))
            return false;
    }
    if (!readStringAt(data, rowBase, offsets[14], pageEnd,
                      &result.track.relativeAnalysisPath)
        || !readStringAt(data, rowBase, offsets[16], pageEnd,
                         &result.track.comment)
        || !readStringAt(data, rowBase, offsets[17], pageEnd,
                         &result.track.title)
        || !readStringAt(data, rowBase, offsets[20], pageEnd,
                         &result.track.relativeAudioPath)) {
        return false;
    }
    rows->tracks.append(std::move(result));
    return true;
}

bool parsePlaylistRow(const QByteArray& data, qsizetype rowBase, qsizetype pageEnd,
                      ParsedRows* rows)
{
    if (!rows || !rangeFits(rowBase, 20, pageEnd))
        return false;
    Playlist playlist;
    quint32 rawFolder = 0;
    if (!readLe32(data, rowBase, &playlist.parentId)
        || !readLe32(data, rowBase + 8, &playlist.sortOrder)
        || !readLe32(data, rowBase + 12, &playlist.id)
        || !readLe32(data, rowBase + 16, &rawFolder)) {
        return false;
    }
    bool valid = false;
    playlist.name = decodeDeviceSqlString(data, rowBase + 20, pageEnd, &valid);
    if (!valid)
        return false;
    playlist.folder = rawFolder != 0;
    rows->playlists.append(std::move(playlist));
    return true;
}

bool parseColorRow(const QByteArray& data, qsizetype rowBase, qsizetype pageEnd,
                   ParsedRows* rows)
{
    if (!rows || !rangeFits(rowBase, 8, pageEnd))
        return false;
    ColorRow color;
    quint16 id = 0;
    if (!readLe16(data, rowBase + 5, &id))
        return false;
    color.id = id;
    bool valid = false;
    color.name = decodeDeviceSqlString(data, rowBase + 8, pageEnd, &valid);
    if (!valid)
        return false;
    rows->colors.append(std::move(color));
    return true;
}

bool parseRow(const QByteArray& data, quint32 type, qsizetype rowBase,
              qsizetype pageEnd, ParsedRows* rows)
{
    if (++rows->total > kMaxRows)
        return false;
    if (type == 0)
        return parseTrackRow(data, rowBase, pageEnd, rows);
    if (type == 1 || type == 2 || type == 3 || type == 5 || type == 13)
        return parseNameRow(data, type, rowBase, pageEnd, rows);
    if (type == 6)
        return parseColorRow(data, rowBase, pageEnd, rows);
    if (type == 7)
        return parsePlaylistRow(data, rowBase, pageEnd, rows);
    if (type == 8) {
        PlaylistEntry entry;
        if (!rangeFits(rowBase, 12, pageEnd)
            || !readLe32(data, rowBase, &entry.index)
            || !readLe32(data, rowBase + 4, &entry.trackId)
            || !readLe32(data, rowBase + 8, &entry.playlistId)) {
            return false;
        }
        rows->playlistEntries.append(entry);
    }
    return true;
}

bool parsePageRows(const QByteArray& data, quint32 pageSize, quint32 pageIndex,
                   quint32 expectedType, quint32* nextPage, ParsedRows* rows,
                   QString* error)
{
    if (pageIndex > static_cast<quint32>(std::numeric_limits<qsizetype>::max() / pageSize)) {
        *error = QStringLiteral("PDB page offset overflow");
        return false;
    }
    const qsizetype pageStart = static_cast<qsizetype>(pageIndex) * pageSize;
    const qsizetype pageEnd = pageStart + pageSize;
    if (!rangeFits(pageStart, pageSize, data.size())) {
        *error = QStringLiteral("PDB page lies outside the file");
        return false;
    }

    quint32 storedIndex = 0;
    quint32 type = 0;
    quint32 packedCounts = 0;
    quint8 flags = 0;
    if (!readLe32(data, pageStart + 4, &storedIndex)
        || !readLe32(data, pageStart + 8, &type)
        || !readLe32(data, pageStart + 12, nextPage)
        || !readLe24(data, pageStart + 24, &packedCounts)
        || !readU8(data, pageStart + 27, &flags)) {
        *error = QStringLiteral("Truncated PDB page header");
        return false;
    }
    if (storedIndex != pageIndex || type != expectedType) {
        *error = QStringLiteral("PDB page chain type/index mismatch");
        return false;
    }

    const quint32 offsetCount = packedCounts & 0x1fffU;
    if (offsetCount > (pageSize - kPageHeaderSize) / 2) {
        *error = QStringLiteral("PDB row offset count is invalid");
        return false;
    }
    if ((flags & 0x40U) != 0)
        return true;

    const quint32 groupCount = offsetCount == 0 ? 0 : ((offsetCount - 1) / 16 + 1);
    for (quint32 group = 0; group < groupCount; ++group) {
        const qsizetype base = pageEnd - static_cast<qsizetype>(group) * 0x24;
        quint16 present = 0;
        if (!readLe16(data, base - 4, &present)) {
            *error = QStringLiteral("Truncated PDB row presence map");
            return false;
        }
        for (quint32 item = 0; item < 16; ++item) {
            const quint32 rowIndex = group * 16 + item;
            if (rowIndex >= offsetCount || (present & (1U << item)) == 0)
                continue;
            quint16 rowOffset = 0;
            if (!readLe16(data, base - (6 + 2 * item), &rowOffset)) {
                *error = QStringLiteral("Truncated PDB row offset");
                return false;
            }
            const qsizetype rowBase = pageStart + kPageHeaderSize + rowOffset;
            if (rowBase < pageStart + kPageHeaderSize || rowBase >= pageEnd
                || !parseRow(data, type, rowBase, pageEnd, rows)) {
                *error = QStringLiteral("Invalid PDB row");
                return false;
            }
        }
    }
    return true;
}

template <typename Row>
QHash<quint32, QString> toNameMap(const QVector<Row>& rows)
{
    QHash<quint32, QString> result;
    result.reserve(rows.size());
    for (const auto& row : rows)
        result.insert(row.id, row.name);
    return result;
}

} // namespace

PdbReader::Result PdbReader::readReadOnly(const QString& path) const
{
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot open export.pdb read-only: %1")
                           .arg(file.errorString());
        return result;
    }
    if (file.size() < 28 || file.size() > kMaxDatabaseBytes) {
        result.error = QStringLiteral("PDB size is outside safe limits");
        return result;
    }
    const QByteArray data = file.readAll();
    if (data.size() != file.size()) {
        result.error = QStringLiteral("Could not read the complete PDB");
        return result;
    }

    quint32 pageSize = 0;
    quint32 tableCount = 0;
    if (!readLe32(data, 4, &pageSize) || !readLe32(data, 8, &tableCount)
        || pageSize < kMinPageSize || pageSize > kMaxPageSize
        || (pageSize & (pageSize - 1)) != 0 || tableCount == 0
        || tableCount > kMaxTables
        || !rangeFits(28, static_cast<qsizetype>(tableCount) * 16,
                      std::min<qsizetype>(data.size(), pageSize))) {
        result.error = QStringLiteral("Invalid PDB header");
        return result;
    }

    ParsedRows parsed;
    for (quint32 tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
        const qsizetype tableOffset = 28 + static_cast<qsizetype>(tableIndex) * 16;
        quint32 type = 0;
        quint32 firstPage = 0;
        quint32 lastPage = 0;
        if (!readLe32(data, tableOffset, &type)
            || !readLe32(data, tableOffset + 8, &firstPage)
            || !readLe32(data, tableOffset + 12, &lastPage)) {
            result.error = QStringLiteral("Truncated PDB table list");
            return result;
        }
        if (type > 19)
            continue;

        QSet<quint32> visited;
        quint32 page = firstPage;
        bool reachedLastPage = false;
        while (page < kMaxPages && !visited.contains(page)) {
            visited.insert(page);
            quint32 nextPage = 0;
            QString pageError;
            if (!parsePageRows(data, pageSize, page, type, &nextPage,
                               &parsed, &pageError)) {
                result.error = pageError;
                return result;
            }
            if (page == lastPage) {
                reachedLastPage = true;
                break;
            }
            if (nextPage == page || static_cast<quint64>(nextPage) * pageSize
                                     >= static_cast<quint64>(data.size())) {
                result.error = QStringLiteral("Invalid PDB page chain");
                return result;
            }
            page = nextPage;
        }
        if (!reachedLastPage) {
            result.error = visited.size() >= static_cast<int>(kMaxPages)
                ? QStringLiteral("PDB page chain exceeds safe limit")
                : QStringLiteral("PDB page chain is cyclic or incomplete");
            return result;
        }
    }

    const auto artists = toNameMap(parsed.artists);
    const auto albums = toNameMap(parsed.albums);
    const auto genres = toNameMap(parsed.genres);
    const auto keys = toNameMap(parsed.keys);
    const auto artwork = toNameMap(parsed.artwork);
    const auto colors = toNameMap(parsed.colors);

    result.tracks.reserve(parsed.tracks.size());
    for (auto& row : parsed.tracks) {
        row.track.artist = artists.value(row.artistId);
        row.track.album = albums.value(row.albumId);
        row.track.genre = genres.value(row.genreId);
        row.track.key = keys.value(row.keyId);
        row.track.relativeArtworkPath = artwork.value(row.artworkId);
        row.track.color = colors.value(row.colorId);
        result.tracks.append(std::move(row.track));
    }

    std::sort(parsed.playlistEntries.begin(), parsed.playlistEntries.end(),
              [](const PlaylistEntry& a, const PlaylistEntry& b) {
                  if (a.playlistId != b.playlistId)
                      return a.playlistId < b.playlistId;
                  return a.index < b.index;
              });
    QHash<quint32, qsizetype> playlistById;
    playlistById.reserve(parsed.playlists.size());
    for (qsizetype i = 0; i < parsed.playlists.size(); ++i)
        playlistById.insert(parsed.playlists[i].id, i);
    for (const auto& entry : parsed.playlistEntries) {
        const auto found = playlistById.constFind(entry.playlistId);
        if (found != playlistById.cend())
            parsed.playlists[*found].trackIds.append(entry.trackId);
    }
    // DeviceSQL heap order is not the playlist presentation order. Publish a
    // deterministic parent-first traversal using Rekordbox's explicit order.
    QHash<quint32, QVector<qsizetype>> children;
    children.reserve(parsed.playlists.size());
    for (qsizetype i = 0; i < parsed.playlists.size(); ++i)
        children[parsed.playlists.at(i).parentId].append(i);
    for (auto it = children.begin(); it != children.end(); ++it) {
        std::stable_sort(it.value().begin(), it.value().end(),
                         [&parsed](qsizetype a, qsizetype b) {
            const auto& left = parsed.playlists.at(a);
            const auto& right = parsed.playlists.at(b);
            if (left.sortOrder != right.sortOrder)
                return left.sortOrder < right.sortOrder;
            return left.id < right.id;
        });
    }
    QVector<Playlist> orderedPlaylists;
    orderedPlaylists.reserve(parsed.playlists.size());
    QSet<quint32> emitted;
    std::function<void(quint32, int)> appendChildren =
        [&](quint32 parentId, int depth) {
            if (depth > 64)
                return;
            for (qsizetype index : children.value(parentId)) {
                const Playlist& playlist = parsed.playlists.at(index);
                if (emitted.contains(playlist.id))
                    continue;
                emitted.insert(playlist.id);
                orderedPlaylists.append(playlist);
                appendChildren(playlist.id, depth + 1);
            }
        };
    appendChildren(0, 0);
    // Keep malformed/orphaned nodes visible without risking recursive cycles.
    QVector<qsizetype> remaining;
    for (qsizetype i = 0; i < parsed.playlists.size(); ++i) {
        if (!emitted.contains(parsed.playlists.at(i).id))
            remaining.append(i);
    }
    std::stable_sort(remaining.begin(), remaining.end(),
                     [&parsed](qsizetype a, qsizetype b) {
        const auto& left = parsed.playlists.at(a);
        const auto& right = parsed.playlists.at(b);
        if (left.sortOrder != right.sortOrder)
            return left.sortOrder < right.sortOrder;
        return left.id < right.id;
    });
    for (qsizetype index : remaining) {
        const Playlist& playlist = parsed.playlists.at(index);
        if (emitted.contains(playlist.id))
            continue;
        emitted.insert(playlist.id);
        orderedPlaylists.append(playlist);
        appendChildren(playlist.id, 1);
    }
    result.playlists = std::move(orderedPlaylists);
    result.ok = true;
    return result;
}

} // namespace rekordbox
