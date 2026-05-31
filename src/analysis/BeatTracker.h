#pragma once

#include "AnalysisResult.h"

namespace analysis {

class BeatTracker {
public:
    BeatTrackingResult track(const AnalysisFeatures& features,
                             double bpm) const;
};

} // namespace analysis
