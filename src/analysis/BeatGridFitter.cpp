#include "BeatGridFitter.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis {
namespace {

struct StableRegion {
    int startFrame = 0;
    int endFrame = 0;
    double score = 0.0;
};

float beatEvidenceAt(const AnalysisFeatures& f, int frame)
{
    if (frame < 0 || frame >= static_cast<int>(f.onsetStrength.size()))
        return 0.0f;
    const size_t i = static_cast<size_t>(frame);
    return std::clamp(0.46f * f.onsetStrength[i]
                    + 0.26f * f.lowSpectralFlux[i]
                    + 0.20f * f.lowEnergy[i]
                    + 0.08f * f.energyNovelty[i], 0.0f, 1.0f);
}

float localBeatEvidence(const AnalysisFeatures& f, double frame, int toleranceFrames)
{
    const int center = static_cast<int>(std::round(frame));
    float best = 0.0f;
    for (int i = center - toleranceFrames; i <= center + toleranceFrames; ++i) {
        const float distance = static_cast<float>(std::abs(i - center));
        const float weight = 1.0f - 0.35f * (distance / static_cast<float>(std::max(1, toleranceFrames)));
        best = std::max(best, beatEvidenceAt(f, i) * weight);
    }
    return best;
}

std::vector<int> strongOnsetFrames(const AnalysisFeatures& f, int startFrame, int endFrame)
{
    std::vector<int> peaks;
    if (endFrame - startFrame < 3)
        return peaks;

    double mean = 0.0;
    for (int i = startFrame; i < endFrame; ++i)
        mean += beatEvidenceAt(f, i);
    mean /= static_cast<double>(std::max(1, endFrame - startFrame));
    const float threshold = static_cast<float>(std::max(0.18, mean * 1.35));

    for (int i = std::max(1, startFrame + 1); i + 1 < endFrame; ++i) {
        const float v = beatEvidenceAt(f, i);
        if (v >= threshold && v >= beatEvidenceAt(f, i - 1) && v > beatEvidenceAt(f, i + 1))
            peaks.push_back(i);
    }
    return peaks;
}

StableRegion findStableRegion(const AnalysisFeatures& f, double periodFrames)
{
    StableRegion best;
    const int n = static_cast<int>(f.onsetStrength.size());
    if (n <= 0)
        return best;

    const int period = std::max(2, static_cast<int>(std::round(periodFrames)));
    const int minWindow = std::clamp(period * 16, 64, std::max(64, n));
    const int maxWindow = std::clamp(period * 64, minWindow, std::max(minWindow, n));
    const int window = std::min(maxWindow, n);
    const int step = std::max(period * 4, window / 8);

    for (int start = 0; start < n; start += step) {
        const int end = std::min(n, start + window);
        if (end - start < minWindow)
            break;

        double mean = 0.0;
        double meanLow = 0.0;
        for (int i = start; i < end; ++i) {
            mean += beatEvidenceAt(f, i);
            meanLow += f.lowEnergy[static_cast<size_t>(i)];
        }
        mean /= static_cast<double>(end - start);
        meanLow /= static_cast<double>(end - start);

        double variance = 0.0;
        for (int i = start; i < end; ++i) {
            const double d = beatEvidenceAt(f, i) - mean;
            variance += d * d;
        }
        const double stddev = std::sqrt(variance / static_cast<double>(end - start));
        const double energyStability = std::clamp(1.0 - stddev / std::max(0.05, mean + 0.05), 0.0, 1.0);

        double pulse = 0.0;
        int pulseCount = 0;
        for (int i = start + period; i < end; ++i) {
            pulse += beatEvidenceAt(f, i) * beatEvidenceAt(f, i - period);
            ++pulseCount;
        }
        pulse = pulseCount > 0 ? pulse / static_cast<double>(pulseCount) : 0.0;

        const auto peaks = strongOnsetFrames(f, start, end);
        const double density = static_cast<double>(peaks.size())
            / std::max(1.0, static_cast<double>(end - start) / periodFrames);
        const double densityScore = std::clamp(density / 1.4, 0.0, 1.0);

        const double score = 0.36 * mean + 0.24 * pulse + 0.18 * meanLow
                           + 0.14 * densityScore + 0.08 * energyStability;
        if (score > best.score) {
            best = {start, end, score};
        }
    }

    if (best.endFrame <= best.startFrame) {
        best.startFrame = 0;
        best.endFrame = n;
        best.score = 0.0;
    }
    return best;
}

double scoreOffset(const AnalysisFeatures& f,
                   double offsetFrame,
                   double periodFrames,
                   const StableRegion& region,
                   int toleranceFrames,
                   double* outMeanEvidence,
                   double* outCoverage)
{
    const int startBeat = static_cast<int>(std::ceil((region.startFrame - offsetFrame) / periodFrames));
    const int endBeat = static_cast<int>(std::floor((region.endFrame - offsetFrame) / periodFrames));
    if (endBeat - startBeat < 8)
        return 0.0;

    std::vector<float> hits;
    hits.reserve(static_cast<size_t>(endBeat - startBeat + 1));
    for (int beat = startBeat; beat <= endBeat; ++beat) {
        const double frame = offsetFrame + static_cast<double>(beat) * periodFrames;
        hits.push_back(localBeatEvidence(f, frame, toleranceFrames));
    }

    const double mean = std::accumulate(hits.begin(), hits.end(), 0.0) / static_cast<double>(hits.size());
    double variance = 0.0;
    int weak = 0;
    for (float hit : hits) {
        variance += (hit - mean) * (hit - mean);
        if (hit < mean * 0.55)
            ++weak;
    }
    const double stddev = std::sqrt(variance / static_cast<double>(hits.size()));
    const double consistency = std::clamp(1.0 - stddev / std::max(0.08, mean), 0.0, 1.0);
    const double weakPenalty = static_cast<double>(weak) / static_cast<double>(hits.size());

    const auto peaks = strongOnsetFrames(f, region.startFrame, region.endFrame);
    int captured = 0;
    for (int peak : peaks) {
        const double nearestBeat = std::round((static_cast<double>(peak) - offsetFrame) / periodFrames);
        const double nearestFrame = offsetFrame + nearestBeat * periodFrames;
        if (std::abs(nearestFrame - static_cast<double>(peak)) <= std::max(2.0, periodFrames * 0.10))
            ++captured;
    }
    const double coverage = peaks.empty() ? mean : static_cast<double>(captured) / static_cast<double>(peaks.size());
    const double densityRatio = static_cast<double>(hits.size()) / std::max(1.0, static_cast<double>(peaks.size()));
    const double densePenalty = densityRatio > 1.75 ? std::min(0.35, (densityRatio - 1.75) * 0.16) : 0.0;
    const double sparsePenalty = coverage < 0.42 ? (0.42 - coverage) * 0.45 : 0.0;

    if (outMeanEvidence)
        *outMeanEvidence = mean;
    if (outCoverage)
        *outCoverage = coverage;

    return 0.52 * mean + 0.22 * consistency + 0.22 * coverage
         - 0.22 * weakPenalty - densePenalty - sparsePenalty;
}

} // namespace

BeatGridFitResult BeatGridFitter::fit(const AnalysisFeatures& features,
                                      const std::vector<BeatMarker>& trackedBeats,
                                      double estimatedBpm) const
{
    BeatGridFitResult result;
    result.bpm = estimatedBpm;
    result.grid.type = BeatGridType::ConstantTempo;

    if (estimatedBpm <= 0.0 || features.onsetStrength.size() < 16 || features.sampleRate <= 0.0)
        return result;

    const double period = 60.0 / estimatedBpm;
    const double periodFrames = period * features.sampleRate / static_cast<double>(features.hopSize);
    if (periodFrames < 2.0)
        return result;

    const StableRegion region = findStableRegion(features, periodFrames);
    const int toleranceFrames = std::max(1, static_cast<int>(std::round(periodFrames * 0.055)));

    double bestOffsetFrame = static_cast<double>(region.startFrame);
    double bestScore = -1.0;
    double bestMeanEvidence = 0.0;
    double bestCoverage = 0.0;
    const int phaseSteps = std::max(2, static_cast<int>(std::ceil(periodFrames)));
    for (int step = 0; step < phaseSteps; ++step) {
        const double offset = static_cast<double>(region.startFrame) + static_cast<double>(step);
        double meanEvidence = 0.0;
        double coverage = 0.0;
        const double score = scoreOffset(features, offset, periodFrames, region,
                                         toleranceFrames, &meanEvidence, &coverage);
        if (score > bestScore) {
            bestScore = score;
            bestOffsetFrame = offset;
            bestMeanEvidence = meanEvidence;
            bestCoverage = coverage;
        }
    }

    const double firstVisibleBeatIndex = std::ceil(-bestOffsetFrame / periodFrames);
    const double anchorFrame = bestOffsetFrame + firstVisibleBeatIndex * periodFrames;
    const double secondsPerFrame = static_cast<double>(features.hopSize) / features.sampleRate;
    const double anchorSec = std::max(0.0, anchorFrame * secondsPerFrame);
    result.bpm = estimatedBpm;
    result.selectedOffsetSec = std::max(0.0, bestOffsetFrame * secondsPerFrame);
    result.stableRegionStartSec = std::max(0.0, static_cast<double>(region.startFrame) * secondsPerFrame);
    result.stableRegionEndSec = std::max(0.0, static_cast<double>(region.endFrame) * secondsPerFrame);
    result.phaseScore = bestScore;
    result.firstBeatSample = static_cast<qint64>(std::llround(anchorSec * features.sampleRate));

    result.beats.reserve(static_cast<size_t>(features.durationSec / period) + 4);
    for (int n = 0; ; ++n) {
        const double sec = anchorSec + static_cast<double>(n) * period;
        if (sec > features.durationSec + period * 0.5)
            break;
        BeatMarker marker;
        marker.positionSec = std::min(sec, features.durationSec);
        marker.isBeat = true;
        marker.confidence = localBeatEvidence(features,
                                              static_cast<double>(features.secondsToFrame(sec)),
                                              toleranceFrames);
        result.beats.push_back(marker);
    }

    std::vector<double> localBpms;
    for (size_t i = 16; i < trackedBeats.size(); i += 16) {
        const double span = trackedBeats[i].positionSec - trackedBeats[i - 16].positionSec;
        if (span > 0.0)
            localBpms.push_back(16.0 * 60.0 / span);
    }

    if (localBpms.size() >= 3) {
        const double mean = std::accumulate(localBpms.begin(), localBpms.end(), 0.0) / static_cast<double>(localBpms.size());
        double variance = 0.0;
        double minBpm = localBpms.front();
        double maxBpm = localBpms.front();
        for (double bpm : localBpms) {
            variance += (bpm - mean) * (bpm - mean);
            minBpm = std::min(minBpm, bpm);
            maxBpm = std::max(maxBpm, bpm);
        }
        const double stddev = std::sqrt(variance / static_cast<double>(localBpms.size()));
        if (stddev > 1.2 || (maxBpm - minBpm) > 2.8) {
            result.grid.type = BeatGridType::DynamicTempo;
            for (size_t i = 16; i < trackedBeats.size(); i += 16) {
                const double span = trackedBeats[i].positionSec - trackedBeats[i - 16].positionSec;
                if (span <= 0.0)
                    continue;
                TempoNode node;
                node.positionSec = trackedBeats[i - 16].positionSec;
                node.bpm = 16.0 * 60.0 / span;
                node.confidence = trackedBeats[i].confidence;
                result.grid.tempoNodes.push_back(node);
            }
        }
    }

    if (result.grid.tempoNodes.empty()) {
        result.grid.tempoNodes.push_back({anchorSec, result.bpm, static_cast<float>(std::clamp(bestScore, 0.0, 1.0))});
    }

    result.confidence = static_cast<float>(std::clamp(0.55 * bestScore
                                                    + 0.25 * bestMeanEvidence
                                                    + 0.20 * bestCoverage,
                                                    0.0, 1.0));
    return result;
}

} // namespace analysis
