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

// Diagnostics intentionally live next to the fitter rather than in UI code so
// synthetic and file-backed regression tests measure the same constant-grid
// contract.  Errors are expressed in seconds and are evaluated over the
// entire supplied duration, not just around the first beat.
struct BeatGridQualityMetrics {
    double bpmError = 0.0;
    double phaseErrorSec = 0.0;
    double meanBeatErrorSec = 0.0;
    double percentile95BeatErrorSec = 0.0;
    double maximumEndOfTrackDriftSec = 0.0;
    float confidence = 0.0f;
};

[[nodiscard]] BeatGridQualityMetrics measureBeatGridQuality(
    const BeatGridFitResult& grid,
    double referenceBpm,
    double referenceFirstBeatSec,
    double durationSec);

class DownbeatDetector {
public:
    DownbeatResult detectAndAnnotate(const AnalysisFeatures& features,
                                     std::vector<BeatMarker>& beats) const;
};

} // namespace analysis
