#include "WaveformAnalysisOrchestrator.h"
#include "analysis/internal/PhraseAnalyzer.h"
#include "analysis/AnalysisTypes.h"
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

        struct CandidateGrid final {
            double bpm = 0.0;
            double tempoPrior = 0.0;
            double combined = -1.0;
            analysis::BeatTrackingResult tracked;
            analysis::BeatGridFitResult fitted;
        };
        const auto evaluateCandidate = [&](double candidateBpm, double tempoPrior) {
            CandidateGrid candidate;
            candidate.bpm = candidateBpm;
            candidate.tempoPrior = tempoPrior;
            candidate.tracked = beatTracker.track(features, candidateBpm);
            double refinedBpm = candidateBpm;
            if (candidate.tracked.beats.size() >= 8) {
                const double span = candidate.tracked.beats.back().positionSec
                    - candidate.tracked.beats.front().positionSec;
                const double observedBpm = span > 0.0
                    ? (static_cast<double>(candidate.tracked.beats.size() - 1) * 60.0) / span
                    : 0.0;
                // Regression only corrects feature-hop quantisation.  It may
                // not fold a metrical ratio into another pulse interpretation.
                if (observedBpm > 0.0
                    && std::abs(observedBpm - candidateBpm) / candidateBpm < 0.08)
                    refinedBpm = observedBpm;
            }
            candidate.fitted = gridFitter.fit(features, candidate.tracked.beats, refinedBpm);
            candidate.combined = 0.68 * candidate.fitted.confidence
                               + 0.12 * candidate.fitted.phaseScore
                               + 0.12 * candidate.tracked.confidence
                               + 0.08 * candidate.tempoPrior;
            return candidate;
        };

        CandidateGrid selected;
        // Metric alternatives now enter TempoEstimator explicitly.  Grid
        // verification remains the deciding signal, which prevents a preferred
        // BPM range from silently rewriting half, double or triplet pulse.
        // Keep the bounded six-candidate budget from the previous pipeline:
        // ratio alternatives compete for those slots, but cannot turn a deck
        // analysis into an unbounded background DSP job.
        const int candidateCount = std::max(1, std::min<int>(6, tempo.candidates.size()));
        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            m_trackData->reportAnalysisProgress(
                0.79 + (static_cast<double>(candidateIndex) / static_cast<double>(candidateCount)) * 0.09,
                true);
            const double candidateBpm = tempo.candidates.empty()
                ? tempo.bpm
                : tempo.candidates[static_cast<size_t>(candidateIndex)].bpm;
            const double tempoPrior = tempo.candidates.empty()
                ? 1.0
                : std::clamp(tempo.candidates[static_cast<size_t>(candidateIndex)].score
                             / std::max(0.001, tempo.candidates.front().score), 0.0, 1.0);
            auto candidate = evaluateCandidate(candidateBpm, tempoPrior);
            if (candidate.combined > selected.combined)
                selected = std::move(candidate);
        }

        // A detected 127.94 BPM can create visible multi-minute drift against
        // an intentional 128.00 master grid.  Verify nearby whole/half BPMs
        // using exactly the same tracker/fitter evidence; never snap merely
        // because an attractive integer is close.  The margin is conservative
        // so noise cannot replace an equally good continuous estimate.
        if (selected.bpm > 0.0) {
            const double snapped = std::round(selected.bpm * 2.0) * 0.5;
            if (std::abs(snapped - selected.bpm) <= 0.18 && snapped > 0.0) {
                auto verified = evaluateCandidate(snapped, selected.tempoPrior);
                constexpr double kIntegerVerificationMargin = 0.01;
                if (verified.combined >= selected.combined + kIntegerVerificationMargin)
                    selected = std::move(verified);
            }
        }

        auto tracked = std::move(selected.tracked);
        auto fitted = std::move(selected.fitted);
        const double bestCombinedGridScore = selected.combined;
        const double selectedTempoPrior = selected.tempoPrior;

        auto downbeat = downbeatDetector.detectAndAnnotate(features, fitted.beats);
        const auto validation = analysis::validateBeatGrid(fitted.beats, fitted.bpm, duration);
        if (!validation.ok)
            qWarning() << "[WaveformAnalyzer] Beatgrid validation:" << validation.message;

        const bool gridPublishable = validation.ok
            && fitted.confidence >= 0.18f
            && tracked.confidence >= 0.10f
            && bestCombinedGridScore >= 0.16;
        if (!gridPublishable) {
            qWarning() << "[WaveformAnalyzer] Suppressing low-confidence beatgrid"
                       << "fit=" << fitted.confidence
                       << "tracked=" << tracked.confidence
                       << "combined=" << bestCombinedGridScore;
            fitted.beats.clear();
            fitted.grid = {};
            fitted.firstBeatSample = 0;
        }

        TrackData::ConfidenceInfo confidence;
        confidence.bpmConfidence = static_cast<float>(std::clamp(
            0.45 * static_cast<double>(tempo.confidence) * selectedTempoPrior
                + 0.55 * static_cast<double>(fitted.confidence),
            0.0, 1.0));
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
                     << "version=" << analysis::kCurrentAnalysisVersion
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

            // Both channels: reading with useReaderRightChan = false makes JUCE
            // duplicate the left channel, so the "mono downmix" below was the
            // left channel alone and a right-panned harmonic never reached the
            // key estimator.
            reader.read(&kfReadBuf, 0, toRead, offset, true, true);

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
