#include "AnalysisValidation.h"

#include <cmath>

namespace analysis {

ValidationResult validateBeatGrid(const std::vector<BeatMarker>& beats,
                                  double bpm,
                                  double durationSec)
{
    if (bpm <= 0.0)
        return {false, QStringLiteral("invalid bpm")};
    if (durationSec <= 0.0)
        return {false, QStringLiteral("invalid duration")};
    if (beats.size() < 4)
        return {false, QStringLiteral("too few beats")};

    int nonBeatOneCount = 0;
    for (size_t i = 0; i < beats.size(); ++i) {
        const auto& beat = beats[i];
        if (beat.positionSec < -0.001 || beat.positionSec > durationSec + 0.001)
            return {false, QStringLiteral("beat outside track duration")};
        if (i > 0 && beat.positionSec <= beats[i - 1].positionSec)
            return {false, QStringLiteral("beat order is not strictly increasing")};
        if (beat.beatInBar < 1 || beat.beatInBar > 4)
            return {false, QStringLiteral("beatInBar outside 1..4")};
        if (beat.beatInBar != 1)
            ++nonBeatOneCount;
        if (beat.isDownbeat && beat.beatInBar != 1)
            return {false, QStringLiteral("downbeat is not beat 1")};
    }

    if (beats.size() >= 8 && nonBeatOneCount == 0)
        return {false, QStringLiteral("beatInBar did not advance")};

    return {};
}

} // namespace analysis
