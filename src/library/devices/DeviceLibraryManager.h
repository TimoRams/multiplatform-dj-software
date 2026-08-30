#pragma once

#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class QStorageInfo;

class DeviceLibraryManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList currentTracks READ currentTracks NOTIFY currentTracksChanged)
    Q_PROPERTY(QVariantList currentPlaylists READ currentPlaylists NOTIFY currentNavigationChanged)
    Q_PROPERTY(QVariantList currentArtists READ currentArtists NOTIFY currentNavigationChanged)
    Q_PROPERTY(QVariantList currentAlbums READ currentAlbums NOTIFY currentNavigationChanged)
    Q_PROPERTY(QVariantList currentGenres READ currentGenres NOTIFY currentNavigationChanged)
    Q_PROPERTY(QVariantList currentFolders READ currentFolders NOTIFY currentNavigationChanged)
    Q_PROPERTY(QString selectedDeviceId READ selectedDeviceId NOTIFY selectedDeviceChanged)
    Q_PROPERTY(QString selectedDeviceName READ selectedDeviceName NOTIFY selectedDeviceChanged)
    Q_PROPERTY(bool selectedDeviceReady READ selectedDeviceReady NOTIFY selectedDeviceChanged)
    Q_PROPERTY(QString selectedViewName READ selectedViewName NOTIFY currentNavigationChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit DeviceLibraryManager(QObject* parent = nullptr);
    DeviceLibraryManager(bool automaticDiscovery, QObject* parent);
    ~DeviceLibraryManager() override;

    DeviceLibraryManager(const DeviceLibraryManager&) = delete;
    DeviceLibraryManager& operator=(const DeviceLibraryManager&) = delete;

    [[nodiscard]] QVariantList devices() const { return m_devicesView; }
    [[nodiscard]] QVariantList currentTracks() const { return m_currentTracks; }
    [[nodiscard]] QVariantList currentPlaylists() const;
    [[nodiscard]] QVariantList currentArtists() const;
    [[nodiscard]] QVariantList currentAlbums() const;
    [[nodiscard]] QVariantList currentGenres() const;
    [[nodiscard]] QVariantList currentFolders() const;
    [[nodiscard]] QString selectedDeviceId() const { return m_selectedDeviceId; }
    [[nodiscard]] QString selectedDeviceName() const;
    [[nodiscard]] bool selectedDeviceReady() const;
    [[nodiscard]] QString selectedViewName() const { return m_selectedViewName; }
    [[nodiscard]] QString filterText() const { return m_filterText; }
    [[nodiscard]] QString sortField() const { return m_sortField; }
    [[nodiscard]] bool sortAscending() const { return m_sortAscending; }
    [[nodiscard]] bool busy() const { return m_busy; }
    [[nodiscard]] QString statusMessage() const { return m_statusMessage; }

    Q_INVOKABLE void rescanNow();
    Q_INVOKABLE void mountDevice(const QString& deviceId);
    Q_INVOKABLE void ejectDevice(const QString& deviceId);
    Q_INVOKABLE void chooseDevice(const QString& deviceId);
    Q_INVOKABLE void chooseTracks();
    Q_INVOKABLE void choosePlaylists();
    Q_INVOKABLE void choosePlaylist(const QString& playlistId);
    Q_INVOKABLE void chooseCategory(const QString& category, const QString& value);
    Q_INVOKABLE void setFilterText(const QString& text);
    Q_INVOKABLE void setSort(const QString& field, bool ascending);
    Q_INVOKABLE void toggleSort(const QString& field);
    Q_INVOKABLE void requestDeckLoad(const QString& sourceAwareTrackId,
                                     const QString& deckLetter);

    // Deterministic volume snapshots for lifecycle tests. These paths are only
    // inspected; the method performs no filesystem mutations.
    void inspectTestMounts(const QStringList& mountPaths);
    void inspectTestSystemDevices(const QVariantList& devices);

signals:
    void devicesChanged();
    void currentTracksChanged();
    void currentNavigationChanged();
    void selectedDeviceChanged();
    void filterTextChanged();
    void sortChanged();
    void busyChanged();
    void statusMessageChanged();
    void deviceRemoved(const QString& deviceId);
    void deviceEjectRequested(const QString& deviceId);
    void deckLoadReady(const QString& deckLetter, const QVariantMap& request);
    void deckLoadFailed(const QString& deckLetter, const QString& message);

private:
    struct DeviceState;
    struct SystemVolume;
    struct EjectOperation;
    struct WorkerTask {
        enum class Kind { Index, Analysis } kind = Kind::Index;
        quint64 generation = 0;
        QString deviceId;
        QString mountPath;
        QString deckLetter;
        QString trackId;
        std::shared_ptr<const rekordbox::DeviceIndex> index;
    };

    void inspectStorageVolumes(const QList<QStorageInfo>& volumes);
    void inspectMountPaths(const QStringList& paths);
    void refreshSystemVolumes();
    void applySystemVolumes(QVector<SystemVolume> volumes);
    void continueEject(const std::shared_ptr<EjectOperation>& operation);
    void finishDeviceOperation(const QStringList& deviceIds, bool success,
                               const QString& message);
    void publishDevices();
    void queueIndex(const QString& deviceId);
    void workerLoop();
    void applyIndexResult(QString deviceId, quint64 generation,
                          rekordbox::DeviceSource::Result result);
    void applyAnalysisResult(QString deviceId, quint64 generation,
                             QString deckLetter, rekordbox::Track track,
                             std::shared_ptr<const rekordbox::DeviceIndex> sourceIndex,
                             rekordbox::AnalysisReader::Result result);
    void rebuildCurrentTracks();
    void setBusy(bool value);
    void setStatusMessage(QString value);
    [[nodiscard]] std::shared_ptr<const rekordbox::DeviceIndex> selectedIndex() const;
    [[nodiscard]] QVariantMap trackMap(const rekordbox::Track& track) const;

    QHash<QString, DeviceState> m_devices;
    QStringList m_deviceOrder;
    QVariantList m_devicesView;
    QVariantList m_currentTracks;
    QString m_selectedDeviceId;
    QString m_selectedViewName = QStringLiteral("Devices");
    QString m_filterText;
    QString m_sortField = QStringLiteral("title");
    bool m_sortAscending = true;
    bool m_busy = false;
    QString m_statusMessage;
    QVector<quint32> m_viewTrackIds;
    bool m_preservePlaylistOrder = false;
    quint64 m_generation = 0;
    QTimer m_pollTimer;
    bool m_systemRefreshPending = false;
    bool m_systemRefreshAgain = false;

    std::mutex m_workerMutex;
    std::condition_variable m_workerCondition;
    std::deque<WorkerTask> m_tasks;
    bool m_workerStopping = false;
    std::thread m_worker;
};
