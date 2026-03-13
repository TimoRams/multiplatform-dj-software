#include "LibraryTableModel.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>

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
    return 9; // id, title, artist, duration, bpm, key, kbps, isAnalyzed, filePath
}

QVariant LibraryTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};

    const auto& row = m_rows[index.row()];

    switch (role) {
    case IdRole:        return row.id;
    case TitleRole:     return row.title;
    case ArtistRole:    return row.artist;
    case DurationRole:  return row.durationSec;
    case BpmRole:       return row.bpm;
    case KeyRole:       return row.key;
    case BitrateRole:   return row.bitrateKbps;
    case AnalyzedRole:  return row.isAnalyzed;
    case FilePathRole:  return row.filePath;

    // Qt::DisplayRole: column-based fallback for TableView delegates.
    case Qt::DisplayRole:
        switch (index.column()) {
        case 0: return row.id;
        case 1: return row.title;
        case 2: return row.artist;
        case 3: return row.durationSec;
        case 4: return row.bpm;
        case 5: return row.key;
        case 6: return row.bitrateKbps;
        case 7: return row.isAnalyzed;
        case 8: return row.filePath;
        }
        break;
    }

    return {};
}

QHash<int, QByteArray> LibraryTableModel::roleNames() const
{
    return {
        { IdRole,       "trackId"    },
        { TitleRole,    "title"      },
        { ArtistRole,   "artist"     },
        { DurationRole, "durationSec" },
        { BpmRole,      "bpm"        },
        { KeyRole,      "key"        },
        { BitrateRole,  "bitrateKbps" },
        { AnalyzedRole, "isAnalyzed" },
        { FilePathRole, "filePath"   }
    };
}

QString LibraryTableModel::sortColumnSql() const
{
    if (m_sortField == "title")
        return "LOWER(title)";
    if (m_sortField == "artist")
        return "LOWER(artist)";
    if (m_sortField == "time" || m_sortField == "duration")
        return "duration_sec";
    if (m_sortField == "bpm")
        return "bpm";
    if (m_sortField == "key")
        return "LOWER(key)";
    if (m_sortField == "kbps" || m_sortField == "bitrate")
        return "bitrate_kbps";
    return "LOWER(artist)";
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

void LibraryTableModel::refresh()
{
    auto db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        qWarning() << "[LibraryTableModel] DB not open for refresh";
        return;
    }

    QSqlQuery q(db);
    const QString sortDir = m_sortAscending ? "ASC" : "DESC";
    const QString query = QString(
        "SELECT Tracks.id, title, artist, duration_sec, bpm, key, bitrate_kbps, is_analyzed, Locations.file_path "
        "FROM Tracks "
        "JOIN Locations ON Tracks.id = Locations.track_id "
        "ORDER BY %1 %2, LOWER(title) ASC")
        .arg(sortColumnSql(), sortDir);

    q.prepare(query);

    if (!q.exec()) {
        qWarning() << "[LibraryTableModel] refresh query failed:" << q.lastError().text();
        return;
    }

    beginResetModel();
    m_rows.clear();
    while (q.next()) {
        LibraryRow row;
        row.id         = q.value(0).toString();
        row.title      = q.value(1).toString();
        row.artist     = q.value(2).toString();
        row.durationSec = q.value(3).toInt();
        row.bpm        = q.value(4).toDouble();
        row.key        = q.value(5).toString();
        row.bitrateKbps = q.value(6).toInt();
        row.isAnalyzed = q.value(7).toBool();
        row.filePath   = q.value(8).toString();
        m_rows.append(std::move(row));
    }
    endResetModel();

    emit countChanged();
}
