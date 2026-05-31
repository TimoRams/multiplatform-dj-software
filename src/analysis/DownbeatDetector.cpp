#include "DownbeatDetector.h"

#include <algorithm>
#include <cmath>

namespace analysis {

DownbeatResult DownbeatDetector::detectAndAnnotate(const AnalysisFeatures& features,
                                                   std::vector<BeatMarker>& beats) const
{
    DownbeatResult result;
    if (beats.empty())
        return result;

    double phaseScore[4] = {0.0, 0.0, 0.0, 0.0};
    int phaseCount[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < beats.size(); ++i) {
        const size_t frame = features.secondsToFrame(beats[i].positionSec);
        if (frame >= features.lowEnergy.size())
            continue;

        const double beatStrength = beats[i].confidence;
        const double lowAccent = features.lowEnergy[frame];
        const double novelty = features.energyNovelty[frame];
        const double onset = features.onsetStrength[frame];
        phaseScore[i % 4] += 0.36 * beatStrength + 0.34 * lowAccent + 0.20 * novelty + 0.10 * onset;
        ++phaseCount[i % 4];
    }

    for (int p = 0; p < 4; ++p) {
        if (phaseCount[p] > 0)
            phaseScore[p] /= static_cast<double>(phaseCount[p]);
    }

    int bestPhase = 0;
    for (int p = 1; p < 4; ++p) {
        if (phaseScore[p] > phaseScore[bestPhase])
            bestPhase = p;
    }

    double second = 0.0;
    for (int p = 0; p < 4; ++p) {
        if (p != bestPhase)
            second = std::max(second, phaseScore[p]);
    }
    result.phase = bestPhase;
    result.confidence = static_cast<float>(std::clamp((phaseScore[bestPhase] - second) / std::max(0.001, phaseScore[bestPhase]), 0.0, 1.0));
    const int annotationPhase = result.confidence >= 0.42f ? bestPhase : 0;

    for (size_t i = 0; i < beats.size(); ++i) {
        const int rel = static_cast<int>(i) - annotationPhase;
        const int mod4 = ((rel % 4) + 4) % 4;
        beats[i].isDownbeat = (mod4 == 0);
        beats[i].beatInBar = mod4 + 1;
        beats[i].barIndex = static_cast<int>(std::floor(static_cast<double>(rel) / 4.0));
        beats[i].barNumber = beats[i].barIndex + 1;
    }

    return result;
}

} // namespace analysis
