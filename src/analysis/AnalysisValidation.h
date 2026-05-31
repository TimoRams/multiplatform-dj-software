#pragma once

#include <QString>

#include "AnalysisResult.h"

namespace analysis {

struct ValidationResult {
    bool ok = true;
    QString message;
};

ValidationResult validateBeatGrid(const std::vector<BeatMarker>& beats,
                                  double bpm,
                                  double durationSec);

} // namespace analysis
