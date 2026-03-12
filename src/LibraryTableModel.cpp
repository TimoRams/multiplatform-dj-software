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
    return 7; // id, title, artist, bpm, key, isAnalyzed, filePath
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
    case BpmRole:       return row.bpm;
    case KeyRole:       return row.key;
    case AnalyzedRole:  return row.isAnalyzed;
    case FilePathRole:  return row.filePath;

    // Qt::DisplayRole: column-based fallback for TableView delegates.
    case Qt::DisplayRole:
        switch (index.column()) {
        case 0: return row.id;
        case 1: return row.title;
        case 2: return row.artist;
        case 3: return row.bpm;
        case 4: return row.key;
        case 5: return row.isAnalyzed;
        case 6: return row.filePath;
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
        { BpmRole,      "bpm"        },
        { KeyRole,      "key"        },
        { AnalyzedRole, "isAnalyzed" },
        { FilePathRole, "filePath"   }
    };
}

void LibraryTableModel::refresh()
{
    auto db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        qWarning() << "[LibraryTableModel] DB not open for refresh";
        return;
    }

    QSqlQuery q(db);
    q.prepare(
        "SELECT Tracks.id, title, artist, bpm, key, is_analyzed, Locations.file_path "
        "FROM Tracks "
        "JOIN Locations ON Tracks.id = Locations.track_id "
        "ORDER BY artist, title");

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
        row.bpm        = q.value(3).toDouble();
        row.key        = q.value(4).toString();
        row.isAnalyzed = q.value(5).toBool();
        row.filePath   = q.value(6).toString();
        m_rows.append(std::move(row));
    }
    endResetModel();

    emit countChanged();
}
