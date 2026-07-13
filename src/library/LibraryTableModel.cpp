#include "LibraryTableModel.h"
#include "LibraryDatabase.h"

#include <QDebug>

namespace {

constexpr QChar kBrowseNoMatchSentinel{0};

bool sameIdentityOrder(const QVector<LibraryRow>& a, const QVector<LibraryRow>& b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id)
            return false;
    }
    return true;
}

bool sameRowData(const LibraryRow& a, const LibraryRow& b)
{
    return a.id == b.id
        && a.title == b.title
        && a.artist == b.artist
        && a.durationSec == b.durationSec
        && qFuzzyCompare(a.bpm + 1.0, b.bpm + 1.0)
        && a.key == b.key
        && a.bitrateKbps == b.bitrateKbps
        && a.isAnalyzed == b.isAnalyzed
        && a.filePath == b.filePath
        && a.genre == b.genre
        && a.album == b.album
        && a.comment == b.comment
        && a.rating == b.rating
        && a.energy == b.energy
        && a.color == b.color
        && a.notes == b.notes
        && a.playCount == b.playCount
        && a.lastPlayed == b.lastPlayed
        && a.dateAdded == b.dateAdded;
}

} // namespace

LibraryTableModel::LibraryTableModel(const QString& connectionName,
                                     QObject* parent)
    : QAbstractTableModel(parent)
    , m_connectionName(connectionName)
{
}

void LibraryTableModel::setDatabase(LibraryDatabase* database)
{
    if (m_database == database)
        return;
    if (m_database)
        disconnect(m_database, nullptr, this, nullptr);
    m_database = database;
    if (m_database) {
        connect(m_database, &LibraryDatabase::libraryPageReady,
                this, &LibraryTableModel::applyLibraryPage);
    }
}

int LibraryTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_rows.size();
}

int LibraryTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return 19;
}

QVariant LibraryTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};

    const auto& row = m_rows[index.row()];

    switch (role) {
    case IdRole:         return row.id;
    case TitleRole:      return row.title;
    case ArtistRole:     return row.artist;
    case DurationRole:   return row.durationSec;
    case BpmRole:        return row.bpm;
    case KeyRole:        return row.key;
    case BitrateRole:    return row.bitrateKbps;
    case AnalyzedRole:   return row.isAnalyzed;
    case FilePathRole:   return row.filePath;
    case GenreRole:      return row.genre;
    case AlbumRole:      return row.album;
    case CommentRole:    return row.comment;
    case RatingRole:     return row.rating;
    case EnergyRole:     return row.energy;
    case ColorRole:      return row.color;
    case NotesRole:      return row.notes;
    case PlayCountRole:  return row.playCount;
    case LastPlayedRole: return row.lastPlayed;
    case DateAddedRole:  return row.dateAdded;

    case Qt::DisplayRole:
        switch (index.column()) {
        case 0:  return row.id;
        case 1:  return row.title;
        case 2:  return row.artist;
        case 3:  return row.durationSec;
        case 4:  return row.bpm;
        case 5:  return row.key;
        case 6:  return row.bitrateKbps;
        case 7:  return row.isAnalyzed;
        case 8:  return row.filePath;
        case 9:  return row.genre;
        case 10: return row.album;
        case 11: return row.comment;
        case 12: return row.rating;
        case 13: return row.energy;
        case 14: return row.color;
        case 15: return row.notes;
        case 16: return row.playCount;
        case 17: return row.lastPlayed;
        case 18: return row.dateAdded;
        }
        break;
    }

    return {};
}

QHash<int, QByteArray> LibraryTableModel::roleNames() const
{
    return {
        { IdRole,          "trackId"     },
        { TitleRole,       "title"       },
        { ArtistRole,      "artist"      },
        { DurationRole,    "durationSec" },
        { BpmRole,         "bpm"         },
        { KeyRole,         "key"         },
        { BitrateRole,     "bitrateKbps" },
        { AnalyzedRole,    "isAnalyzed"  },
        { FilePathRole,    "filePath"    },
        { GenreRole,       "genre"       },
        { AlbumRole,       "album"       },
        { CommentRole,     "comment"     },
        { RatingRole,      "rating"      },
        { EnergyRole,      "energy"      },
        { ColorRole,       "trackColor"  },
        { NotesRole,       "notes"       },
        { PlayCountRole,   "playCount"   },
        { LastPlayedRole,  "lastPlayed"  },
        { DateAddedRole,   "dateAdded"   },
    };
}

QString LibraryTableModel::sortColumnSql() const
{
    if (m_sortField == "title")
        return "LOWER(t.title)";
    if (m_sortField == "artist")
        return "LOWER(t.artist)";
    if (m_sortField == "time" || m_sortField == "duration")
        return "t.duration_sec";
    if (m_sortField == "bpm")
        return "t.bpm";
    if (m_sortField == "key")
        return "LOWER(t.key)";
    if (m_sortField == "kbps" || m_sortField == "bitrate")
        return "t.bitrate_kbps";
    if (m_sortField == "genre")
        return "LOWER(COALESCE(t.genre,''))";
    if (m_sortField == "rating")
        return "t.rating";
    if (m_sortField == "energy")
        return "t.energy";
    if (m_sortField == "playCount")
        return "t.play_count";
    if (m_sortField == "dateAdded")
        return "t.date_added";
    return "LOWER(t.artist)";
}

void LibraryTableModel::toggleSort(const QString& field)
{
    if (m_sortField == field) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortField = field;
        m_sortAscending = true;
    }
    emit sortChanged();
    refresh();
}

void LibraryTableModel::setSort(const QString& field, bool ascending)
{
    if (m_sortField == field && m_sortAscending == ascending)
        return;
    m_sortField = field;
    m_sortAscending = ascending;
    emit sortChanged();
    refresh();
}

void LibraryTableModel::setFilterText(const QString& text)
{
    const QString normalized = text.trimmed();
    if (m_filterText == normalized)
        return;
    m_filterText = normalized;
    emit filterTextChanged();
    refresh();
}

void LibraryTableModel::setFilterBpmMin(double v)
{
    if (qFuzzyCompare(m_filterBpmMin + 1.0, v + 1.0)) return;
    m_filterBpmMin = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterBpmMax(double v)
{
    if (qFuzzyCompare(m_filterBpmMax + 1.0, v + 1.0)) return;
    m_filterBpmMax = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterKey(const QString& v)
{
    if (m_filterKey == v) return;
    m_filterKey = v;
    m_filterKeys.clear();
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterArtist(const QString& v)
{
    if (m_filterArtist == v) return;
    m_filterArtist = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterAlbum(const QString& v)
{
    if (m_filterAlbum == v) return;
    m_filterAlbum = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterSourcePath(const QString& v)
{
    if (m_filterSourcePath == v) return;
    m_filterSourcePath = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterKeys(const QStringList& keys)
{
    QStringList normalized;
    for (const QString& k : keys) {
        const QString t = k.trimmed();
        if (!t.isEmpty())
            normalized.append(t);
    }
    if (m_filterKeys == normalized) return;
    m_filterKeys = normalized;
    m_filterKey.clear();
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterGenre(const QString& v)
{
    if (m_filterGenre == v) return;
    m_filterGenre = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterRatingMin(int v)
{
    if (m_filterRatingMin == v) return;
    m_filterRatingMin = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::setFilterEnergyMin(int v)
{
    if (m_filterEnergyMin == v) return;
    m_filterEnergyMin = v;
    emit filtersChanged();
    refresh();
}

void LibraryTableModel::clearFilters()
{
    bool changed = !m_filterText.isEmpty()
        || m_filterBpmMin != 0.0 || m_filterBpmMax != 0.0
        || !m_filterKey.isEmpty() || !m_filterArtist.isEmpty() || !m_filterAlbum.isEmpty()
        || !m_filterSourcePath.isEmpty() || !m_filterKeys.isEmpty() || !m_filterGenre.isEmpty()
        || m_filterRatingMin != 0 || m_filterEnergyMin != 0;

    m_filterText      = {};
    m_filterBpmMin    = 0.0;
    m_filterBpmMax    = 0.0;
    m_filterKey       = {};
    m_filterArtist    = {};
    m_filterAlbum     = {};
    m_filterSourcePath = {};
    m_filterKeys      = {};
    m_filterGenre     = {};
    m_filterRatingMin = 0;
    m_filterEnergyMin = 0;

    if (changed) {
        emit filterTextChanged();
        emit filtersChanged();
        refresh();
    }
}

void LibraryTableModel::clearAioBrowseFilterFields()
{
    m_filterBpmMin    = 0.0;
    m_filterBpmMax    = 0.0;
    m_filterKey       = {};
    m_filterArtist    = {};
    m_filterAlbum     = {};
    m_filterSourcePath = {};
    m_filterKeys      = {};
    m_filterGenre     = {};
    m_filterRatingMin = 0;
    m_filterEnergyMin = 0;
}

bool LibraryTableModel::aioBrowseFiltersEqual(const QString& field, const QString& value,
                                              const QStringList& keys) const
{
    if (field == QStringLiteral("artist"))
        return m_filterArtist == value
            && m_filterAlbum.isEmpty() && m_filterKey.isEmpty()
            && m_filterSourcePath.isEmpty() && m_filterKeys.isEmpty()
            && m_filterBpmMin == 0.0 && m_filterBpmMax == 0.0
            && m_filterGenre.isEmpty() && m_filterRatingMin == 0 && m_filterEnergyMin == 0;
    if (field == QStringLiteral("album"))
        return m_filterAlbum == value
            && m_filterArtist.isEmpty() && m_filterKey.isEmpty()
            && m_filterSourcePath.isEmpty() && m_filterKeys.isEmpty()
            && m_filterBpmMin == 0.0 && m_filterBpmMax == 0.0
            && m_filterGenre.isEmpty() && m_filterRatingMin == 0 && m_filterEnergyMin == 0;
    if (field == QStringLiteral("key"))
        return m_filterKey == value
            && m_filterArtist.isEmpty() && m_filterAlbum.isEmpty()
            && m_filterSourcePath.isEmpty() && m_filterKeys.isEmpty()
            && m_filterBpmMin == 0.0 && m_filterBpmMax == 0.0
            && m_filterGenre.isEmpty() && m_filterRatingMin == 0 && m_filterEnergyMin == 0;
    if (field == QStringLiteral("source"))
        return m_filterSourcePath == value
            && m_filterArtist.isEmpty() && m_filterAlbum.isEmpty()
            && m_filterKey.isEmpty() && m_filterKeys.isEmpty()
            && m_filterBpmMin == 0.0 && m_filterBpmMax == 0.0
            && m_filterGenre.isEmpty() && m_filterRatingMin == 0 && m_filterEnergyMin == 0;
    if (field == QStringLiteral("keys")) {
        QStringList normalized;
        for (const QString& k : keys) {
            const QString t = k.trimmed();
            if (!t.isEmpty())
                normalized.append(t);
        }
        return m_filterKeys == normalized
            && m_filterArtist.isEmpty() && m_filterAlbum.isEmpty()
            && m_filterKey.isEmpty() && m_filterSourcePath.isEmpty()
            && m_filterBpmMin == 0.0 && m_filterBpmMax == 0.0
            && m_filterGenre.isEmpty() && m_filterRatingMin == 0 && m_filterEnergyMin == 0;
    }
    return false;
}

void LibraryTableModel::applyAioBrowseFilter(const QString& field, const QString& value,
                                             const QStringList& keys)
{
    if (aioBrowseFiltersEqual(field, value, keys))
        return;

    clearAioBrowseFilterFields();

    if (field == QStringLiteral("artist"))
        m_filterArtist = value;
    else if (field == QStringLiteral("album"))
        m_filterAlbum = value;
    else if (field == QStringLiteral("key"))
        m_filterKey = value;
    else if (field == QStringLiteral("source"))
        m_filterSourcePath = value;
    else if (field == QStringLiteral("keys")) {
        for (const QString& k : keys) {
            const QString t = k.trimmed();
            if (!t.isEmpty())
                m_filterKeys.append(t);
        }
    }

    emit filtersChanged();
    refresh();
}

void LibraryTableModel::refresh()
{
    if (!m_database) {
        qWarning() << "[LibraryTableModel] database worker unavailable for refresh";
        return;
    }

    // Build WHERE conditions dynamically.
    QStringList conditions;
    QVariantMap bindings;

    if (!m_filterText.isEmpty()) {
        const QString fv = "%" + m_filterText.toLower() + "%";
        conditions << "(LOWER(t.title) LIKE :filter"
                      " OR LOWER(t.artist) LIKE :filter"
                      " OR LOWER(COALESCE(t.genre,'')) LIKE :filter"
                      " OR LOWER(COALESCE(t.album,'')) LIKE :filter"
                      " OR LOWER(COALESCE(t.comment,'')) LIKE :filter"
                      " OR LOWER(COALESCE(t.notes,'')) LIKE :filter"
                      " OR LOWER(COALESCE(l.file_path,'')) LIKE :filter"
                      " OR EXISTS (SELECT 1 FROM TrackTags tt"
                      "   JOIN Tags tg ON tt.tag_id = tg.id"
                      "   WHERE tt.track_id = t.id AND LOWER(tg.name) LIKE :filter))";
        bindings[":filter"] = fv;
    }

    if (m_filterBpmMin > 0.0) {
        conditions << "t.bpm >= :bpmMin";
        bindings[":bpmMin"] = m_filterBpmMin;
    }
    if (m_filterBpmMax > 0.0) {
        conditions << "t.bpm <= :bpmMax";
        bindings[":bpmMax"] = m_filterBpmMax;
    }
    if (!m_filterKey.isEmpty()) {
        if (m_filterKey == QStringLiteral("(Unknown)") || m_filterKey == QStringLiteral("?")) {
            conditions << "(t.key IS NULL OR TRIM(t.key) = '')";
        } else {
            conditions << "LOWER(TRIM(COALESCE(t.key,''))) = LOWER(TRIM(:fkey))";
            bindings[":fkey"] = m_filterKey;
        }
    }
    if (!m_filterArtist.isEmpty()) {
        if (m_filterArtist == QString(kBrowseNoMatchSentinel)) {
            conditions << "1=0";
        } else if (m_filterArtist == QStringLiteral("(Unknown)")) {
            conditions << "(t.artist IS NULL OR TRIM(t.artist) = '')";
        } else {
            conditions << "LOWER(TRIM(COALESCE(t.artist,''))) = LOWER(TRIM(:fartist))";
            bindings[":fartist"] = m_filterArtist;
        }
    }
    if (!m_filterAlbum.isEmpty()) {
        if (m_filterAlbum == QString(kBrowseNoMatchSentinel)) {
            conditions << "1=0";
        } else if (m_filterAlbum == QStringLiteral("(Unknown)")) {
            conditions << "(t.album IS NULL OR TRIM(t.album) = '')";
        } else {
            conditions << "LOWER(TRIM(COALESCE(t.album,''))) = LOWER(TRIM(:falbum))";
            bindings[":falbum"] = m_filterAlbum;
        }
    }
    if (!m_filterSourcePath.isEmpty()) {
        conditions << "LOWER(l.file_path) LIKE LOWER(:fsource)";
        bindings[":fsource"] = m_filterSourcePath + "%";
    }
    if (!m_filterKeys.isEmpty()) {
        QStringList placeholders;
        for (int i = 0; i < m_filterKeys.size(); ++i) {
            const QString ph = QStringLiteral(":fkeys%1").arg(i);
            placeholders << ph;
            bindings[ph] = m_filterKeys.at(i);
        }
        conditions << QStringLiteral("LOWER(COALESCE(t.key,'')) IN (%1)")
                            .arg(placeholders.join(", "));
    }
    if (!m_filterGenre.isEmpty()) {
        conditions << "LOWER(COALESCE(t.genre,'')) LIKE LOWER(:fgenre)";
        bindings[":fgenre"] = "%" + m_filterGenre + "%";
    }
    if (m_filterRatingMin > 0) {
        conditions << "COALESCE(t.rating,0) >= :ratingMin";
        bindings[":ratingMin"] = m_filterRatingMin;
    }
    if (m_filterEnergyMin > 0) {
        conditions << "COALESCE(t.energy,0) >= :energyMin";
        bindings[":energyMin"] = m_filterEnergyMin;
    }

    const QString sortDir = m_sortAscending ? "ASC" : "DESC";
    QString query =
        "SELECT t.id AS id, t.title AS title, t.artist AS artist,"
        "       t.duration_sec AS duration_sec, t.bpm AS bpm, t.key AS key,"
        "       t.bitrate_kbps AS bitrate_kbps, t.is_analyzed AS is_analyzed,"
        "       COALESCE(l.file_path,'') AS file_path,"
        "       COALESCE(t.genre,'') AS genre, COALESCE(t.album,'') AS album,"
        "       COALESCE(t.comment,'') AS comment, COALESCE(t.rating,0) AS rating,"
        "       COALESCE(t.energy,0) AS energy, COALESCE(t.color,'') AS color,"
        "       COALESCE(t.notes,'') AS notes, COALESCE(t.play_count,0) AS play_count,"
        "       COALESCE(t.last_played,0) AS last_played,"
        "       COALESCE(t.date_added,0) AS date_added"
        " FROM Tracks t"
        " JOIN Locations l ON t.id = l.track_id";

    if (!conditions.isEmpty())
        query += " WHERE " + conditions.join(" AND ");

    query += QString(" ORDER BY %1 %2, LOWER(t.title) ASC")
        .arg(sortColumnSql(), sortDir);

    const auto generation = ++m_refreshGeneration;
    if (!m_database->requestLibraryPage(std::move(query), std::move(bindings), generation))
        qWarning() << "[LibraryTableModel] database worker rejected refresh";
}

void LibraryTableModel::applyLibraryPage(std::uint64_t generation,
                                         const QVariantList& rows,
                                         const QString& error)
{
    if (generation != m_refreshGeneration)
        return;
    if (!error.isEmpty()) {
        qWarning() << "[LibraryTableModel] refresh query failed:" << error;
        return;
    }

    QVector<LibraryRow> newRows;
    newRows.reserve(rows.size());
    for (const auto& value : rows) {
        const QVariantMap q = value.toMap();
        LibraryRow row;
        row.id          = q.value(QStringLiteral("id")).toString();
        row.title       = q.value(QStringLiteral("title")).toString();
        row.artist      = q.value(QStringLiteral("artist")).toString();
        row.durationSec = q.value(QStringLiteral("duration_sec")).toInt();
        row.bpm         = q.value(QStringLiteral("bpm")).toDouble();
        row.key         = q.value(QStringLiteral("key")).toString();
        row.bitrateKbps = q.value(QStringLiteral("bitrate_kbps")).toInt();
        row.isAnalyzed  = q.value(QStringLiteral("is_analyzed")).toBool();
        row.filePath    = q.value(QStringLiteral("file_path")).toString();
        row.genre       = q.value(QStringLiteral("genre")).toString();
        row.album       = q.value(QStringLiteral("album")).toString();
        row.comment     = q.value(QStringLiteral("comment")).toString();
        row.rating      = q.value(QStringLiteral("rating")).toInt();
        row.energy      = q.value(QStringLiteral("energy")).toInt();
        row.color       = q.value(QStringLiteral("color")).toString();
        row.notes       = q.value(QStringLiteral("notes")).toString();
        row.playCount   = q.value(QStringLiteral("play_count")).toInt();
        row.lastPlayed  = q.value(QStringLiteral("last_played")).toLongLong();
        row.dateAdded   = q.value(QStringLiteral("date_added")).toLongLong();
        newRows.append(std::move(row));
    }

    if (sameIdentityOrder(m_rows, newRows)) {
        for (qsizetype rowIndex = 0; rowIndex < newRows.size(); ++rowIndex) {
            if (sameRowData(m_rows[rowIndex], newRows[rowIndex]))
                continue;
            m_rows[rowIndex] = std::move(newRows[rowIndex]);
            emit dataChanged(index(static_cast<int>(rowIndex), 0),
                             index(static_cast<int>(rowIndex), columnCount() - 1),
                             {});
        }
    } else {
        beginResetModel();
        m_rows = std::move(newRows);
        endResetModel();
        emit countChanged();
    }
}

QString LibraryTableModel::filePathAtRow(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows[row].filePath;
}

QString LibraryTableModel::trackIdAtRow(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows[row].id;
}

int LibraryTableModel::indexOfTrackId(const QString& trackId) const
{
    if (trackId.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id == trackId)
            return i;
    }
    return -1;
}

void LibraryTableModel::refreshMetaForTrack(const QString& trackId)
{
    if (!trackId.isEmpty())
        refresh();
}

void LibraryTableModel::updateAnalysisForTrack(const QString& trackId,
                                               double bpm,
                                               const QString& key,
                                               bool isAnalyzed)
{
    if (trackId.isEmpty())
        return;

    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        auto& row = m_rows[rowIndex];
        if (row.id != trackId)
            continue;

        bool changed = false;

        if (bpm > 0.0 && !qFuzzyCompare(row.bpm + 1.0, bpm + 1.0)) {
            row.bpm = bpm;
            changed = true;
        }
        if (!key.trimmed().isEmpty() && row.key != key) {
            row.key = key;
            changed = true;
        }
        if (row.isAnalyzed != isAnalyzed) {
            row.isAnalyzed = isAnalyzed;
            changed = true;
        }

        if (!changed)
            return;

        const QModelIndex left  = index(rowIndex, 4);
        const QModelIndex right = index(rowIndex, 7);
        emit dataChanged(left, right, { Qt::DisplayRole, BpmRole, KeyRole, AnalyzedRole });
        return;
    }
}
