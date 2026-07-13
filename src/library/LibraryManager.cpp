#include "LibraryManager.h"
#include "io/MediaIoScheduler.h"
#include <QDir>
#include <QTimer>

static const QStringList kAudioFilters = {
    "*.mp3", "*.flac", "*.wav", "*.aif", "*.aiff",
    "*.ogg", "*.m4a", "*.aac", "*.opus"
};

namespace {
constexpr std::uint32_t kLibraryManagerOwner = 1;
}

LibraryManager::LibraryManager(MediaIoScheduler& mediaIoScheduler, QObject* parent)
    : QObject(parent)
    , m_mediaIoScheduler(mediaIoScheduler)
{
    QStringList musicLocations = QStandardPaths::standardLocations(QStandardPaths::MusicLocation);
    m_rootPath = musicLocations.isEmpty() ? QDir::homePath() : musicLocations.first();
    m_currentFolder = m_rootPath;

    m_resultTimer.setInterval(20);
    m_resultTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_resultTimer, &QTimer::timeout, this, &LibraryManager::collectResults);
    m_resultTimer.start();

    // Defer submission so the UI can show immediately. The scan itself is worker-only.
    QTimer::singleShot(0, this, &LibraryManager::refresh);
}

void LibraryManager::enterFolder(const QString& folderName)
{
    const QString newPath = QDir::cleanPath(QDir(m_currentFolder).absoluteFilePath(folderName));
    const QString rootPrefix = QDir::cleanPath(m_rootPath) + QDir::separator();
    if (newPath != m_rootPath && !newPath.startsWith(rootPrefix)) return;
    m_currentFolder = newPath;
    emit currentFolderChanged();
    refresh();
}

void LibraryManager::navigateUp()
{
    if (m_currentFolder == m_rootPath) return;
    QDir dir(m_currentFolder);
    dir.cdUp();
    // Don't go above rootPath
    const QString rootPrefix = QDir::cleanPath(m_rootPath) + QDir::separator();
    if (dir.absolutePath() != m_rootPath && !dir.absolutePath().startsWith(rootPrefix))
        m_currentFolder = m_rootPath;
    else
        m_currentFolder = dir.absolutePath();
    emit currentFolderChanged();
    refresh();
}

void LibraryManager::selectFolder(const QString& absolutePath)
{
    const QString path = QDir::cleanPath(absolutePath);
    const QString rootPrefix = QDir::cleanPath(m_rootPath) + QDir::separator();
    if (path != m_rootPath && !path.startsWith(rootPrefix)) return;
    m_currentFolder = path;
    emit currentFolderChanged();
    refresh();
}

void LibraryManager::refresh()
{
    ++m_generation;
    m_mediaIoScheduler.setCurrentGeneration(kLibraryManagerOwner, m_generation);
    const std::uint64_t base = m_generation * 2;
    m_folderRequestId = base;
    m_trackRequestId = base + 1;

    MediaIoRequest folders;
    folders.type = MediaIoRequestType::ScanDirectory;
    folders.priority = MediaIoPriority::LibraryScan;
    folders.ownerId = kLibraryManagerOwner;
    folders.requestId = m_folderRequestId;
    folders.generation = m_generation;
    folders.inputPath = m_currentFolder;
    folders.maximumEntries = 4096;
    folders.includeFiles = false;
    folders.includeDirectories = true;
    folders.coalescingKey = QStringLiteral("library-folders");
    (void)m_mediaIoScheduler.enqueue(std::move(folders));

    MediaIoRequest tracks;
    tracks.type = MediaIoRequestType::ScanDirectory;
    tracks.priority = MediaIoPriority::LibraryScan;
    tracks.ownerId = kLibraryManagerOwner;
    tracks.requestId = m_trackRequestId;
    tracks.generation = m_generation;
    tracks.inputPath = m_currentFolder;
    tracks.nameFilters = kAudioFilters;
    tracks.maximumEntries = 4096;
    tracks.includeFiles = true;
    tracks.includeDirectories = false;
    tracks.coalescingKey = QStringLiteral("library-tracks");
    (void)m_mediaIoScheduler.enqueue(std::move(tracks));
}

void LibraryManager::collectResults()
{
    for (auto& result : m_mediaIoScheduler.takeResultsForOwner(kLibraryManagerOwner)) {
        if (!result.success || result.cancelled || result.stale
            || result.generation != m_generation)
            continue;
        QStringList names;
        names.reserve(result.paths.size());
        for (const auto& path : result.paths)
            names.push_back(path);
        names.sort(Qt::CaseInsensitive);
        if (result.requestId == m_folderRequestId) {
            m_folders = std::move(names);
            emit foldersChanged();
        } else if (result.requestId == m_trackRequestId) {
            m_tracks = std::move(names);
            emit tracksChanged();
        }
    }
}
