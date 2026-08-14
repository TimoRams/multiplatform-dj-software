#include "library/devices/DeviceLibraryManager.h"

#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QStorageInfo>

#include <algorithm>
#include <cmath>
#include <functional>
#include <QSet>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

QString stableDeviceId(const QByteArray& device, const QString& name,
                       const QByteArray& fileSystemType, const QString& fallbackRoot)
{
    QByteArray identity = device;
    identity += '\0';
    identity += name.toUtf8();
    identity += '\0';
    identity += fileSystemType;
    if (device.isEmpty()) {
        identity += '\0';
        identity += QDir::cleanPath(fallbackRoot).toUtf8();
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(20));
}

bool hasFile(const QString& root, const QString& relative)
{
    const QFileInfo info(QDir(root).filePath(relative));
    return info.exists() && info.isFile() && info.isReadable();
}

bool likelyRemovableMount(const QString& root)
{
#ifdef Q_OS_LINUX
    return root.startsWith(QStringLiteral("/run/media/"))
        || root.startsWith(QStringLiteral("/media/"))
        || root.startsWith(QStringLiteral("/mnt/"));
#elif defined(Q_OS_MACOS)
    return root.startsWith(QStringLiteral("/Volumes/"));
#elif defined(Q_OS_WIN)
    return root.size() >= 3 && root.at(1) == QLatin1Char(':');
#else
    return root != QDir::rootPath();
#endif
}

void lowerDeviceWorkerPriority()
{
#ifdef __linux__
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 15);
#ifdef SYS_ioprio_set
    constexpr int kWhoProcess = 1;
    constexpr int kIdleClass = 3;
    syscall(SYS_ioprio_set, kWhoProcess, 0, kIdleClass << 13);
#endif
#endif
}

QString normalized(const QString& value)
{
    return value.trimmed().toCaseFolded();
}

QVariantList aggregateValues(const rekordbox::DeviceIndex& index,
                             const std::function<QString(const rekordbox::Track&)>& getter)
{
    QHash<QString, QPair<QString, int>> counts;
    for (const auto& track : index.tracks) {
        const QString display = getter(track).trimmed();
        const QString key = display.toCaseFolded();
        auto& entry = counts[key];
        if (entry.first.isEmpty())
            entry.first = display.isEmpty() ? QStringLiteral("(Unknown)") : display;
        ++entry.second;
    }
    QVariantList result;
    result.reserve(counts.size());
    for (const auto& entry : counts.asKeyValueRange()) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), entry.second.first);
        item.insert(QStringLiteral("trackCount"), entry.second.second);
        result.append(item);
    }
    std::sort(result.begin(), result.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("name")).toString().localeAwareCompare(
                   b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    return result;
}

} // namespace

struct DeviceLibraryManager::DeviceState {
    QString id;
    QString name;
    QString mountPath;
    QString fileSystemType;
    bool ready = false;
    bool volumeReadOnly = false;
    bool scanning = false;
    quint64 scanGeneration = 0;
    rekordbox::DeviceLibraryKind kind = rekordbox::DeviceLibraryKind::GenericUsb;
    std::shared_ptr<const rekordbox::DeviceIndex> index;
    QString status;
};

struct DeviceLibraryManager::WorkerTask {
    enum class Kind { Index, Analysis } kind = Kind::Index;
    quint64 generation = 0;
    QString deviceId;
    QString mountPath;
    QString deckLetter;
    QString trackId;
    std::shared_ptr<const rekordbox::DeviceIndex> index;
};

DeviceLibraryManager::DeviceLibraryManager(QObject* parent)
    : DeviceLibraryManager(true, parent)
{
}

DeviceLibraryManager::DeviceLibraryManager(bool automaticDiscovery, QObject* parent)
    : QObject(parent)
{
    m_worker = std::thread([this] { workerLoop(); });
    m_pollTimer.setInterval(2000);
    m_pollTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &DeviceLibraryManager::rescanNow);
    if (automaticDiscovery) {
        m_pollTimer.start();
        QTimer::singleShot(0, this, &DeviceLibraryManager::rescanNow);
    }
}

DeviceLibraryManager::~DeviceLibraryManager()
{
    m_pollTimer.stop();
    {
        std::lock_guard lock(m_workerMutex);
        m_workerStopping = true;
        m_tasks.clear();
    }
    m_workerCondition.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void DeviceLibraryManager::rescanNow()
{
    inspectStorageVolumes(QStorageInfo::mountedVolumes());
}

void DeviceLibraryManager::inspectTestMounts(const QStringList& mountPaths)
{
    inspectMountPaths(mountPaths);
}

void DeviceLibraryManager::inspectStorageVolumes(const QList<QStorageInfo>& volumes)
{
    QStringList roots;
    for (const QStorageInfo& storage : volumes) {
        if (!storage.isValid() || !storage.isReady())
            continue;
        const QString root = QDir::cleanPath(storage.rootPath());
        const bool rekordboxSignature = hasFile(
            root, QStringLiteral("PIONEER/rekordbox/export.pdb"))
            || hasFile(root, QStringLiteral("PIONEER/rekordbox/exportLibrary.db"));
        if (!rekordboxSignature && !likelyRemovableMount(root))
            continue;
        roots.append(root);

        const QString id = stableDeviceId(storage.device(), storage.displayName(),
                                          storage.fileSystemType(), root);
        DeviceState state;
        if (m_devices.contains(id))
            state = m_devices.value(id);
        state.id = id;
        state.name = storage.displayName().trimmed().isEmpty()
            ? QFileInfo(root).fileName() : storage.displayName().trimmed();
        state.mountPath = root;
        state.fileSystemType = QString::fromLatin1(storage.fileSystemType());
        state.ready = true;
        state.volumeReadOnly = storage.isReadOnly();
        const bool legacy = hasFile(root, QStringLiteral("PIONEER/rekordbox/export.pdb"));
        const bool plus = hasFile(root, QStringLiteral("PIONEER/rekordbox/exportLibrary.db"));
        state.kind = legacy ? rekordbox::DeviceLibraryKind::LegacyPdb
                            : (plus ? rekordbox::DeviceLibraryKind::DeviceLibraryPlus
                                    : rekordbox::DeviceLibraryKind::GenericUsb);
        if (state.kind == rekordbox::DeviceLibraryKind::DeviceLibraryPlus)
            state.status = QStringLiteral("Device Library Plus detected — not yet supported");
        m_devices.insert(id, state);
        if (!m_deviceOrder.contains(id))
            m_deviceOrder.append(id);
    }

    QSet<QString> present;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        if (roots.contains(it->mountPath))
            present.insert(it.key());
    }
    QStringList removed;
    for (const QString& id : std::as_const(m_deviceOrder)) {
        if (!present.contains(id))
            removed.append(id);
    }
    for (const QString& id : removed) {
        m_devices.remove(id);
        m_deviceOrder.removeAll(id);
        emit deviceRemoved(id);
        if (m_selectedDeviceId == id) {
            m_selectedDeviceId.clear();
            m_viewTrackIds.clear();
            m_currentTracks.clear();
            m_selectedViewName = QStringLiteral("Devices");
            setStatusMessage(QStringLiteral("Device removed"));
            emit selectedDeviceChanged();
            emit currentNavigationChanged();
            emit currentTracksChanged();
        }
    }
    ++m_generation;
    for (const QString& id : std::as_const(m_deviceOrder)) {
        const auto& state = m_devices[id];
        if (state.kind == rekordbox::DeviceLibraryKind::LegacyPdb
            && !state.index && !state.scanning) {
            queueIndex(id);
        }
    }
    publishDevices();
}

void DeviceLibraryManager::inspectMountPaths(const QStringList& paths)
{
    QSet<QString> present;
    for (const QString& path : paths) {
        const QFileInfo rootInfo(path);
        const QString root = rootInfo.canonicalFilePath();
        if (root.isEmpty() || !rootInfo.isDir())
            continue;

        const QString name = rootInfo.fileName().isEmpty()
            ? root : rootInfo.fileName();
        const QString id = stableDeviceId({}, name, QByteArrayLiteral("test"), root);
        DeviceState state;
        if (m_devices.contains(id))
            state = m_devices.value(id);
        state.id = id;
        state.name = name;
        state.mountPath = root;
        state.fileSystemType = QStringLiteral("test");
        state.ready = true;
        state.volumeReadOnly = !rootInfo.isWritable();
        const bool legacy = hasFile(root, QStringLiteral("PIONEER/rekordbox/export.pdb"));
        const bool plus = hasFile(root, QStringLiteral("PIONEER/rekordbox/exportLibrary.db"));
        state.kind = legacy ? rekordbox::DeviceLibraryKind::LegacyPdb
                            : (plus ? rekordbox::DeviceLibraryKind::DeviceLibraryPlus
                                    : rekordbox::DeviceLibraryKind::GenericUsb);
        if (state.kind == rekordbox::DeviceLibraryKind::DeviceLibraryPlus)
            state.status = QStringLiteral("Device Library Plus detected — not yet supported");
        m_devices.insert(id, state);
        present.insert(id);
        if (!m_deviceOrder.contains(id))
            m_deviceOrder.append(id);
    }

    QStringList removed;
    for (const QString& id : std::as_const(m_deviceOrder)) {
        if (!present.contains(id))
            removed.append(id);
    }
    for (const QString& id : removed) {
        m_devices.remove(id);
        m_deviceOrder.removeAll(id);
        emit deviceRemoved(id);
        if (m_selectedDeviceId == id) {
            m_selectedDeviceId.clear();
            m_viewTrackIds.clear();
            m_currentTracks.clear();
            m_selectedViewName = QStringLiteral("Devices");
            setStatusMessage(QStringLiteral("Device removed"));
            emit selectedDeviceChanged();
            emit currentNavigationChanged();
            emit currentTracksChanged();
        }
    }
    ++m_generation;
    for (const QString& id : std::as_const(m_deviceOrder)) {
        const auto& state = m_devices[id];
        if (state.kind == rekordbox::DeviceLibraryKind::LegacyPdb
            && !state.index && !state.scanning) {
            queueIndex(id);
        }
    }
    publishDevices();
}

void DeviceLibraryManager::publishDevices()
{
    QVariantList view;
    view.reserve(m_deviceOrder.size());
    for (const QString& id : std::as_const(m_deviceOrder)) {
        const auto found = m_devices.constFind(id);
        if (found == m_devices.cend())
            continue;
        const DeviceState& state = found.value();
        QVariantMap item;
        item.insert(QStringLiteral("id"), state.id);
        item.insert(QStringLiteral("name"), state.name);
        item.insert(QStringLiteral("mountPath"), state.mountPath);
        item.insert(QStringLiteral("fileSystemType"), state.fileSystemType);
        item.insert(QStringLiteral("ready"), state.ready);
        item.insert(QStringLiteral("readOnly"), true);
        item.insert(QStringLiteral("volumeReadOnly"), state.volumeReadOnly);
        item.insert(QStringLiteral("scanning"), state.scanning);
        item.insert(QStringLiteral("libraryType"),
                    state.kind == rekordbox::DeviceLibraryKind::LegacyPdb
                        ? QStringLiteral("rekordboxLegacy")
                        : (state.kind == rekordbox::DeviceLibraryKind::DeviceLibraryPlus
                               ? QStringLiteral("rekordboxDeviceLibraryPlus")
                               : QStringLiteral("genericUsb")));
        item.insert(QStringLiteral("badge"),
                    state.kind == rekordbox::DeviceLibraryKind::GenericUsb
                        ? QStringLiteral("USB") : QStringLiteral("REKORDBOX"));
        item.insert(QStringLiteral("trackCount"), state.index ? state.index->tracks.size() : 0);
        item.insert(QStringLiteral("playlistCount"),
                    state.index ? state.index->playlists.size() : 0);
        item.insert(QStringLiteral("status"), state.status);
        view.append(item);
    }
    if (m_devicesView == view)
        return;
    m_devicesView = std::move(view);
    emit devicesChanged();
}

void DeviceLibraryManager::queueIndex(const QString& deviceId)
{
    auto found = m_devices.find(deviceId);
    if (found == m_devices.end() || found->kind != rekordbox::DeviceLibraryKind::LegacyPdb)
        return;
    found->scanning = true;
    found->status = QStringLiteral("Scanning library…");
    WorkerTask task;
    task.kind = WorkerTask::Kind::Index;
    task.generation = ++m_generation;
    found->scanGeneration = task.generation;
    task.deviceId = deviceId;
    task.mountPath = found->mountPath;
    {
        std::lock_guard lock(m_workerMutex);
        m_tasks.push_back(std::move(task));
    }
    m_workerCondition.notify_one();
    setBusy(true);
}

void DeviceLibraryManager::workerLoop()
{
    lowerDeviceWorkerPriority();
    for (;;) {
        WorkerTask task;
        {
            std::unique_lock lock(m_workerMutex);
            m_workerCondition.wait(lock, [this] {
                return m_workerStopping || !m_tasks.empty();
            });
            if (m_workerStopping)
                return;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }

        QPointer<DeviceLibraryManager> safeThis(this);
        if (task.kind == WorkerTask::Kind::Index) {
            auto result = rekordbox::DeviceSource{}.readIndexReadOnly(
                task.mountPath, task.deviceId);
            QMetaObject::invokeMethod(this,
                [safeThis, deviceId = task.deviceId, generation = task.generation,
                 result = std::move(result)]() mutable {
                    if (safeThis)
                        safeThis->applyIndexResult(std::move(deviceId), generation,
                                                   std::move(result));
                }, Qt::QueuedConnection);
            continue;
        }

        rekordbox::Track track;
        rekordbox::AnalysisReader::Result result;
        if (task.index) {
            const auto found = task.index->trackBySourceAwareId.constFind(task.trackId);
            if (found != task.index->trackBySourceAwareId.cend()) {
                track = task.index->tracks.at(*found);
                if (!track.analysisPath.isEmpty())
                    result = rekordbox::DeviceSource{}.readAnalysisReadOnly(*task.index,
                                                                            task.trackId);
                else
                    result.ok = true;
            } else {
                result.error = QStringLiteral("Rekordbox track disappeared from the device index");
            }
        } else {
            result.error = QStringLiteral("Rekordbox device index is unavailable");
        }
        QMetaObject::invokeMethod(this,
            [safeThis, deviceId = task.deviceId, generation = task.generation,
             deckLetter = task.deckLetter, track = std::move(track),
             sourceIndex = std::move(task.index),
             result = std::move(result)]() mutable {
                if (safeThis)
                    safeThis->applyAnalysisResult(std::move(deviceId), generation,
                                                  std::move(deckLetter), std::move(track),
                                                  std::move(sourceIndex),
                                                  std::move(result));
            }, Qt::QueuedConnection);
    }
}

void DeviceLibraryManager::applyIndexResult(QString deviceId, quint64 generation,
                                             rekordbox::DeviceSource::Result result)
{
    auto found = m_devices.find(deviceId);
    if (found == m_devices.end()
        || found->scanGeneration != generation
        || (result.ok && found->mountPath != result.index.mountPath)) {
        bool anyScanning = false;
        for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it)
            anyScanning = anyScanning || it->scanning;
        setBusy(anyScanning);
        return;
    }
    found->scanning = false;
    if (!result.ok) {
        found->status = result.error;
        if (m_selectedDeviceId == deviceId) {
            m_selectedViewName = found->name;
            m_viewTrackIds.clear();
            m_currentTracks.clear();
            m_preservePlaylistOrder = false;
            setStatusMessage(result.error);
            emit currentNavigationChanged();
            emit currentTracksChanged();
        }
    } else {
        found->index = std::make_shared<const rekordbox::DeviceIndex>(std::move(result.index));
        found->status = QStringLiteral("%1 tracks · %2 playlists")
                            .arg(found->index->tracks.size())
                            .arg(found->index->playlists.size());
        if (m_selectedDeviceId == deviceId) {
            m_selectedViewName = found->name;
            m_viewTrackIds.clear();
            m_currentTracks.clear();
            m_preservePlaylistOrder = false;
            setStatusMessage(QStringLiteral("Choose All Tracks or Playlists"));
            emit currentNavigationChanged();
            emit currentTracksChanged();
        }
    }
    bool anyScanning = false;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it)
        anyScanning = anyScanning || it->scanning;
    setBusy(anyScanning);
    publishDevices();
}

void DeviceLibraryManager::chooseDevice(const QString& deviceId)
{
    const auto found = m_devices.constFind(deviceId);
    if (found == m_devices.cend()) {
        setStatusMessage(QStringLiteral("Device is no longer available"));
        return;
    }
    m_selectedDeviceId = deviceId;
    m_selectedViewName = found->name;
    m_viewTrackIds.clear();
    m_currentTracks.clear();
    m_preservePlaylistOrder = false;
    emit selectedDeviceChanged();
    emit currentNavigationChanged();
    emit currentTracksChanged();
    setStatusMessage(found->index
                         ? QStringLiteral("Choose All Tracks or Playlists")
                         : found->status);
}

std::shared_ptr<const rekordbox::DeviceIndex> DeviceLibraryManager::selectedIndex() const
{
    const auto found = m_devices.constFind(m_selectedDeviceId);
    return found == m_devices.cend() ? nullptr : found->index;
}

void DeviceLibraryManager::chooseTracks()
{
    const auto index = selectedIndex();
    if (!index)
        return;
    m_viewTrackIds.clear();
    m_viewTrackIds.reserve(index->tracks.size());
    for (const auto& track : index->tracks)
        m_viewTrackIds.append(track.id);
    m_selectedViewName = QStringLiteral("All Tracks");
    m_preservePlaylistOrder = false;
    m_sortField = QStringLiteral("title");
    m_sortAscending = true;
    emit sortChanged();
    emit currentNavigationChanged();
    rebuildCurrentTracks();
}

void DeviceLibraryManager::choosePlaylists()
{
    const auto index = selectedIndex();
    if (!index)
        return;
    m_viewTrackIds.clear();
    m_currentTracks.clear();
    m_selectedViewName = QStringLiteral("Playlists");
    m_preservePlaylistOrder = false;
    setStatusMessage(QStringLiteral("%1 playlists").arg(index->playlists.size()));
    emit currentNavigationChanged();
    emit currentTracksChanged();
}

void DeviceLibraryManager::choosePlaylist(const QString& playlistId)
{
    const auto index = selectedIndex();
    bool ok = false;
    const quint32 id = playlistId.toUInt(&ok);
    if (!index || !ok)
        return;
    const auto found = std::find_if(index->playlists.cbegin(), index->playlists.cend(),
                                    [id](const auto& value) { return value.id == id; });
    if (found == index->playlists.cend() || found->folder)
        return;
    m_viewTrackIds = found->trackIds;
    m_selectedViewName = found->name;
    m_preservePlaylistOrder = true;
    m_sortField = QStringLiteral("playlistOrder");
    m_sortAscending = true;
    emit sortChanged();
    emit currentNavigationChanged();
    rebuildCurrentTracks();
}

void DeviceLibraryManager::chooseCategory(const QString& category, const QString& value)
{
    const auto index = selectedIndex();
    if (!index)
        return;
    const QString wanted = normalized(value == QStringLiteral("(Unknown)") ? QString() : value);
    m_viewTrackIds.clear();
    for (const auto& track : index->tracks) {
        QString field;
        if (category == QStringLiteral("artist"))
            field = track.artist;
        else if (category == QStringLiteral("album"))
            field = track.album;
        else if (category == QStringLiteral("genre"))
            field = track.genre;
        else if (category == QStringLiteral("folder"))
            field = QFileInfo(QDir::fromNativeSeparators(track.relativeAudioPath)).path();
        if (normalized(field) == wanted)
            m_viewTrackIds.append(track.id);
    }
    m_selectedViewName = value;
    m_preservePlaylistOrder = false;
    emit currentNavigationChanged();
    rebuildCurrentTracks();
}

void DeviceLibraryManager::setFilterText(const QString& text)
{
    const QString next = text.trimmed();
    if (m_filterText == next)
        return;
    m_filterText = next;
    emit filterTextChanged();
    rebuildCurrentTracks();
}

void DeviceLibraryManager::setSort(const QString& field, bool ascending)
{
    if (m_sortField == field && m_sortAscending == ascending)
        return;
    m_sortField = field;
    m_sortAscending = ascending;
    m_preservePlaylistOrder = field == QStringLiteral("playlistOrder");
    emit sortChanged();
    rebuildCurrentTracks();
}

void DeviceLibraryManager::toggleSort(const QString& field)
{
    setSort(field, m_sortField == field ? !m_sortAscending : true);
}

QVariantMap DeviceLibraryManager::trackMap(const rekordbox::Track& track) const
{
    QVariantMap row;
    row.insert(QStringLiteral("trackId"), track.sourceAwareId);
    row.insert(QStringLiteral("sourceType"), QStringLiteral("rekordboxDevice"));
    row.insert(QStringLiteral("sourceId"), m_selectedDeviceId);
    row.insert(QStringLiteral("title"), track.title);
    row.insert(QStringLiteral("artist"), track.artist);
    row.insert(QStringLiteral("album"), track.album);
    row.insert(QStringLiteral("genre"), track.genre);
    row.insert(QStringLiteral("comment"), track.comment);
    row.insert(QStringLiteral("durationSec"), track.durationSec);
    row.insert(QStringLiteral("bpm"), track.bpm);
    row.insert(QStringLiteral("key"), track.key);
    row.insert(QStringLiteral("bitrateKbps"), track.bitrateKbps);
    row.insert(QStringLiteral("rating"), track.rating);
    row.insert(QStringLiteral("trackColor"), track.color);
    row.insert(QStringLiteral("filePath"), track.audioPath);
    row.insert(QStringLiteral("artworkPath"), track.artworkPath);
    row.insert(QStringLiteral("isAnalyzed"), !track.analysisPath.isEmpty());
    row.insert(QStringLiteral("available"), !track.audioPath.isEmpty());
    return row;
}

void DeviceLibraryManager::rebuildCurrentTracks()
{
    const auto index = selectedIndex();
    if (!index) {
        if (!m_currentTracks.isEmpty()) {
            m_currentTracks.clear();
            emit currentTracksChanged();
        }
        return;
    }
    QVector<const rekordbox::Track*> rows;
    rows.reserve(m_viewTrackIds.size());
    const QString query = normalized(m_filterText);
    for (quint32 id : std::as_const(m_viewTrackIds)) {
        const auto found = index->trackById.constFind(id);
        if (found == index->trackById.cend())
            continue;
        const auto& track = index->tracks.at(*found);
        if (!query.isEmpty()) {
            const QString haystack = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6")
                .arg(track.title, track.artist, track.album, track.genre,
                     track.key, track.comment).toCaseFolded();
            if (!haystack.contains(query))
                continue;
        }
        rows.append(&track);
    }

    if (!m_preservePlaylistOrder) {
        const QString field = m_sortField;
        const bool ascending = m_sortAscending;
        std::stable_sort(rows.begin(), rows.end(), [field, ascending](const auto* a, const auto* b) {
            int comparison = 0;
            if (field == QStringLiteral("bpm"))
                comparison = a->bpm < b->bpm ? -1 : (a->bpm > b->bpm ? 1 : 0);
            else if (field == QStringLiteral("duration") || field == QStringLiteral("time"))
                comparison = a->durationSec < b->durationSec ? -1
                    : (a->durationSec > b->durationSec ? 1 : 0);
            else if (field == QStringLiteral("key"))
                comparison = a->key.localeAwareCompare(b->key);
            else if (field == QStringLiteral("album"))
                comparison = a->album.localeAwareCompare(b->album);
            else if (field == QStringLiteral("genre"))
                comparison = a->genre.localeAwareCompare(b->genre);
            else if (field == QStringLiteral("artist"))
                comparison = a->artist.localeAwareCompare(b->artist);
            else
                comparison = a->title.localeAwareCompare(b->title);
            if (comparison == 0)
                comparison = a->title.localeAwareCompare(b->title);
            return ascending ? comparison < 0 : comparison > 0;
        });
    }
    QVariantList next;
    next.reserve(rows.size());
    for (const auto* track : rows)
        next.append(trackMap(*track));
    m_currentTracks = std::move(next);
    setStatusMessage(QStringLiteral("%1 tracks").arg(m_currentTracks.size()));
    emit currentTracksChanged();
}

QVariantList DeviceLibraryManager::currentPlaylists() const
{
    QVariantList result;
    const auto index = selectedIndex();
    if (!index)
        return result;
    QHash<quint32, quint32> parents;
    for (const auto& playlist : index->playlists)
        parents.insert(playlist.id, playlist.parentId);
    result.reserve(index->playlists.size());
    for (const auto& playlist : index->playlists) {
        int depth = 0;
        quint32 parent = playlist.parentId;
        QSet<quint32> visited;
        while (parent != 0 && parents.contains(parent) && depth < 32
               && !visited.contains(parent)) {
            visited.insert(parent);
            parent = parents.value(parent);
            ++depth;
        }
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::number(playlist.id));
        item.insert(QStringLiteral("parentId"), playlist.parentId == 0
            ? QString() : QString::number(playlist.parentId));
        item.insert(QStringLiteral("sortOrder"), playlist.sortOrder);
        item.insert(QStringLiteral("name"), playlist.name);
        item.insert(QStringLiteral("folder"), playlist.folder);
        item.insert(QStringLiteral("depth"), depth);
        item.insert(QStringLiteral("trackCount"), playlist.trackIds.size());
        result.append(item);
    }
    return result;
}

QVariantList DeviceLibraryManager::currentArtists() const
{
    const auto index = selectedIndex();
    return index ? aggregateValues(*index, [](const auto& track) { return track.artist; })
                 : QVariantList{};
}

QVariantList DeviceLibraryManager::currentAlbums() const
{
    const auto index = selectedIndex();
    return index ? aggregateValues(*index, [](const auto& track) { return track.album; })
                 : QVariantList{};
}

QVariantList DeviceLibraryManager::currentGenres() const
{
    const auto index = selectedIndex();
    return index ? aggregateValues(*index, [](const auto& track) { return track.genre; })
                 : QVariantList{};
}

QVariantList DeviceLibraryManager::currentFolders() const
{
    const auto index = selectedIndex();
    return index ? aggregateValues(*index, [](const auto& track) {
        return QFileInfo(QDir::fromNativeSeparators(track.relativeAudioPath)).path();
    }) : QVariantList{};
}

QString DeviceLibraryManager::selectedDeviceName() const
{
    const auto found = m_devices.constFind(m_selectedDeviceId);
    return found == m_devices.cend() ? QString() : found->name;
}

void DeviceLibraryManager::requestDeckLoad(const QString& sourceAwareTrackId,
                                            const QString& deckLetter)
{
    const auto index = selectedIndex();
    const auto device = m_devices.constFind(m_selectedDeviceId);
    if (!index || device == m_devices.cend()) {
        emit deckLoadFailed(deckLetter, QStringLiteral("Device is unavailable"));
        return;
    }
    const auto track = index->trackBySourceAwareId.constFind(sourceAwareTrackId);
    if (track == index->trackBySourceAwareId.cend()
        || index->tracks.at(*track).audioPath.isEmpty()) {
        emit deckLoadFailed(deckLetter, QStringLiteral("Track is unavailable on the device"));
        return;
    }
    WorkerTask task;
    task.kind = WorkerTask::Kind::Analysis;
    task.generation = m_generation;
    task.deviceId = m_selectedDeviceId;
    task.deckLetter = deckLetter;
    task.trackId = sourceAwareTrackId;
    task.index = index;
    {
        std::lock_guard lock(m_workerMutex);
        m_tasks.push_front(std::move(task));
    }
    m_workerCondition.notify_one();
    setStatusMessage(QStringLiteral("Reading Rekordbox analysis…"));
}

void DeviceLibraryManager::applyAnalysisResult(
    QString deviceId, quint64 generation, QString deckLetter, rekordbox::Track track,
    std::shared_ptr<const rekordbox::DeviceIndex> sourceIndex,
    rekordbox::AnalysisReader::Result result)
{
    const auto device = m_devices.constFind(deviceId);
    Q_UNUSED(generation)
    if (device == m_devices.cend() || !sourceIndex || device->index != sourceIndex) {
        emit deckLoadFailed(deckLetter, QStringLiteral("Device was removed during track load"));
        return;
    }
    if (!result.ok) {
        // Missing/corrupt external analysis must not block direct audio loading.
        setStatusMessage(result.error);
        result.analysis = {};
    }

    QVariantMap request = trackMap(track);
    request.insert(QStringLiteral("sourceId"), deviceId);
    request.insert(QStringLiteral("readOnlyExternal"), true);
    request.insert(QStringLiteral("analysisOrigin"), QStringLiteral("rekordboxDevice"));
    QVariantList beats;
    beats.reserve(result.analysis.beats.size());
    for (const auto& beat : result.analysis.beats) {
        beats.append(QVariantMap{{QStringLiteral("positionSec"), beat.positionSec},
                                 {QStringLiteral("bpm"), beat.bpm},
                                 {QStringLiteral("beatInBar"), beat.beatInBar}});
    }
    auto cuesToVariant = [](const QVector<rekordbox::Cue>& cues) {
        QVariantList values;
        values.reserve(cues.size());
        for (const auto& cue : cues) {
            values.append(QVariantMap{{QStringLiteral("hotCueIndex"), cue.hotCueIndex},
                                      {QStringLiteral("positionSec"), cue.positionSec},
                                      {QStringLiteral("loopEndSec"), cue.loopEndSec},
                                      {QStringLiteral("label"), cue.label},
                                      {QStringLiteral("color"), cue.color}});
        }
        return values;
    };
    request.insert(QStringLiteral("beats"), beats);
    request.insert(QStringLiteral("hotCues"), cuesToVariant(result.analysis.hotCues));
    request.insert(QStringLiteral("memoryCues"), cuesToVariant(result.analysis.memoryCues));
    request.insert(QStringLiteral("loops"), cuesToVariant(result.analysis.loops));
    setStatusMessage(result.analysis.beats.isEmpty()
        ? QStringLiteral("Loading track without Rekordbox beatgrid")
        : QStringLiteral("Rekordbox beatgrid ready"));
    emit deckLoadReady(deckLetter, request);
}

void DeviceLibraryManager::setBusy(bool value)
{
    if (m_busy == value)
        return;
    m_busy = value;
    emit busyChanged();
}

void DeviceLibraryManager::setStatusMessage(QString value)
{
    if (m_statusMessage == value)
        return;
    m_statusMessage = std::move(value);
    emit statusMessageChanged();
}
