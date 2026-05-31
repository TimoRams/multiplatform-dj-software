#include "BeatTracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis {
namespace {

float localEvidence(const AnalysisFeatures& f, int frame)
{
    if (frame < 0 || frame >= static_cast<int>(f.onsetStrength.size()))
        return 0.0f;
    const size_t i = static_cast<size_t>(frame);
    return std::clamp(0.52f * f.onsetStrength[i]
                    + 0.24f * f.lowSpectralFlux[i]
                    + 0.16f * f.lowEnergy[i]
                    + 0.08f * f.energyNovelty[i], 0.0f, 1.0f);
}

int bestLocalFrame(const AnalysisFeatures& f, int predicted, int radius)
{
    int best = std::clamp(predicted, 0, static_cast<int>(f.onsetStrength.size()) - 1);
    float bestScore = -1.0f;
    for (int frame = predicted - radius; frame <= predicted + radius; ++frame) {
        const float score = localEvidence(f, frame);
        if (score > bestScore) {
            bestScore = score;
            best = frame;
        }
    }
    return best;
}

} // namespace

BeatTrackingResult BeatTracker::track(const AnalysisFeatures& features,
                                      double bpm) const
{
    BeatTrackingResult result;
    if (bpm <= 0.0 || features.onsetStrength.size() < 16 || features.sampleRate <= 0.0)
        return result;

    const double framesPerBeat = (60.0 * features.sampleRate)
        / (bpm * static_cast<double>(features.hopSize));
    if (framesPerBeat < 2.0)
        return result;

    const int period = std::max(1, static_cast<int>(std::round(framesPerBeat)));
    const int phaseLimit = std::min(period, static_cast<int>(features.onsetStrength.size()));
    const int localRadius = std::max(1, static_cast<int>(std::round(framesPerBeat * 0.12)));

    int bestPhase = 0;
    double bestPhaseScore = -1.0;
    for (int phase = 0; phase < phaseLimit; ++phase) {
        double score = 0.0;
        int count = 0;
        for (int frame = phase; frame < static_cast<int>(features.onsetStrength.size()); frame += period) {
            score += localEvidence(features, bestLocalFrame(features, frame, localRadius));
            ++count;
        }
        if (count > 4)
            score /= std::sqrt(static_cast<double>(count));
        if (score > bestPhaseScore) {
            bestPhaseScore = score;
            bestPhase = phase;
        }
    }

    std::vector<double> refinedFrames;
    std::vector<float> confidences;
    for (double predicted = static_cast<double>(bestPhase);
         predicted < static_cast<double>(features.onsetStrength.size());
         predicted += framesPerBeat) {
        const int local = bestLocalFrame(features, static_cast<int>(std::round(predicted)), localRadius);
        const float confidence = localEvidence(features, local);
        refinedFrames.push_back(static_cast<double>(local));
        confidences.push_back(confidence);
    }

    if (refinedFrames.size() < 4)
        return result;

    double sumW = 0.0;
    double sumI = 0.0;
    double sumT = 0.0;
    double sumII = 0.0;
    double sumIT = 0.0;
    for (size_t i = 0; i < refinedFrames.size(); ++i) {
        const double w = 0.25 + static_cast<double>(confidences[i]);
        const double idx = static_cast<double>(i);
        sumW += w;
        sumI += w * idx;
        sumT += w * refinedFrames[i];
        sumII += w * idx * idx;
        sumIT += w * idx * refinedFrames[i];
    }

    double slope = framesPerBeat;
    double intercept = refinedFrames.front();
    const double denom = sumW * sumII - sumI * sumI;
    if (std::abs(denom) > 1.0e-9) {
        slope = (sumW * sumIT - sumI * sumT) / denom;
        intercept = (sumT - slope * sumI) / sumW;
    }

    result.beats.reserve(refinedFrames.size());
    double confidenceSum = 0.0;
    for (size_t i = 0; i < refinedFrames.size(); ++i) {
        const double frame = intercept + slope * static_cast<double>(i);
        const double seconds = features.frameToSeconds(static_cast<size_t>(std::max(0.0, frame)));
        if (seconds < 0.0 || seconds > features.durationSec)
            continue;

        BeatMarker marker;
        marker.positionSec = seconds;
        marker.isBeat = true;
        marker.confidence = confidences[i];
        result.beats.push_back(marker);
        confidenceSum += marker.confidence;
    }

    if (!result.beats.empty())
        result.confidence = static_cast<float>(std::clamp(confidenceSum / static_cast<double>(result.beats.size()), 0.0, 1.0));
    return result;
}

} // namespace analysis
