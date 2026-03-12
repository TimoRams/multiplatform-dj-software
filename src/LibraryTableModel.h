#pragma once

#include <QAbstractTableModel>
#include <QSqlDatabase>
#include <QVector>

// Row cache: one entry per track returned by the JOIN query.
struct LibraryRow {
    QString id;
    QString title;
    QString artist;
    double  bpm        = 0.0;
    QString key;
    bool    isAnalyzed = false;
    QString filePath;
};

class LibraryTableModel : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        BpmRole,
        KeyRole,
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

signals:
    void countChanged();

private:
    QString m_connectionName;
    QVector<LibraryRow> m_rows;
};
