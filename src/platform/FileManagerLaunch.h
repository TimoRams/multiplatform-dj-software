#pragma once

#include <QString>

namespace platform {

// Opens `path` in the desktop's file manager, creating it first if needed.
// Returns false when the directory could not be created or no handler took it.
bool openDirectoryInFileManager(const QString& path);

} // namespace platform
