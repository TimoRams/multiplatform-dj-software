#pragma once

#include <QList>
#include <QString>

// Reading and writing of portable playlist files (M3U/M3U8, PLS, XSPF).
// Pure file-format code: no library access, so it is testable on its own.
namespace brockdj::library {

struct PlaylistFileEntry {
    QString path;   // absolute local path
    QString title;
    QString artist;
    int durationSec = -1;
};

enum class PlaylistFileFormat { M3U, PLS, XSPF, Unknown };

PlaylistFileFormat playlistFormatForPath(const QString& path);

// Accepts plain paths and file:// URLs (as QML file dialogs hand them out).
QString localPathFromUrlOrPath(const QString& urlOrPath);

// Relative entries are resolved against the playlist's folder. Remote
// (http/…) entries are skipped. Returns false with `error` set on failure.
bool readPlaylistFile(const QString& path, QList<PlaylistFileEntry>& entries, QString* error = nullptr);

// Writes UTF-8 with absolute paths; the format follows the file extension
// (unknown extensions are written as M3U8).
bool writePlaylistFile(const QString& path, const QList<PlaylistFileEntry>& entries,
                       QString* error = nullptr);

} // namespace brockdj::library
