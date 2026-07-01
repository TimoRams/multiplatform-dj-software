#pragma once

#include <QAbstractTableModel>
#include <QSqlDatabase>
#include <QVector>

struct LibraryRow {
    QString id;
    QString title;
    QString artist;
    int     durationSec  = 0;
    double  bpm          = 0.0;
    QString key;
    int     bitrateKbps  = 0;
    bool    isAnalyzed   = false;
    QString filePath;
    QString genre;
    QString album;
    QString comment;
    int     rating       = 0;
    int     energy       = 0;
    QString color;
    QString notes;
    int     playCount    = 0;
    qint64  lastPlayed   = 0;
    qint64  dateAdded    = 0;
};

class LibraryTableModel : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(int     count           READ rowCount       NOTIFY countChanged)
    Q_PROPERTY(QString sortField       READ sortField      NOTIFY sortChanged)
    Q_PROPERTY(bool    sortAscending   READ sortAscending  NOTIFY sortChanged)
    Q_PROPERTY(QString filterText      READ filterText     WRITE setFilterText    NOTIFY filterTextChanged)
    Q_PROPERTY(double  filterBpmMin    READ filterBpmMin   WRITE setFilterBpmMin  NOTIFY filtersChanged)
    Q_PROPERTY(double  filterBpmMax    READ filterBpmMax   WRITE setFilterBpmMax  NOTIFY filtersChanged)
    Q_PROPERTY(QString filterKey       READ filterKey      WRITE setFilterKey     NOTIFY filtersChanged)
    Q_PROPERTY(QString filterArtist     READ filterArtist   WRITE setFilterArtist  NOTIFY filtersChanged)
    Q_PROPERTY(QString filterAlbum      READ filterAlbum    WRITE setFilterAlbum   NOTIFY filtersChanged)
    Q_PROPERTY(QString filterSourcePath READ filterSourcePath WRITE setFilterSourcePath NOTIFY filtersChanged)
    Q_PROPERTY(QString filterGenre     READ filterGenre    WRITE setFilterGenre   NOTIFY filtersChanged)
    Q_PROPERTY(int     filterRatingMin READ filterRatingMin WRITE setFilterRatingMin NOTIFY filtersChanged)
    Q_PROPERTY(int     filterEnergyMin READ filterEnergyMin WRITE setFilterEnergyMin NOTIFY filtersChanged)

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
        FilePathRole,
        GenreRole,
        AlbumRole,
        CommentRole,
        RatingRole,
        EnergyRole,
        ColorRole,
        NotesRole,
        PlayCountRole,
        LastPlayedRole,
        DateAddedRole,
    };

    explicit LibraryTableModel(const QString& connectionName,
                               QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();
    void updateAnalysisForTrack(const QString& trackId,
                                double bpm,
                                const QString& key,
                                bool isAnalyzed);
    void refreshMetaForTrack(const QString& trackId);
    Q_INVOKABLE void toggleSort(const QString& field);
    Q_INVOKABLE void setSort(const QString& field, bool ascending);
    Q_INVOKABLE void setFilterText(const QString& text);
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void setFilterKeys(const QStringList& keys);
    Q_INVOKABLE void setFilterBpmMin(double v);
    Q_INVOKABLE void setFilterBpmMax(double v);
    Q_INVOKABLE void setFilterKey(const QString& v);
    Q_INVOKABLE void setFilterArtist(const QString& v);
    Q_INVOKABLE void setFilterAlbum(const QString& v);
    Q_INVOKABLE void setFilterSourcePath(const QString& v);
    Q_INVOKABLE void setFilterGenre(const QString& v);
    Q_INVOKABLE void setFilterRatingMin(int v);
    Q_INVOKABLE void setFilterEnergyMin(int v);
    Q_INVOKABLE void applyAioBrowseFilter(const QString& field, const QString& value,
                                          const QStringList& keys = {});
    Q_INVOKABLE QString filePathAtRow(int row) const;
    Q_INVOKABLE QString trackIdAtRow(int row) const;
    Q_INVOKABLE int indexOfTrackId(const QString& trackId) const;

    QString sortField()    const { return m_sortField; }
    bool sortAscending()   const { return m_sortAscending; }
    QString filterText()   const { return m_filterText; }
    double  filterBpmMin() const { return m_filterBpmMin; }
    double  filterBpmMax() const { return m_filterBpmMax; }
    QString filterKey()    const { return m_filterKey; }
    QString filterArtist() const { return m_filterArtist; }
    QString filterAlbum()  const { return m_filterAlbum; }
    QString filterSourcePath() const { return m_filterSourcePath; }
    QString filterGenre()  const { return m_filterGenre; }
    int filterRatingMin()  const { return m_filterRatingMin; }
    int filterEnergyMin()  const { return m_filterEnergyMin; }

signals:
    void countChanged();
    void sortChanged();
    void filterTextChanged();
    void filtersChanged();

private:
    QString sortColumnSql() const;
    bool aioBrowseFiltersEqual(const QString& field, const QString& value,
                               const QStringList& keys) const;
    void clearAioBrowseFilterFields();

    QString m_connectionName;
    QVector<LibraryRow> m_rows;
    QString m_sortField     = "artist";
    bool    m_sortAscending = true;
    QString m_filterText;
    double  m_filterBpmMin  = 0.0;
    double  m_filterBpmMax  = 0.0;
    QString m_filterKey;
    QString m_filterArtist;
    QString m_filterAlbum;
    QString m_filterSourcePath;
    QStringList m_filterKeys;
    QString m_filterGenre;
    int     m_filterRatingMin = 0;
    int     m_filterEnergyMin = 0;
};
