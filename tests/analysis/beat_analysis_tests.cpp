#include "analysis/AnalysisValidation.h"
#include "analysis/internal/BeatAnalysis.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

analysis::AnalysisFeatures pulseTrain(double bpm,
                                      double durationSeconds = 60.0,
                                      int downbeatPhase = 0,
                                      bool jitterOnsets = false)
{
    analysis::AnalysisFeatures features;
    features.sampleRate = 100.0;
    features.hopSize = 1;
    features.frameSize = 64;
    features.durationSec = durationSeconds;
    const int frames = static_cast<int>(durationSeconds * features.sampleRate);
    features.rms.assign(frames, 0.05f);
    features.lowEnergy.assign(frames, 0.04f);
    features.midEnergy.assign(frames, 0.03f);
    features.highEnergy.assign(frames, 0.02f);
    features.spectralFlux.assign(frames, 0.0f);
    features.lowSpectralFlux.assign(frames, 0.0f);
    features.transientStrength.assign(frames, 0.0f);
    features.energyNovelty.assign(frames, 0.0f);
    features.onsetStrength.assign(frames, 0.0f);

    const double periodFrames = features.sampleRate * 60.0 / bpm;
    constexpr double phaseFrames = 17.0;
    for (int beat = 0;; ++beat) {
        const double ideal = phaseFrames + static_cast<double>(beat) * periodFrames;
        if (ideal >= frames)
            break;
        const int jitter = jitterOnsets ? (beat % 2 == 0 ? -2 : 2) : 0;
        const int frame = std::clamp(
            static_cast<int>(std::lround(ideal)) + jitter, 1, frames - 2);
        const bool downbeat = beat % 4 == downbeatPhase;
        features.onsetStrength[frame] = downbeat ? 1.0f : 0.82f;
        features.spectralFlux[frame] = downbeat ? 0.92f : 0.72f;
        features.lowSpectralFlux[frame] = downbeat ? 1.0f : 0.52f;
        features.lowEnergy[frame] = downbeat ? 1.0f : 0.38f;
        features.energyNovelty[frame] = downbeat ? 0.88f : 0.35f;
        features.transientStrength[frame] = downbeat ? 1.0f : 0.68f;

        // Quiet eighth-note detail must not trick the estimator into double
        // tempo or the grid fitter into shifting alternating beat lines.
        const int offbeat = static_cast<int>(std::lround(ideal + periodFrames * 0.5));
        if (offbeat > 0 && offbeat < frames) {
            features.onsetStrength[offbeat] = 0.18f;
            features.spectralFlux[offbeat] = 0.22f;
        }
    }
    return features;
}

bool tempoAndMetricalLevelAreStable()
{
    bool ok = true;
    analysis::TempoEstimator estimator;
    analysis::BeatTracker tracker;
    analysis::BeatGridFitter fitter;
    for (double expected : {96.0, 120.0, 128.0, 174.0}) {
        const auto features = pulseTrain(expected);
        const auto estimate = estimator.estimate(features);
        ok &= require(!estimate.candidates.empty(), "tempo candidates are produced");
        const bool hasMetricAlternative = std::any_of(
            estimate.candidates.cbegin(), estimate.candidates.cend(),
            [](const analysis::TempoCandidate& candidate) {
                return candidate.source.contains(QStringLiteral("metric-ratio"));
            });
        ok &= require(hasMetricAlternative,
                      "metric ratio candidates remain explicit for grid verification");

        double selectedBpm = 0.0;
        double selectedScore = -1.0;
        struct GridDiagnostic {
            double bpm;
            double combined;
            double confidence;
            double phase;
            double tracking;
        };
        std::vector<GridDiagnostic> gridScores;
        const int candidateCount = std::min<int>(6, estimate.candidates.size());
        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            const auto& candidate = estimate.candidates[static_cast<std::size_t>(candidateIndex)];
            const auto tracked = tracker.track(features, candidate.bpm);
            double refinedBpm = candidate.bpm;
            if (tracked.beats.size() >= 8) {
                const double span = tracked.beats.back().positionSec
                    - tracked.beats.front().positionSec;
                const double observedBpm = span > 0.0
                    ? (static_cast<double>(tracked.beats.size() - 1) * 60.0) / span
                    : 0.0;
                if (observedBpm > 0.0
                    && std::abs(observedBpm - candidate.bpm) / candidate.bpm < 0.08) {
                    refinedBpm = observedBpm;
                }
            }
            const auto fitted = fitter.fit(features, tracked.beats, refinedBpm);
            const double tempoPrior = candidate.score
                / std::max(0.001, estimate.candidates.front().score);
            const double combined = 0.68 * fitted.confidence
                + 0.12 * fitted.phaseScore + 0.12 * tracked.confidence
                + 0.08 * tempoPrior;
            gridScores.push_back({fitted.bpm, combined, fitted.confidence,
                                  fitted.phaseScore, tracked.confidence});
            if (combined > selectedScore) {
                selectedScore = combined;
                selectedBpm = fitted.bpm;
            }
        }
        if (std::abs(selectedBpm - expected) >= 2.0) {
            std::cerr << "  expected=" << expected << " selected=" << selectedBpm
                      << " raw=" << estimate.bpm;
            for (const auto& candidate : estimate.candidates)
                std::cerr << " candidate=" << candidate.bpm << ':' << candidate.score;
            for (const auto& grid : gridScores)
                std::cerr << " grid=" << grid.bpm << ':' << grid.combined
                          << "/fit=" << grid.confidence
                          << "/phase=" << grid.phase
                          << "/track=" << grid.tracking;
            std::cerr << '\n';
        }
        ok &= require(std::abs(selectedBpm - expected) < 2.0,
                      "candidate grid scoring chooses the musical pulse, not half/double time");
    }
    return ok;
}

bool constantGridNeverWobbles()
{
    const auto features = pulseTrain(120.0, 60.0, 0, true);
    analysis::BeatTracker tracker;
    analysis::BeatGridFitter fitter;
    const auto tracked = tracker.track(features, 120.0);
    auto fitted = fitter.fit(features, tracked.beats, 120.0);
    analysis::DownbeatDetector detector;
    detector.detectAndAnnotate(features, fitted.beats);
    bool ok = require(fitted.beats.size() > 80, "full synthetic grid is generated");
    const double expectedPeriod = 0.5;
    for (std::size_t i = 1; i < fitted.beats.size(); ++i) {
        if (std::abs((fitted.beats[i].positionSec
                      - fitted.beats[i - 1].positionSec)
                     - expectedPeriod) >= 1.0e-9) {
            std::cerr << "  spacing[" << i << "]="
                      << fitted.beats[i].positionSec - fitted.beats[i - 1].positionSec
                      << " bpm=" << fitted.bpm << '\n';
        }
        ok &= require(std::abs((fitted.beats[i].positionSec
                               - fitted.beats[i - 1].positionSec)
                              - expectedPeriod) < 1.0e-9,
                      "constant grid spacing does not follow local onset jitter");
    }
    ok &= require(analysis::validateBeatGrid(
                      fitted.beats, fitted.bpm, features.durationSec).ok,
                  "fitted constant grid passes semantic validation");
    const auto metrics = analysis::measureBeatGridQuality(
        fitted, 120.0, 0.17, features.durationSec);
    ok &= require(metrics.bpmError < 0.01,
                  "quality metrics report negligible BPM error for a stable grid");
    ok &= require(metrics.phaseErrorSec < 0.03
                      && metrics.meanBeatErrorSec < 0.03
                      && metrics.percentile95BeatErrorSec < 0.03
                      && metrics.maximumEndOfTrackDriftSec < 0.03,
                  "quality metrics bound phase and end-of-track drift");
    return ok;
}

bool downbeatPhaseUsesRepeatedBassAccents()
{
    auto features = pulseTrain(120.0, 24.0, 2, false);
    std::vector<analysis::BeatMarker> beats;
    for (int beat = 0; beat < 46; ++beat) {
        analysis::BeatMarker marker;
        marker.positionSec = 0.17 + static_cast<double>(beat) * 0.5;
        marker.confidence = 0.8f;
        beats.push_back(marker);
    }
    analysis::DownbeatDetector detector;
    const auto downbeat = detector.detectAndAnnotate(features, beats);
    return require(downbeat.phase == 2, "repeated bass accents select the correct bar phase")
        && require(beats[2].isDownbeat && beats[2].beatInBar == 1,
                   "selected downbeat phase is applied to marker annotations");
}

bool validationRejectsPlausibleLookingWrongTempo()
{
    std::vector<analysis::BeatMarker> beats;
    for (int beat = 0; beat < 32; ++beat) {
        analysis::BeatMarker marker;
        marker.positionSec = static_cast<double>(beat) * 0.5;
        marker.beatInBar = beat % 4 + 1;
        marker.isDownbeat = marker.beatInBar == 1;
        beats.push_back(marker);
    }
    return require(!analysis::validateBeatGrid(beats, 60.0, 20.0).ok,
                   "validation rejects a half-time BPM attached to 120 BPM spacing");
}

} // namespace

int main()
{
    bool ok = true;
    ok &= tempoAndMetricalLevelAreStable();
    ok &= constantGridNeverWobbles();
    ok &= downbeatPhaseUsesRepeatedBassAccents();
    ok &= validationRejectsPlausibleLookingWrongTempo();
    return ok ? 0 : 1;
}
