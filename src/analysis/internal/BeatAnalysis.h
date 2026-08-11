#pragma once

#include "analysis/AnalysisTypes.h"

namespace analysis {

class TempoEstimator {
public:
    struct Options {
        double minBpm = 60.0;
        double maxBpm = 200.0;
        double preferredMinBpm = 70.0;
        double preferredMaxBpm = 180.0;
    };

    TempoEstimator();
    explicit TempoEstimator(Options options);

    TempoEstimate estimate(const AnalysisFeatures& features) const;

private:
    Options m_options;
};

class BeatTracker {
public:
    BeatTrackingResult track(const AnalysisFeatures& features,
                             double bpm) const;
};

class BeatGridFitter {
public:
    BeatGridFitResult fit(const AnalysisFeatures& features,
                          const std::vector<BeatMarker>& trackedBeats,
                          double estimatedBpm) const;
};

class DownbeatDetector {
public:
    DownbeatResult detectAndAnnotate(const AnalysisFeatures& features,
                                     std::vector<BeatMarker>& beats) const;
};

} // namespace analysis
