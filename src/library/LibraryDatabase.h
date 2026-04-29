#pragma once

#include <QObject>
#include <QString>
#include <QSqlDatabase>
#include <QTimer>
#include <QVariantList>
#include <vector>

#include "TrackData.h"
#include "TrackSegment.h"

class LibraryTableModel;

class LibraryDatabase : public QObject
{
    Q_OBJECT

public:
    struct AnalysisSnapshot {
        double bpm = 0.0;
        QString key;
        bool isAnalyzed = false;
        qint64 firstBeatSample = 0;
        double sampleRate = 44100.0;
        std::vector<TrackData::BeatMarker> beatGrid;
    };

    explicit LibraryDatabase(QObject* parent = nullptr);
    ~LibraryDatabase() override;

    bool open();

    // Insert or update a track + its file location.
    Q_INVOKABLE bool addTrack(const QString& trackId,
                              const QString& title,
                              const QString& artist,
                              int durationSec,
                              const QString& filePath,
                              int bitrateKbps = 0);

    // Called by the analyzer when BPM / key detection finishes.
    Q_INVOKABLE void updateAnalysisData(const QString& trackId,
                                        float newBpm,
                                        const QString& newKey,
                                        qint64 firstBeatSample = 0,
                                        double sampleRate = 44100.0,
                                        const std::vector<TrackData::BeatMarker>& beatGrid = {});

    bool tryGetAnalysisData(const QString& trackId, AnalysisSnapshot* out) const;

    // Segment JSON helpers for DB storage and QML bridge.
    static QString trackSegmentsToJson(const std::vector<TrackSegment>& segments);
    static QVariantList trackSegmentsJsonToVariantList(const QString& json);

    Q_INVOKABLE bool updateTrackSegments(const QString& trackId,
                                         const std::vector<TrackSegment>& segments);
    Q_INVOKABLE QVariantList trackSegmentsForTrack(const QString& trackId) const;

    // Hotcue persistence (8 slots per track, index 0..7).
    Q_INVOKABLE bool upsertCuePoint(const QString& trackId,
                                    int cueIndex,
                                    double positionSec,
                                    const QString& label,
                                    const QString& colorHex);
    Q_INVOKABLE bool deleteCuePoint(const QString& trackId, int cueIndex);
    Q_INVOKABLE QVariantList cuePointsForTrack(const QString& trackId) const;

    // Main CUE persistence (single point per track, seconds, <0 means unset).
    Q_INVOKABLE bool upsertMainCuePoint(const QString& trackId, double positionSec);
    Q_INVOKABLE double mainCuePointForTrack(const QString& trackId) const;

    // Check whether a track is already in the database.
    Q_INVOKABLE bool trackExists(const QString& trackId) const;

    // Retrieve the file_path for a given trackId (first location).
    Q_INVOKABLE QString filePath(const QString& trackId) const;

    // Human-readable mirrored database status for the exit dialog.
    Q_PROPERTY(QString mirroredDatabaseStatus READ mirroredDatabaseStatus NOTIFY mirroredDatabaseStatusChanged)
    Q_INVOKABLE QString mirroredDatabaseStatus() const;

    // Flush pending DB work and close the connection for clean shutdown.
    Q_INVOKABLE void shutdown(bool syncBackup = false);

    // Wire up the table model so it auto-refreshes after mutations.
    void setTableModel(LibraryTableModel* model);

signals:
    void trackAdded(const QString& trackId);
    void analysisUpdated(const QString& trackId);
    void mirroredDatabaseStatusChanged();

private:
    bool createSchema();
    void scheduleTableModelRefresh();
    bool isHealthyDatabaseFile(const QString& path) const;
    bool recreateDatabaseFileFromLiveConnection(const QString& targetPath);
    bool reopenDatabaseConnection();
    void performMirrorSelfCheck();
    bool restorePrimaryFromBackup();
    bool syncBackupFromPrimary();
    bool copyDatabaseFile(const QString& sourcePath, const QString& targetPath) const;
    void clearDatabaseConnection();

    QSqlDatabase m_db;
    LibraryTableModel* m_tableModel = nullptr;
    QString m_dbPath;
    QString m_backupDbPath;
    QString m_activeDbPath;
    QString m_manualBackupDbPath;
    QString m_lastRecoveryEvent;
    QString m_cachedMirrorStatus;
    QTimer m_mirrorSelfCheckTimer;
    bool m_primaryMirrorDegraded = false;
    bool m_backupMirrorDegraded = false;
    bool m_tableModelRefreshPending = false;

    static constexpr int kSchemaVersion = 6;
};
