#pragma once

#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QString>
#include <QUrl>
#include <QtGlobal>

namespace platform {

// Opens `path` in the desktop's file manager, creating it first if needed.
// Returns false when the directory could not be created or no handler took it.
inline bool openDirectoryInFileManager(const QString& path)
{
    if (path.isEmpty())
        return false;

    QDir dir(path);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return false;

    const QString nativePath = QDir::toNativeSeparators(dir.absolutePath());

#if defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"), {nativePath}))
        return true;
#elif defined(Q_OS_WIN)
    if (QProcess::startDetached(QStringLiteral("explorer.exe"), {nativePath}))
        return true;
#elif defined(Q_OS_UNIX)
    if (QProcess::startDetached(QStringLiteral("xdg-open"), {nativePath}))
        return true;
#endif

    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

} // namespace platform
