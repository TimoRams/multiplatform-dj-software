#include "AnalysisValidation.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace analysis {

ValidationResult validateBeatGrid(const std::vector<BeatMarker>& beats,
                                  double bpm,
                                  double durationSec)
{
    if (!std::isfinite(bpm) || bpm < 40.0 || bpm > 300.0)
        return {false, QStringLiteral("invalid bpm")};
    if (!std::isfinite(durationSec) || durationSec <= 0.0)
        return {false, QStringLiteral("invalid duration")};
    if (beats.size() < 4)
        return {false, QStringLiteral("too few beats")};

    int nonBeatOneCount = 0;
    int sequenceErrors = 0;
    std::vector<double> intervals;
    intervals.reserve(beats.size() - 1);
    for (size_t i = 0; i < beats.size(); ++i) {
        const auto& beat = beats[i];
        if (beat.positionSec < -0.001 || beat.positionSec > durationSec + 0.001)
            return {false, QStringLiteral("beat outside track duration")};
        if (i > 0) {
            if (beat.positionSec <= beats[i - 1].positionSec)
                return {false, QStringLiteral("beat order is not strictly increasing")};
            intervals.push_back(beat.positionSec - beats[i - 1].positionSec);
            const int expectedBeatInBar = beats[i - 1].beatInBar % 4 + 1;
            if (beat.beatInBar != expectedBeatInBar)
                ++sequenceErrors;
        }
        if (beat.beatInBar < 1 || beat.beatInBar > 4)
            return {false, QStringLiteral("beatInBar outside 1..4")};
        if (beat.beatInBar != 1)
            ++nonBeatOneCount;
        if (beat.isDownbeat && beat.beatInBar != 1)
            return {false, QStringLiteral("downbeat is not beat 1")};
    }

    if (beats.size() >= 8 && nonBeatOneCount == 0)
        return {false, QStringLiteral("beatInBar did not advance")};
    if (sequenceErrors > 0)
        return {false, QStringLiteral("beatInBar sequence is discontinuous")};

    const double expectedInterval = 60.0 / bpm;
    std::vector<double> relativeErrors;
    relativeErrors.reserve(intervals.size());
    int severeSpacingErrors = 0;
    for (double interval : intervals) {
        const double relativeError = std::abs(interval - expectedInterval)
            / expectedInterval;
        relativeErrors.push_back(relativeError);
        if (relativeError > 0.20)
            ++severeSpacingErrors;
    }
    const auto middle = relativeErrors.begin()
        + static_cast<std::ptrdiff_t>(relativeErrors.size() / 2);
    std::nth_element(relativeErrors.begin(), middle, relativeErrors.end());
    const double medianSpacingError = *middle;
    if (medianSpacingError > 0.08
        || severeSpacingErrors > std::max<int>(1, intervals.size() / 12)) {
        return {false, QStringLiteral("beat spacing disagrees with bpm")};
    }

    return {};
}

} // namespace analysis
