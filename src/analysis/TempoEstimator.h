#pragma once

#include "AnalysisResult.h"

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

} // namespace analysis
