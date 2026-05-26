#include "LibraryTableModel.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QElapsedTimer>

LibraryTableModel::LibraryTableModel(const QString& connectionName,
                                     QObject* parent)
    : QAbstractTableModel(parent)
    , m_connectionName(connectionName)
{
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
        || !m_filterKey.isEmpty() || !m_filterGenre.isEmpty()
        || m_filterRatingMin != 0 || m_filterEnergyMin != 0;

    m_filterText      = {};
    m_filterBpmMin    = 0.0;
    m_filterBpmMax    = 0.0;
    m_filterKey       = {};
    m_filterGenre     = {};
    m_filterRatingMin = 0;
    m_filterEnergyMin = 0;

    if (changed) {
        emit filterTextChanged();
        emit filtersChanged();
        refresh();
    }
}

void LibraryTableModel::refresh()
{
    QElapsedTimer timer;
    timer.start();

    auto db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        qWarning() << "[LibraryTableModel] DB not open for refresh";
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
        conditions << "LOWER(COALESCE(t.key,'')) = LOWER(:fkey)";
        bindings[":fkey"] = m_filterKey;
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
        "SELECT t.id, t.title, t.artist, t.duration_sec, t.bpm, t.key,"
        "       t.bitrate_kbps, t.is_analyzed, COALESCE(l.file_path,''),"
        "       COALESCE(t.genre,''), COALESCE(t.album,''), COALESCE(t.comment,''),"
        "       COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0),"
        "       COALESCE(t.last_played,0), COALESCE(t.date_added,0)"
        " FROM Tracks t"
        " JOIN Locations l ON t.id = l.track_id";

    if (!conditions.isEmpty())
        query += " WHERE " + conditions.join(" AND ");

    query += QString(" ORDER BY %1 %2, LOWER(t.title) ASC")
        .arg(sortColumnSql(), sortDir);

    QSqlQuery q(db);
    q.prepare(query);
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
        q.bindValue(it.key(), it.value());

    if (!q.exec()) {
        qWarning() << "[LibraryTableModel] refresh query failed:" << q.lastError().text();
        return;
    }

    beginResetModel();
    m_rows.clear();
    while (q.next()) {
        LibraryRow row;
        row.id          = q.value(0).toString();
        row.title       = q.value(1).toString();
        row.artist      = q.value(2).toString();
        row.durationSec = q.value(3).toInt();
        row.bpm         = q.value(4).toDouble();
        row.key         = q.value(5).toString();
        row.bitrateKbps = q.value(6).toInt();
        row.isAnalyzed  = q.value(7).toBool();
        row.filePath    = q.value(8).toString();
        row.genre       = q.value(9).toString();
        row.album       = q.value(10).toString();
        row.comment     = q.value(11).toString();
        row.rating      = q.value(12).toInt();
        row.energy      = q.value(13).toInt();
        row.color       = q.value(14).toString();
        row.notes       = q.value(15).toString();
        row.playCount   = q.value(16).toInt();
        row.lastPlayed  = q.value(17).toLongLong();
        row.dateAdded   = q.value(18).toLongLong();
        m_rows.append(std::move(row));
    }
    endResetModel();

    emit countChanged();
    qDebug() << "[LibraryTableModel] refresh() loaded" << m_rows.size() << "rows in" << timer.elapsed() << "ms";
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
    if (trackId.isEmpty()) return;

    int rowIndex = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id == trackId) { rowIndex = i; break; }
    }
    if (rowIndex < 0) return;

    auto db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return;

    QSqlQuery q(db);
    q.prepare(
        "SELECT COALESCE(t.rating,0), COALESCE(t.energy,0), COALESCE(t.color,''),"
        "       COALESCE(t.notes,''), COALESCE(t.play_count,0), COALESCE(t.last_played,0)"
        " FROM Tracks t WHERE t.id = :id LIMIT 1");
    q.bindValue(":id", trackId);
    if (!q.exec() || !q.next()) return;

    auto& row = m_rows[rowIndex];
    row.rating    = q.value(0).toInt();
    row.energy    = q.value(1).toInt();
    row.color     = q.value(2).toString();
    row.notes     = q.value(3).toString();
    row.playCount = q.value(4).toInt();
    row.lastPlayed= q.value(5).toLongLong();

    const QModelIndex left  = index(rowIndex, 12);
    const QModelIndex right = index(rowIndex, 17);
    emit dataChanged(left, right, { Qt::DisplayRole, RatingRole, EnergyRole,
                                    ColorRole, NotesRole, PlayCountRole, LastPlayedRole });
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
