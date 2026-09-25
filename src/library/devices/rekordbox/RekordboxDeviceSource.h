#pragma once

#include "library/devices/rekordbox/RekordboxTypes.h"

#include <QString>

namespace rekordbox {

class PdbReader final
{
public:
    struct Result : ReadResult {
        QVector<Track> tracks;
        QVector<Playlist> playlists;
    };

    [[nodiscard]] Result readReadOnly(const QString& path) const;
};

class AnalysisReader final
{
public:
    struct Result : ReadResult {
        Analysis analysis;
    };

    [[nodiscard]] Result readReadOnly(const QString& path) const;
    [[nodiscard]] Result readRelatedReadOnly(const QString& datPath) const;
};

class DeviceSource final
{
public:
    struct Result : ReadResult {
        DeviceIndex index;
    };

    [[nodiscard]] Result readIndexReadOnly(const QString& mountPath,
                                           const QString& deviceId) const;
    [[nodiscard]] AnalysisReader::Result readAnalysisReadOnly(
        const DeviceIndex& index, const QString& sourceAwareTrackId) const;

    [[nodiscard]] static QString resolveContainedPath(const QString& mountPath,
                                                      const QString& devicePath,
                                                      bool requireFile = true);
};

} // namespace rekordbox
