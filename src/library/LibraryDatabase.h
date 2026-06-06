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
        int analysisVersion = 0;
        TrackData::ConfidenceInfo confidence;
        TrackData::BeatGridInfo beatGridInfo;
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
                              int bitrateKbps = 0,
                              const QString& genre = {},
                              const QString& album = {},
                              const QString& comment = {},
                              qint64 dateAdded = 0);

    // Called by the analyzer when BPM / key detection finishes.
    Q_INVOKABLE void updateAnalysisData(const QString& trackId,
                                        float newBpm,
                                        const QString& newKey,
                                        qint64 firstBeatSample = 0,
                                        double sampleRate = 44100.0,
                                        const std::vector<TrackData::BeatMarker>& beatGrid = {},
                                        TrackData::ConfidenceInfo confidence = {},
                                        TrackData::BeatGridInfo beatGridInfo = {});

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

    // Saved loop persistence (8 slots per track, index 0..7).
    Q_INVOKABLE bool upsertSavedLoop(const QString& trackId,
                                     int loopIndex,
                                     double inSec,
                                     double outSec,
                                     const QString& label,
                                     const QString& colorHex);
    Q_INVOKABLE bool deleteSavedLoop(const QString& trackId, int loopIndex);
    Q_INVOKABLE QVariantList savedLoopsForTrack(const QString& trackId) const;

    // Main CUE persistence (single point per track, seconds, <0 means unset).
    Q_INVOKABLE bool upsertMainCuePoint(const QString& trackId, double positionSec);
    Q_INVOKABLE double mainCuePointForTrack(const QString& trackId) const;

    // Check whether a track is already in the database.
    Q_INVOKABLE bool trackExists(const QString& trackId) const;

    // Retrieve the file_path for a given trackId (first location).
    Q_INVOKABLE QString filePath(const QString& trackId) const;

    // Retrieve the track_id for a given file path (returns empty string if not found).
    Q_INVOKABLE QString trackIdForFilePath(const QString& filePath) const;

    // ── Per-track user metadata ────────────────────────────────────────────
    Q_INVOKABLE bool setTrackRating(const QString& trackId, int rating);   // 0–5
    Q_INVOKABLE bool setTrackEnergy(const QString& trackId, int energy);   // 0–5
    Q_INVOKABLE bool setTrackColor(const QString& trackId, const QString& colorHex);
    Q_INVOKABLE bool setTrackNotes(const QString& trackId, const QString& notes);
    // Returns map with: rating, energy, color, notes, genre, album, comment, playCount, lastPlayed, dateAdded
    Q_INVOKABLE QVariantMap getTrackMeta(const QString& trackId) const;

    // ── Tag system ─────────────────────────────────────────────────────────
    // Returns new tag id, or empty on failure.
    Q_INVOKABLE QString createTag(const QString& name, const QString& colorHex = "#888888");
    Q_INVOKABLE bool deleteTag(const QString& tagId);
    Q_INVOKABLE bool renameTag(const QString& tagId, const QString& newName);
    Q_INVOKABLE bool setTagColor(const QString& tagId, const QString& colorHex);
    // Returns [{id, name, color}, ...]
    Q_INVOKABLE QVariantList getAllTags() const;
    Q_INVOKABLE bool addTagToTrack(const QString& trackId, const QString& tagId);
    Q_INVOKABLE bool removeTagFromTrack(const QString& trackId, const QString& tagId);
    // Returns [{id, name, color}, ...]
    Q_INVOKABLE QVariantList getTagsForTrack(const QString& trackId) const;
    // Returns full track maps (same format as getPlaylistTracks) for tag.
    Q_INVOKABLE QVariantList getTracksForTag(const QString& tagId) const;
    Q_INVOKABLE bool isTagOnTrack(const QString& trackId, const QString& tagId) const;

    // ── Favorites ─────────────────────────────────────────────────────────
    Q_INVOKABLE bool addToFavorites(const QString& trackId);
    Q_INVOKABLE bool removeFromFavorites(const QString& trackId);
    Q_INVOKABLE bool isFavorite(const QString& trackId) const;
    Q_INVOKABLE QVariantList getFavoriteTracks() const;

    // ── Prepare Crate ──────────────────────────────────────────────────────
    Q_INVOKABLE bool addToPrepareCrate(const QString& trackId);
    Q_INVOKABLE bool removeFromPrepareCrate(const QString& trackId);
    Q_INVOKABLE bool clearPrepareCrate();
    Q_INVOKABLE QVariantList getPrepareCrateTracks() const;
    Q_INVOKABLE bool savePrepareCrateAsPlaylist(const QString& name);
    Q_INVOKABLE bool setPrepareCratePosition(const QString& trackId, int position);

    // ── Track Queue ────────────────────────────────────────────────────────
    Q_INVOKABLE bool enqueueTrack(const QString& trackId);
    Q_INVOKABLE bool dequeueTrack(const QString& trackId);
    Q_INVOKABLE bool clearQueue();
    Q_INVOKABLE QVariantList getQueueTracks() const;
    Q_INVOKABLE bool setQueuePosition(const QString& trackId, int position);

    // ── Play History ───────────────────────────────────────────────────────
    Q_INVOKABLE bool logPlay(const QString& trackId);
    // period: "today" | "week" | "month" | "all"
    // Returns one row per real play event:
    // [{historyId, playedAt, trackId, title, artist, bpm, key, playCount, lastPlayed, filePath, ...}, ...]
    Q_INVOKABLE QVariantList getPlayHistory(const QString& period = "all") const;

    // ── Smart Collections ──────────────────────────────────────────────────
    // rulesJson: [{field, op, value}, ...] — see evaluateSmartCollection for ops.
    // Returns new collection id, or empty on failure.
    Q_INVOKABLE QString createSmartCollection(const QString& name, const QString& rulesJson);
    Q_INVOKABLE bool deleteSmartCollection(const QString& id);
    Q_INVOKABLE bool updateSmartCollection(const QString& id, const QString& name, const QString& rulesJson);
    // Returns [{id, name, rulesJson}, ...]
    Q_INVOKABLE QVariantList getAllSmartCollections() const;
    // Evaluate rules and return matching tracks (same map format as getPlaylistTracks).
    Q_INVOKABLE QVariantList evaluateSmartCollection(const QString& rulesJson) const;

    // ── Playlist management ────────────────────────────────────────────────
    // Returns new playlist id, or empty string on failure.
    Q_INVOKABLE QString createPlaylist(const QString& name,
                                       const QString& parentId = QString());
    Q_INVOKABLE bool deletePlaylist(const QString& playlistId);
    Q_INVOKABLE bool renamePlaylist(const QString& playlistId, const QString& newName);
    // Update the drag-reorder sort position of a playlist.
    Q_INVOKABLE bool setPlaylistSortOrder(const QString& playlistId, int sortOrder);

    // Returns list of {id, name, parentId, sortOrder, trackCount} maps.
    Q_INVOKABLE QVariantList getAllPlaylists() const;

    Q_INVOKABLE bool addTrackToPlaylist(const QString& playlistId, const QString& trackId);
    Q_INVOKABLE bool removeTrackFromPlaylist(const QString& playlistId, const QString& trackId);
    Q_INVOKABLE bool setPlaylistTrackPosition(const QString& playlistId, const QString& trackId, int newPosition);
    // Returns list of full track maps {trackId, title, artist, durationSec, bpm, key,
    // bitrateKbps, isAnalyzed, filePath, genre, album, rating, energy, color, notes, playCount} ordered by position.
    Q_INVOKABLE QVariantList getPlaylistTracks(const QString& playlistId) const;
    Q_INVOKABLE QVariantList getAllTrackAnalysisItems(bool includeAnalyzed = false) const;
    Q_INVOKABLE QVariantList getPlaylistAnalysisItems(const QString& playlistId,
                                                      bool includeAnalyzed = false) const;
    Q_INVOKABLE bool isTrackInPlaylist(const QString& playlistId, const QString& trackId) const;
    Q_INVOKABLE int getPlaylistTrackCount(const QString& playlistId) const;
    // Move a playlist to a new parent (empty string = top level).
    Q_INVOKABLE bool setPlaylistParent(const QString& playlistId, const QString& newParentId);

    // ── Generic settings (stored in Meta table) ────────────────────────────
    Q_INVOKABLE QString getSetting(const QString& key, const QString& defaultValue = {}) const;
    Q_INVOKABLE bool    setSetting(const QString& key, const QString& value);

    // ── Remove track from library (DB + waveform cache) ────────────────────
    Q_INVOKABLE bool removeTrackFromLibrary(const QString& trackId);

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
    void playlistsChanged();
    void trackRemovedFromLibrary(const QString& trackId);
    void trackMetaChanged(const QString& trackId);
    void favoritesChanged();
    void crateChanged();
    void queueChanged();
    void historyChanged();
    void tagsChanged();
    void smartCollectionsChanged();

private:
    bool createSchema();
    void scheduleTableModelRefresh();
    void scheduleBackupSync();
    void startDeferredBackupSync();
    bool isHealthyDatabaseFile(const QString& path) const;
    bool recreateDatabaseFileFromLiveConnection(const QString& targetPath);
    bool reopenDatabaseConnection();
    void performMirrorSelfCheck();
    bool restorePrimaryFromBackup();
    bool syncBackupFromPrimary();
    bool copyDatabaseFile(const QString& sourcePath, const QString& targetPath) const;
    void clearDatabaseConnection();

    // Helper: build a full track-map from a positioned SELECT row.
    QVariantMap buildTrackMap(const QSqlQuery& q) const;

    QSqlDatabase m_db;
    LibraryTableModel* m_tableModel = nullptr;
    QString m_dbPath;
    QString m_backupDbPath;
    QString m_activeDbPath;
    QString m_manualBackupDbPath;
    QString m_lastRecoveryEvent;
    QString m_cachedMirrorStatus;
    QTimer m_mirrorSelfCheckTimer;
    QTimer m_backupSyncTimer;
    bool m_primaryMirrorDegraded = false;
    bool m_backupMirrorDegraded = false;
    bool m_tableModelRefreshPending = false;
    bool m_backupSyncRunning = false;
    bool m_backupSyncAgain = false;

    static constexpr int kSchemaVersion = 16;
};
