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

} // namespace rekordbox
