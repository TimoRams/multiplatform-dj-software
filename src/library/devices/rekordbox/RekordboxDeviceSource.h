#pragma once

#include "library/devices/rekordbox/RekordboxAnalysisReader.h"
#include "library/devices/rekordbox/RekordboxPdbReader.h"

#include <QString>

namespace rekordbox {

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
