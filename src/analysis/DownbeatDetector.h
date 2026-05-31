#pragma once

#include "AnalysisResult.h"

namespace analysis {

class DownbeatDetector {
public:
    DownbeatResult detectAndAnnotate(const AnalysisFeatures& features,
                                     std::vector<BeatMarker>& beats) const;
};

} // namespace analysis
