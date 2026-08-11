#include "WaveformAnalysisOrchestrator.h"
#include "analysis/internal/PhraseAnalyzer.h"
#include "analysis/AnalysisCacheVersion.h"
#include "analysis/internal/AnalysisFeatureExtractor.h"
#include "analysis/internal/BeatAnalysis.h"
#include "analysis/AnalysisValidation.h"
#include <keyfinder/keyfinder.h>
#include <keyfinder/audiodata.h>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <vector>

namespace waveform_internal {

bool runAnalysisOrchestrator(const AnalysisOrchestratorInput& input)
{
    auto* m_trackData = input.trackData;
    auto& reader = input.reader;
    auto& thread = input.thread;
    const int m_pointsPerSecond = input.pointsPerSecond;
    const juce::int64 totalSamples = input.totalSamples;
    const double sampleRate = input.sampleRate;
    const double duration = input.duration;
    const bool haveFullWaveform = input.haveFullWaveform;

    auto threadShouldExit = [&]() { return thread.threadShouldExit(); };

    m_trackData->reportAnalysisProgress(haveFullWaveform ? 0.35 : 0.62, true);

    {
        analysis::AnalysisFeatureExtractor::Options featOpts;
        featOpts.frameSize = 2048;
        // 512 samples gives the grid a roughly 12 ms temporal resolution at
        // 44.1 kHz (instead of ~23 ms) and avoids basing a long track's tempo
        // solely on its intro.
        featOpts.hopSize = 512;
        featOpts.maxDurationSec = 0.0;
        analysis::AnalysisFeatureExtractor featureExtractor(featOpts);
        analysis::TempoEstimator tempoEstimator;
        analysis::BeatTracker beatTracker;
        analysis::BeatGridFitter gridFitter;
        analysis::DownbeatDetector downbeatDetector;

        auto features = featureExtractor.extract(reader, &thread,
            [m_trackData](double frac) {
                m_trackData->reportAnalysisProgress(0.62 + frac * 0.16, true);
            });
        if (threadShouldExit()) return false;

        m_trackData->reportAnalysisProgress(0.79, true);

        const auto tempo = tempoEstimator.estimate(features);

        analysis::BeatTrackingResult tracked;
        analysis::BeatGridFitResult fitted;
        double bestCombinedGridScore = -1.0;
        const int candidateCount = std::max(1, std::min<int>(2, tempo.candidates.size()));
        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            m_trackData->reportAnalysisProgress(
                0.79 + (static_cast<double>(candidateIndex) / static_cast<double>(candidateCount)) * 0.09,
                true);
            const double candidateBpm = tempo.candidates.empty()
                ? tempo.bpm
                : tempo.candidates[static_cast<size_t>(candidateIndex)].bpm;
            auto candidateTracked = beatTracker.track(features, candidateBpm);
            double refinedBpm = candidateBpm;
            if (candidateTracked.beats.size() >= 8) {
                const double span = candidateTracked.beats.back().positionSec
                    - candidateTracked.beats.front().positionSec;
                const double observedBpm = span > 0.0
                    ? (static_cast<double>(candidateTracked.beats.size() - 1) * 60.0) / span
                    : 0.0;
                // The tracker regression removes hop quantisation. Keep the
                // original candidate if the observed period describes another
                // metrical level (half/double tempo) instead.
                if (observedBpm > 0.0
                    && std::abs(observedBpm - candidateBpm) / candidateBpm < 0.08)
                    refinedBpm = observedBpm;
            }
            auto candidateFit = gridFitter.fit(features, candidateTracked.beats, refinedBpm);

            const double tempoPrior = tempo.candidates.empty()
                ? 1.0
                : std::clamp(tempo.candidates[static_cast<size_t>(candidateIndex)].score
                             / std::max(0.001, tempo.candidates.front().score), 0.0, 1.0);
            const double combined = 0.76 * candidateFit.confidence
                                  + 0.18 * candidateFit.phaseScore
                                  + 0.06 * tempoPrior;
            if (combined > bestCombinedGridScore) {
                bestCombinedGridScore = combined;
                tracked = std::move(candidateTracked);
                fitted = std::move(candidateFit);
            }
        }

        auto downbeat = downbeatDetector.detectAndAnnotate(features, fitted.beats);
        const auto validation = analysis::validateBeatGrid(fitted.beats, fitted.bpm, duration);
        if (!validation.ok)
            qWarning() << "[WaveformAnalyzer] Beatgrid validation:" << validation.message;

        TrackData::ConfidenceInfo confidence;
        confidence.bpmConfidence = tempo.confidence;
        confidence.beatConfidence = tracked.confidence;
        confidence.downbeatConfidence = downbeat.confidence;
        confidence.gridConfidence = fitted.confidence;

        if (!fitted.beats.empty()) {
            fitted.firstBeatSample = static_cast<qint64>(
                std::llround(fitted.beats.front().positionSec * sampleRate));
        }

        const double finalBpm = fitted.bpm > 0.0 ? fitted.bpm : tempo.bpm;
        if (finalBpm > 0.0) {
            if (!m_trackData->beatgridLockedByUser()) {
                m_trackData->setBpmData(finalBpm,
                                        fitted.firstBeatSample,
                                        sampleRate,
                                        std::move(fitted.beats),
                                        confidence,
                                        std::move(fitted.grid));
            } else {
                m_trackData->setBpmData(finalBpm,
                                        m_trackData->getFirstBeatSample(),
                                        sampleRate,
                                        {},
                                        confidence,
                                        m_trackData->getBeatGridInfo());
            }

            QString candidateLog;
            const int show = std::min<int>(5, tempo.candidates.size());
            for (int i = 0; i < show; ++i) {
                const double relConfidence = tempo.candidates.empty()
                    ? 0.0
                    : std::clamp(tempo.candidates[static_cast<size_t>(i)].score
                                 / std::max(0.001, tempo.candidates.front().score), 0.0, 1.0);
                candidateLog += QStringLiteral("%1(%2,%3) ")
                    .arg(tempo.candidates[static_cast<size_t>(i)].bpm, 0, 'f', 2)
                    .arg(relConfidence, 0, 'f', 2)
                    .arg(tempo.candidates[static_cast<size_t>(i)].source);
            }

            const bool downbeatTrusted = downbeat.confidence >= 0.42f;
            qDebug() << "[WaveformAnalyzer] Internal rhythm analysis"
                     << "version=" << analysis::kAnalysisVersion
                     << "bpm=" << finalBpm
                     << "selectedOffsetSec=" << fitted.selectedOffsetSec
                     << "stableRegion=" << fitted.stableRegionStartSec << "->" << fitted.stableRegionEndSec
                     << "phaseScore=" << fitted.phaseScore
                     << "bpmConf=" << confidence.bpmConfidence
                     << "beatConf=" << confidence.beatConfidence
                     << "downbeatConf=" << confidence.downbeatConfidence
                     << "downbeatTrusted=" << downbeatTrusted
                     << "gridConf=" << confidence.gridConfidence
                     << "gridType=" << (fitted.grid.type == TrackData::BeatGridType::DynamicTempo ? "dynamic" : "constant")
                     << "candidates=" << candidateLog;
        }

        std::vector<TrackSegment> segments;
        const auto beatGrid = m_trackData->getBeatGrid();
        if (beatGrid.size() >= 17 && duration > 12.0) {
            PhraseAnalyzer phraseAnalyzer;
            segments = phraseAnalyzer.analyze(features, beatGrid, duration);
        }
        m_trackData->setSegmentsData(std::move(segments));
    }

    if (threadShouldExit()) return false;

    m_trackData->reportAnalysisProgress(0.91, true);

    // -------------------------------------------------------------------------
    // Stage 5: Key detection via libKeyFinder.
    //
    // We feed the entire track as a mono downmix (first ~90 s is sufficient for
    // accuracy) into KeyFinder::KeyFinder::keyOfAudio(). libKeyFinder handles
    // its own chromagram + Krumhansl-Schmuckler internally; we just map the
    // returned key_t enum to Camelot Wheel notation.
    // -------------------------------------------------------------------------
    {
        // Limit to the first 90 seconds to keep analysis fast.
        const double maxKeySeconds = 90.0;
        const juce::int64 keySamples = std::min(
            totalSamples,
            static_cast<juce::int64>(maxKeySeconds * sampleRate));

        KeyFinder::AudioData kfAudio;
        kfAudio.setChannels(1);
        kfAudio.setFrameRate(static_cast<unsigned int>(sampleRate));
        kfAudio.addToFrameCount(static_cast<unsigned int>(keySamples));

        const int kfBlockSize = 8192;
        juce::AudioBuffer<float> kfReadBuf(static_cast<int>(reader.numChannels), kfBlockSize);

        for (juce::int64 offset = 0; offset < keySamples; offset += kfBlockSize) {
            if (threadShouldExit()) break;

            const int toRead = static_cast<int>(
                std::min(static_cast<juce::int64>(kfBlockSize), keySamples - offset));

            reader.read(&kfReadBuf, 0, toRead, offset, true, false);

            const int numCh2 = static_cast<int>(reader.numChannels);
            for (int s = 0; s < toRead; ++s) {
                float mono = 0.0f;
                for (int ch = 0; ch < numCh2; ++ch)
                    mono += kfReadBuf.getReadPointer(ch)[s];
                mono /= static_cast<float>(numCh2);
                kfAudio.setSample(static_cast<unsigned int>(offset + s), mono);
            }
        }

        if (!threadShouldExit()) {
            // Camelot Wheel mapping for key_t (0=A_MAJOR … 23=A_FLAT_MINOR, 24=SILENCE).
            // Order matches KeyFinder::key_t enum in constants.h.
            static const char* kCamelot[25] = {
                "11B",  // A_MAJOR
                "8A",   // A_MINOR
                "6B",   // B_FLAT_MAJOR
                "3A",   // B_FLAT_MINOR
                "1B",   // B_MAJOR
                "10A",  // B_MINOR
                "8B",   // C_MAJOR
                "5A",   // C_MINOR
                "3B",   // D_FLAT_MAJOR
                "12A",  // D_FLAT_MINOR
                "10B",  // D_MAJOR
                "7A",   // D_MINOR
                "5B",   // E_FLAT_MAJOR
                "2A",   // E_FLAT_MINOR
                "12B",  // E_MAJOR
                "9A",   // E_MINOR
                "7B",   // F_MAJOR
                "4A",   // F_MINOR
                "2B",   // G_FLAT_MAJOR
                "11A",  // G_FLAT_MINOR
                "9B",   // G_MAJOR
                "6A",   // G_MINOR
                "4B",   // A_FLAT_MAJOR
                "1A",   // A_FLAT_MINOR
                ""      // SILENCE
            };

            try {
                KeyFinder::KeyFinder kf;
                KeyFinder::key_t key = kf.keyOfAudio(kfAudio);
                QString camelot = (key < 25) ? QString::fromLatin1(kCamelot[key]) : QString();
                if (!camelot.isEmpty())
                    m_trackData->setKeyData(camelot);
            } catch (const std::exception& e) {
                qWarning() << "[WaveformAnalyzer] libKeyFinder error:" << e.what();
            }
        }
    }

    m_trackData->reportAnalysisProgress(0.97, true);

    return !thread.threadShouldExit();
}

} // namespace waveform_internal
