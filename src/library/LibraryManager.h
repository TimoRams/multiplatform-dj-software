#pragma once

#include <QObject>
#include <QStringList>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>

#include <cstdint>

class MediaIoScheduler;

class LibraryManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList folders       READ folders       NOTIFY foldersChanged)
    Q_PROPERTY(QStringList tracks        READ tracks        NOTIFY tracksChanged)
    Q_PROPERTY(QString     currentFolder READ currentFolder NOTIFY currentFolderChanged)
    Q_PROPERTY(bool        canNavigateUp READ canNavigateUp NOTIFY currentFolderChanged)

public:
    explicit LibraryManager(MediaIoScheduler& mediaIoScheduler, QObject* parent = nullptr);

    QStringList folders()       const { return m_folders; }
    QStringList tracks()        const { return m_tracks; }
    QString     currentFolder() const { return m_currentFolder; }
    bool        canNavigateUp() const { return m_currentFolder != m_rootPath; }

    Q_INVOKABLE void enterFolder(const QString& folderName);
    Q_INVOKABLE void navigateUp();
    Q_INVOKABLE void selectFolder(const QString& absolutePath);

signals:
    void foldersChanged();
    void tracksChanged();
    void currentFolderChanged();

private:
    void refresh();
    void collectResults();

    MediaIoScheduler& m_mediaIoScheduler;
    QTimer m_resultTimer;
    QString     m_rootPath;
    QString     m_currentFolder;
    QStringList m_folders;
    QStringList m_tracks;
    std::uint64_t m_generation = 0;
    std::uint64_t m_folderRequestId = 0;
    std::uint64_t m_trackRequestId = 0;

    static const inline QStringList kAudioFilters = {
        "*.mp3", "*.flac", "*.wav", "*.aif", "*.aiff",
        "*.ogg", "*.m4a", "*.aac", "*.opus"
    };
};
