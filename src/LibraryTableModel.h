#pragma once

#include <QAbstractTableModel>
#include <QSqlDatabase>
#include <QVector>

// Row cache: one entry per track returned by the JOIN query.
struct LibraryRow {
    QString id;
    QString title;
    QString artist;
    int     durationSec = 0;
    double  bpm        = 0.0;
    QString key;
    int     bitrateKbps = 0;
    bool    isAnalyzed = false;
    QString filePath;
};

class LibraryTableModel : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        DurationRole,
        BpmRole,
        KeyRole,
        BitrateRole,
        AnalyzedRole,
        FilePathRole
    };

    explicit LibraryTableModel(const QString& connectionName,
                               QObject* parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Re-run the SELECT and reset the model.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void toggleSort(const QString& field);
    Q_INVOKABLE void setSort(const QString& field, bool ascending);

    QString sortField() const { return m_sortField; }
    bool sortAscending() const { return m_sortAscending; }

signals:
    void countChanged();
    void sortChanged();

private:
    QString sortColumnSql() const;

    QString m_connectionName;
    QVector<LibraryRow> m_rows;
    QString m_sortField = "artist";
    bool m_sortAscending = true;
};
