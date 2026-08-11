#include "BeatAnalysis.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace analysis {
namespace {

double autocorrScore(const std::vector<float>& curve, int lag)
{
    if (lag <= 0 || curve.size() <= static_cast<size_t>(lag + 8))
        return 0.0;

    double sum = 0.0;
    double normA = 0.0;
    double normB = 0.0;
    for (size_t i = static_cast<size_t>(lag); i < curve.size(); ++i) {
        const double a = curve[i];
        const double b = curve[i - static_cast<size_t>(lag)];
        sum += a * b;
        normA += a * a;
        normB += b * b;
    }
    const double denom = std::sqrt(normA * normB);
    return denom > 1.0e-12 ? sum / denom : 0.0;
}

double combScore(const std::vector<float>& curve, int lag)
{
    if (lag <= 1 || curve.empty())
        return 0.0;

    const int maxPhase = std::min(lag, static_cast<int>(curve.size()));
    double best = 0.0;
    for (int phase = 0; phase < maxPhase; ++phase) {
        double score = 0.0;
        int count = 0;
        for (size_t i = static_cast<size_t>(phase); i < curve.size(); i += static_cast<size_t>(lag)) {
            score += curve[i];
            ++count;
        }
        if (count > 4)
            best = std::max(best, score / std::sqrt(static_cast<double>(count)));
    }
    return best;
}

std::vector<size_t> pickOnsets(const std::vector<float>& curve)
{
    std::vector<size_t> peaks;
    if (curve.size() < 3)
        return peaks;

    const double mean = std::accumulate(curve.begin(), curve.end(), 0.0) / static_cast<double>(curve.size());
    const float threshold = static_cast<float>(mean * 1.45);
    for (size_t i = 1; i + 1 < curve.size(); ++i) {
        if (curve[i] >= threshold && curve[i] >= curve[i - 1] && curve[i] > curve[i + 1])
            peaks.push_back(i);
    }
    return peaks;
}

double foldToDjRange(double bpm, double preferredMin, double preferredMax)
{
    while (bpm > preferredMax && bpm * 0.5 >= 55.0)
        bpm *= 0.5;
    while (bpm < preferredMin && bpm * 2.0 <= 210.0)
        bpm *= 2.0;
    return bpm;
}

} // namespace

TempoEstimator::TempoEstimator()
    : TempoEstimator(Options{})
{
}

TempoEstimator::TempoEstimator(Options options)
    : m_options(options)
{
}

TempoEstimate TempoEstimator::estimate(const AnalysisFeatures& features) const
{
    TempoEstimate result;
    if (features.onsetStrength.size() < 32 || features.sampleRate <= 0.0 || features.hopSize <= 0)
        return result;

    std::vector<TempoCandidate> raw;
    const double framesPerSecond = features.sampleRate / static_cast<double>(features.hopSize);
    const int minLag = std::max(1, static_cast<int>(std::floor((framesPerSecond * 60.0) / m_options.maxBpm)));
    const int maxLag = std::max(minLag + 1, static_cast<int>(std::ceil((framesPerSecond * 60.0) / m_options.minBpm)));

    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double bpm = (framesPerSecond * 60.0) / static_cast<double>(lag);
        const double onsetAuto = autocorrScore(features.onsetStrength, lag);
        const double lowAuto = autocorrScore(features.lowEnergy, lag);
        const double onsetComb = combScore(features.onsetStrength, lag);
        const double lowComb = combScore(features.lowSpectralFlux, lag);
        const double score = 0.38 * onsetAuto + 0.22 * lowAuto + 0.28 * onsetComb + 0.12 * lowComb;
        if (score > 0.01)
            raw.push_back({bpm, score, QStringLiteral("autocorr+comb")});
    }

    const auto peaks = pickOnsets(features.onsetStrength);
    std::map<int, double> ioiBins;
    for (size_t i = 0; i < peaks.size(); ++i) {
        for (size_t j = i + 1; j < std::min(peaks.size(), i + 9); ++j) {
            const double seconds = static_cast<double>(peaks[j] - peaks[i]) / framesPerSecond;
            if (seconds <= 0.0)
                continue;
            double bpm = 60.0 / seconds;
            bpm = foldToDjRange(bpm, m_options.preferredMinBpm, m_options.preferredMaxBpm);
            if (bpm < m_options.minBpm || bpm > m_options.maxBpm)
                continue;
            const int bin = static_cast<int>(std::round(bpm * 2.0));
            ioiBins[bin] += 1.0 / static_cast<double>(j - i);
        }
    }
    for (const auto& [bin, score] : ioiBins)
        raw.push_back({static_cast<double>(bin) * 0.5, score * 0.08, QStringLiteral("ioi")});

    std::vector<TempoCandidate> merged;
    for (const auto& candidate : raw) {
        const double variants[] = {
            candidate.bpm,
            candidate.bpm * 0.5,
            candidate.bpm * 2.0
        };
        const double weights[] = {1.0, 0.88, 0.82};
        for (int v = 0; v < 3; ++v) {
            double bpm = foldToDjRange(variants[v], m_options.preferredMinBpm, m_options.preferredMaxBpm);
            if (bpm < m_options.minBpm || bpm > m_options.maxBpm)
                continue;
            auto it = std::find_if(merged.begin(), merged.end(), [&](const TempoCandidate& existing) {
                return std::abs(existing.bpm - bpm) <= 0.75;
            });
            const double score = candidate.score * weights[v];
            if (it == merged.end()) {
                merged.push_back({bpm, score, candidate.source});
            } else {
                const double totalScore = it->score + score;
                it->bpm = (it->bpm * it->score + bpm * score) / totalScore;
                it->score = totalScore;
                if (!it->source.contains(candidate.source))
                    it->source += QStringLiteral("+") + candidate.source;
            }
        }
    }

    std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
        return a.score > b.score;
    });
    if (merged.size() > 8)
        merged.resize(8);

    result.candidates = merged;
    if (merged.empty())
        return result;

    result.bpm = merged.front().bpm;
    const double top = merged.front().score;
    const double second = merged.size() > 1 ? merged[1].score : 0.0;
    const double dominance = top > 1.0e-9 ? (top - second) / top : 0.0;
    const double absolute = std::min(1.0, top / 14.0);
    result.confidence = static_cast<float>(std::clamp(0.35 * absolute + 0.65 * dominance, 0.05, 1.0));
    return result;
}

} // namespace analysis

#include "BeatAnalysis.h"

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

#include "BeatAnalysis.h"

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

int strongestLocalBeatFrame(const AnalysisFeatures& f, double frame, int toleranceFrames)
{
    const int center = static_cast<int>(std::round(frame));
    int best = std::clamp(center, 0, static_cast<int>(f.onsetStrength.size()) - 1);
    float bestScore = -1.0f;
    for (int i = center - toleranceFrames; i <= center + toleranceFrames; ++i) {
        const float score = beatEvidenceAt(f, i);
        // Equal-energy plateaus are common after FFT analysis. Prefer the
        // closest frame so they do not introduce a systematic grid drift.
        if (score > bestScore
            || (score == bestScore && std::abs(i - center) < std::abs(best - center))) {
            bestScore = score;
            best = std::clamp(i, 0, static_cast<int>(f.onsetStrength.size()) - 1);
        }
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
        // Keep the global tempo grid stable, but align every visible grid line
        // to the strongest nearby transient.  The bounded search prevents one
        // missing kick from pulling a beat into its neighbour.
        const int predictedFrame = features.secondsToFrame(sec);
        const int snappedFrame = strongestLocalBeatFrame(features, predictedFrame,
                                                         toleranceFrames);
        marker.positionSec = std::min(features.durationSec,
                                      features.frameToSeconds(static_cast<size_t>(snappedFrame)));
        marker.isBeat = true;
        marker.confidence = localBeatEvidence(features, snappedFrame, toleranceFrames);
        if (!result.beats.empty() && marker.positionSec <= result.beats.back().positionSec)
            marker.positionSec = std::min(features.durationSec,
                                          result.beats.back().positionSec + secondsPerFrame * 0.25);
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

#include "BeatAnalysis.h"

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
