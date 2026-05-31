#pragma once

#include "AnalysisResult.h"

namespace analysis {

class BeatGridFitter {
public:
    BeatGridFitResult fit(const AnalysisFeatures& features,
                          const std::vector<BeatMarker>& trackedBeats,
                          double estimatedBpm) const;
};

} // namespace analysis
