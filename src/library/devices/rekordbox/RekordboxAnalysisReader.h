#pragma once

#include "library/devices/rekordbox/RekordboxTypes.h"

#include <QStringList>

namespace rekordbox {

class AnalysisReader final
{
public:
    struct Result : ReadResult {
        Analysis analysis;
    };

    [[nodiscard]] Result readReadOnly(const QString& path) const;
    [[nodiscard]] Result readRelatedReadOnly(const QString& datPath) const;
};

} // namespace rekordbox
