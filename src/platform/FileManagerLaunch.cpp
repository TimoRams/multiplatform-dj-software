#include "FileManagerLaunch.h"

#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QUrl>
#include <QtGlobal>

namespace platform {

bool openDirectoryInFileManager(const QString& path)
{
    if (path.isEmpty())
        return false;

    QDir dir(path);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return false;

    const QString nativePath = QDir::toNativeSeparators(dir.absolutePath());

#if defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"), { nativePath }))
        return true;
#elif defined(Q_OS_WIN)
    if (QProcess::startDetached(QStringLiteral("explorer.exe"), { nativePath }))
        return true;
#elif defined(Q_OS_UNIX)
    if (QProcess::startDetached(QStringLiteral("xdg-open"), { nativePath }))
        return true;
#endif

    // Falling back through Qt covers desktops where none of the above exists.
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

} // namespace platform
