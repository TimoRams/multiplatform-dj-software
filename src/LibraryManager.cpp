#include "LibraryManager.h"
#include <QDir>

static const QStringList kAudioFilters = {
    "*.mp3", "*.flac", "*.wav", "*.aif", "*.aiff",
    "*.ogg", "*.m4a", "*.aac", "*.opus"
};

LibraryManager::LibraryManager(QObject* parent)
    : QObject(parent)
{
    QStringList musicLocations = QStandardPaths::standardLocations(QStandardPaths::MusicLocation);
    m_rootPath = musicLocations.isEmpty() ? QDir::homePath() : musicLocations.first();
    m_currentFolder = m_rootPath;
    refresh();
}

void LibraryManager::enterFolder(const QString& folderName)
{
    QString newPath = QDir(m_currentFolder).absoluteFilePath(folderName);
    QDir dir(newPath);
    if (!dir.exists()) return;
    m_currentFolder = dir.absolutePath();
    emit currentFolderChanged();
    refresh();
}

void LibraryManager::navigateUp()
{
    if (m_currentFolder == m_rootPath) return;
    QDir dir(m_currentFolder);
    dir.cdUp();
    // Don't go above rootPath
    if (!dir.absolutePath().startsWith(m_rootPath))
        m_currentFolder = m_rootPath;
    else
        m_currentFolder = dir.absolutePath();
    emit currentFolderChanged();
    refresh();
}

void LibraryManager::selectFolder(const QString& absolutePath)
{
    QDir dir(absolutePath);
    if (!dir.exists()) return;
    m_currentFolder = dir.absolutePath();
    emit currentFolderChanged();
    refresh();
}

void LibraryManager::refresh()
{
    QDir dir(m_currentFolder);

    // Subdirectories
    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    dir.setSorting(QDir::Name | QDir::IgnoreCase);
    m_folders = dir.entryList();
    emit foldersChanged();

    // Audio files
    dir.setNameFilters(kAudioFilters);
    dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
    m_tracks = dir.entryList();
    emit tracksChanged();
}
