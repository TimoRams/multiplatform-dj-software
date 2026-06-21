#include "WaveformAnalysisOrchestrator.h"
#include "WaveformAnalyzer.h"
#include "WaveformCache.h"
#include "PhraseAnalyzer.h"
#include "AnalysisCacheVersion.h"
#include "AnalysisFeatureExtractor.h"
#include "TempoEstimator.h"
#include "BeatTracker.h"
#include "DownbeatDetector.h"
#include "BeatGridFitter.h"
#include "AnalysisValidation.h"
#include <keyfinder/keyfinder.h>
#include <keyfinder/audiodata.h>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
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

    // -------------------------------------------------------------------------
    // Stage 4: BPM detection + elastic beat-grid alignment.
    //
    //  1. ENERGY GATE  — median RMS pre-scan; low-energy hops are skipped.
    //
    //  2. SINGLE DUAL-METHOD PASS (specdiff + hfc, full track)
    //     One pass over the complete audio. Both aubio_tempo instances run in
    //     parallel on every hop; their beat timestamps are merged and deduplicated
    //     (< 100 ms apart). This eliminates the 3× duplication bug that was
    //     polluting the histogram with triple-counted votes.
    //
    //  3. 16-BEAT MACRO-DISTANCE HISTOGRAM
    //     Measure BPM over 16-beat intervals to average out hop-size quantisation
    //     (±1.3 BPM → ±0.08 BPM). Votes go into a 0.5-BPM binned histogram.
    //     Winning bin is found, then half-time / double-time correction applied.
    //     Final BPM is the weighted average of raw values within ±0.25 BPM of
    //     the winner (un-quantised BPM for maximum grid accuracy).
    //
    //  4. 3-STAGE COMB FILTER (coarse → fine → ultra-fine)
    //     Stage 1: 256-sample steps over the scan window.
    //     Stage 2: 16-sample steps within ±256 samples of the coarse peak.
    //     Stage 3: 1-sample steps within ±16 samples of the fine peak.
    //     Achieves sub-sample (< 0.025 ms) grid alignment without brute force.
    //
    //  5. ROBUST WALK-BACK
    //     Walks backwards in beat periods from the comb anchor. Requires TWO
    //     consecutive below-threshold positions to stop — prevents early stopping
    //     on quiet synth pads that happen to be above 15 % of track peak.
    //
    //  6. ELASTIC BEAT-GRID (per-beat transient micro-snap)
    //     After the rigid grid is locked, we build a full beat-timestamp array.
    //     For every beat, we search rawBins in a ±15 ms window and snap the
    //     timestamp to the highest low-band peak. This gives a beat array where
    //     EVERY grid line sits exactly on the kick-drum transient, not on a
    //     mathematical average. The array is stored in TrackData as the elastic
    //     beat grid for pixel-perfect rendering.
    // -------------------------------------------------------------------------
    {
#if 0
        std::map<double, int>                  bpmHistogram;
        std::map<double, std::vector<double>>  bpmRawValues;
        std::vector<uint64_t> beatPositions;
        std::vector<double>   allBeats;
        double essentiaBpmHint = 0.0;

        // ── Essentia beat + BPM extraction ───────────────────────────────────
        {
#ifdef ESSENTIA_FOUND
            static std::once_flag essentiaInitOnce;
            std::call_once(essentiaInitOnce, []() {
                essentia::init();
            });

            try {
                std::vector<essentia::Real> monoSignal;
                monoSignal.reserve(static_cast<size_t>(totalSamples));

                const int blockSize = 8192;
                juce::AudioBuffer<float> readBufMono(static_cast<int>(reader.numChannels), blockSize);

                for (juce::int64 offset = 0; offset < totalSamples; offset += blockSize) {
                    if (threadShouldExit()) break;
                    const int toRead = static_cast<int>(
                        std::min(static_cast<juce::int64>(blockSize), totalSamples - offset));
                    reader.read(&readBufMono, 0, toRead, offset, true, false);

                    const int nc = static_cast<int>(reader.numChannels);
                    for (int s = 0; s < toRead; ++s) {
                        float mono = 0.0f;
                        for (int ch = 0; ch < nc; ++ch)
                            mono += readBufMono.getReadPointer(ch)[s];
                        monoSignal.push_back(static_cast<essentia::Real>(mono / static_cast<float>(nc)));
                    }
                }

                if (!threadShouldExit() && !monoSignal.empty()) {
                    // RGB waveform feature extraction (FrameCutter -> Windowing -> Spectrum -> EnergyBand).
                    // This provides one color+RMS sample per analysis frame for QML painting.
                    QVector<TrackData::RgbWaveformFrame> rgbFrames =
                        analyzeRgbFramesWithEssentia(monoSignal, sampleRate, numPoints, 2048, 1024);
                    if (!rgbFrames.isEmpty()) {
                        const auto currentRgb = m_trackData->getRgbWaveformData();
                        auto blended = blendRgbPreferDynamics(currentRgb, rgbFrames);
                        m_trackData->setRgbWaveformData(std::move(blended));
                    }

                    using essentia::standard::Algorithm;
                    using essentia::standard::AlgorithmFactory;

                    essentia::Real bpm = 0.0f;
                    essentia::Real confidence = 0.0f;
                    std::vector<essentia::Real> ticks;
                    std::vector<essentia::Real> estimates;
                    std::vector<essentia::Real> bpmIntervals;

                    std::unique_ptr<Algorithm> rhythm(
                        AlgorithmFactory::create("RhythmExtractor2013",
                                                 "method", std::string("multifeature")));

                    rhythm->input("signal").set(monoSignal);
                    rhythm->output("bpm").set(bpm);
                    rhythm->output("ticks").set(ticks);
                    rhythm->output("confidence").set(confidence);
                    rhythm->output("estimates").set(estimates);
                    rhythm->output("bpmIntervals").set(bpmIntervals);
                    rhythm->compute();

                    essentiaBpmHint = static_cast<double>(bpm);

                    beatPositions.reserve(ticks.size());
                    allBeats.reserve(ticks.size());
                    for (essentia::Real t : ticks) {
                        const double sec = static_cast<double>(t);
                        if (sec < 0.0 || sec >= duration)
                            continue;
                        allBeats.push_back(sec);
                        beatPositions.push_back(static_cast<uint64_t>(sec * sampleRate));
                    }

                    std::sort(allBeats.begin(), allBeats.end());
                    std::sort(beatPositions.begin(), beatPositions.end());
                    {
                        const uint64_t minSpacing = static_cast<uint64_t>(0.05 * sampleRate);
                        auto last = std::unique(beatPositions.begin(), beatPositions.end(),
                                                [minSpacing](uint64_t a, uint64_t b) {
                                                    return (b - a) < minSpacing;
                                                });
                        beatPositions.erase(last, beatPositions.end());
                    }

                    qDebug() << "[WaveformAnalyzer] Essentia BPM:" << essentiaBpmHint
                             << "confidence:" << static_cast<double>(confidence)
                             << "ticks:" << beatPositions.size();
                }
            } catch (const std::exception& e) {
                qWarning() << "[WaveformAnalyzer] Essentia BPM extraction failed:" << e.what();
            } catch (...) {
                qWarning() << "[WaveformAnalyzer] Essentia BPM extraction failed with unknown error.";
            }
#else
        qWarning() << "[WaveformAnalyzer] ESSENTIA_FOUND not set, using internal BPM fallback analysis.";
#endif
        }

        // Fallback: if Essentia did not yield a BPM, estimate tempo from the
        // already computed low-band waveform envelope via lag autocorrelation.
        if (essentiaBpmHint <= 0.0 && !rawBins.empty()) {
            const int fps = std::max(1, m_pointsPerSecond);
            const int minLag = std::max(1, static_cast<int>(std::floor((fps * 60.0) / 200.0)));
            const int maxLag = std::max(minLag + 1, static_cast<int>(std::ceil((fps * 60.0) / 60.0)));

            std::vector<double> env;
            env.reserve(rawBins.size());
            for (const auto& b : rawBins)
                env.push_back(static_cast<double>(b.low + 0.35f * b.lowMid));

            if (static_cast<int>(env.size()) > maxLag + 8) {
                const double mean = std::accumulate(env.begin(), env.end(), 0.0) / static_cast<double>(env.size());
                for (double& v : env)
                    v -= mean;

                int bestLag = minLag;
                double bestScore = -1.0;
                for (int lag = minLag; lag <= maxLag; ++lag) {
                    double s = 0.0;
                    for (size_t i = static_cast<size_t>(lag); i < env.size(); ++i)
                        s += env[i] * env[i - static_cast<size_t>(lag)];
                    if (s > bestScore) {
                        bestScore = s;
                        bestLag = lag;
                    }
                }

                if (bestScore > 0.0)
                    essentiaBpmHint = (fps * 60.0) / static_cast<double>(bestLag);
            }

            if (essentiaBpmHint > 0.0)
                qDebug() << "[WaveformAnalyzer] BPM fallback estimate:" << essentiaBpmHint;
        }

        // ── 16-beat macro-distance histogram ────────────────────────────────
        if (beatPositions.size() >= 17) {
            for (size_t i = 16; i < beatPositions.size(); ++i) {
                uint64_t delta = beatPositions[i] - beatPositions[i - 16];
                if (delta == 0) continue;
                double instantBpm = (16.0 * sampleRate * 60.0) / static_cast<double>(delta);
                if (instantBpm < 60.0 || instantBpm > 200.0) continue;
                double binnedBpm = std::round(instantBpm * 2.0) / 2.0;
                bpmHistogram[binnedBpm]++;
                bpmRawValues[binnedBpm].push_back(instantBpm);
            }
        }

        // ── Peak finding ─────────────────────────────────────────────────────
        double estimatedBpm = 0.0;
        if (!bpmHistogram.empty()) {
            double peakBpm   = 0.0;
            int    peakCount = 0;
            for (auto& [bpm, count] : bpmHistogram) {
                if (count > peakCount) { peakCount = count; peakBpm = bpm; }
            }

            // ── Half-time / Double-time correction ───────────────────────────
            //
            // HALF-TIME  (detected 2× too fast, e.g. Trap/DnB/Future Bass):
            //   Trigger when peak > 135 BPM. Look for peakBpm/2 in the histogram
            //   with a ±1.0 BPM fuzzy window (handles bin-boundary artefacts where
            //   the true 85.0 landed in the 85.5 bin because of float rounding).
            //
            // DOUBLE-TIME (detected 2× too slow, e.g. slow-burn house misread as 65):
            //   Trigger when peak < 80 BPM. Check if peakBpm*2 has histogram votes
            //   and falls in the DJ range [80, 160].
            //
            // Helper: find the histogram bin with the highest vote count within
            // [target - radius, target + radius].  Returns 0 if nothing found.
            auto histVotesNear = [&](double target, double radius) -> int {
                int best = 0;
                for (auto& [bin, cnt] : bpmHistogram)
                    if (std::abs(bin - target) <= radius && cnt > best)
                        best = cnt;
                return best;
            };

            if (peakBpm > 135.0) {
                double halfBpm = std::round((peakBpm / 2.0) * 2.0) / 2.0;
                if (halfBpm >= 60.0 && halfBpm <= 135.0) {
                    int halfVotes = histVotesNear(halfBpm, 1.0);
                    if (halfVotes > 0) {
                        qDebug() << "[WaveformAnalyzer] Half-time correction:"
                                 << peakBpm << "->" << halfBpm
                                 << "(halfVotes:" << halfVotes << ")";
                        peakBpm = halfBpm;
                    }
                }
            } else if (peakBpm < 80.0) {
                double doubleBpm = std::round((peakBpm * 2.0) * 2.0) / 2.0;
                if (doubleBpm >= 80.0 && doubleBpm <= 160.0) {
                    int doubleVotes = histVotesNear(doubleBpm, 1.0);
                    if (doubleVotes > 0) {
                        qDebug() << "[WaveformAnalyzer] Double-time correction:"
                                 << peakBpm << "->" << doubleBpm
                                 << "(doubleVotes:" << doubleVotes << ")";
                        peakBpm = doubleBpm;
                    }
                }
            }

            // ── Weighted-average BPM (un-quantised) ──────────────────────────
            {
                double rawSum = 0.0; int rawCount = 0;
                for (auto& [bin, vals] : bpmRawValues) {
                    if (std::abs(bin - peakBpm) <= 0.25) {
                        for (double v : vals) { rawSum += v; ++rawCount; }
                    }
                }
                estimatedBpm = rawCount > 0 ? rawSum / rawCount : peakBpm;
            }

            // Debug: top-5 histogram.
            std::vector<std::pair<double,int>> sorted(bpmHistogram.begin(), bpmHistogram.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](auto& a, auto& b){ return a.second > b.second; });
            int show = std::min(static_cast<int>(sorted.size()), 5);
            QString top5;
            for (int i = 0; i < show; ++i)
                top5 += QString("%1(%2) ").arg(sorted[i].first).arg(sorted[i].second);
            qDebug() << "[WaveformAnalyzer] 16-beat histogram top-5:" << top5
                     << "-> winner:" << estimatedBpm
                     << "(totalBeats:" << beatPositions.size() << ")";
        }

        // If the histogram cannot lock a stable tempo, fall back to Essentia's
        // direct BPM estimate so the deck still gets a usable BPM value.
        if (estimatedBpm <= 0.0 && essentiaBpmHint > 0.0)
            estimatedBpm = essentiaBpmHint;

        // ── Grid alignment ───────────────────────────────────────────────────
        qint64 firstBeatSample = 0;
        std::vector<TrackData::BeatMarker> finalBeatGrid;

        if (estimatedBpm > 0.0 && beatPositions.size() >= 16) {
            // ── STEP 1: Aubio latency compensation ────────────────────────────
            const uint64_t latComp = 512; // Keep previous tempo-detector latency compensation.
            for (auto& pos : beatPositions)
                pos = (pos >= latComp) ? (pos - latComp) : 0;

            const double samplesPerBeat = (sampleRate * 60.0) / estimatedBpm;
            const double tolerance      = 0.005 * sampleRate; // ±5 ms

            // ── STEP 2: 3-Stage Comb Filter ───────────────────────────────────
            // Scores a phase offset by counting how many beatPositions land within
            // `tolerance` of any grid line anchored at that offset.
            auto scorePhase = [&](uint64_t phase) -> int {
                int score = 0;
                for (uint64_t beat : beatPositions) {
                    double diff = static_cast<double>(beat) - static_cast<double>(phase);
                    double err  = diff - samplesPerBeat * std::round(diff / samplesPerBeat);
                    if (std::abs(err) <= tolerance) ++score;
                }
                return score;
            };

            uint64_t scanLimit = static_cast<uint64_t>(4.0 * samplesPerBeat);
            if (!beatPositions.empty())
                scanLimit = std::max(scanLimit,
                                     beatPositions[0] + static_cast<uint64_t>(samplesPerBeat));

            // Stage 1: 256-sample steps.
            uint64_t bestPhase = 0;
            int      bestScore = -1;
            for (uint64_t p = 0; p < scanLimit; p += 256) {
                int s = scorePhase(p);
                if (s > bestScore) { bestScore = s; bestPhase = p; }
            }

            // Stage 2: 16-sample steps, ±256 samples around stage-1 peak.
            {
                uint64_t lo = (bestPhase >= 256) ? bestPhase - 256 : 0;
                uint64_t hi = bestPhase + 256;
                for (uint64_t p = lo; p <= hi; p += 16) {
                    int s = scorePhase(p);
                    if (s > bestScore) { bestScore = s; bestPhase = p; }
                }
            }

            // Stage 3: 1-sample steps, ±16 samples around stage-2 peak.
            {
                uint64_t lo = (bestPhase >= 16) ? bestPhase - 16 : 0;
                uint64_t hi = bestPhase + 16;
                for (uint64_t p = lo; p <= hi; ++p) {
                    int s = scorePhase(p);
                    if (s > bestScore) { bestScore = s; bestPhase = p; }
                }
            }

            qDebug() << "[WaveformAnalyzer] 3-stage comb filter:"
                     << "anchor=" << bestPhase << "samples"
                     << "(" << (static_cast<double>(bestPhase)/sampleRate*1000.0) << "ms)"
                     << "score=" << bestScore << "/" << beatPositions.size();

            // ── STEP 3: Robust walk-back (2-consecutive-miss rule) ─────────────
            // Walk backwards until TWO consecutive candidate positions are below
            // the energy threshold — prevents early stopping on quiet pads in the
            // intro that happen to be above 15 % of the global peak.
            const unsigned int rmsHop          = 512;
            const float   energyThreshold = globalMaxPeak * 0.15f;
            juce::AudioBuffer<float> rmsBuf(static_cast<int>(reader.numChannels),
                                            static_cast<int>(rmsHop));

            auto peakAt = [&](juce::int64 sample) -> float {
                reader.read(&rmsBuf, 0, static_cast<int>(rmsHop), sample, true, false);
                float pk = 0.0f;
                const int nc = static_cast<int>(reader.numChannels);
                for (int ch = 0; ch < nc; ++ch) {
                    const float* p = rmsBuf.getReadPointer(ch);
                    for (unsigned int s = 0; s < rmsHop; ++s)
                        pk = std::max(pk, std::abs(p[s]));
                }
                return pk;
            };

            uint64_t gridAnchor = bestPhase;
            int      missCount  = 0;
            while (true) {
                int64_t prev = static_cast<int64_t>(gridAnchor)
                             - static_cast<int64_t>(samplesPerBeat);
                if (prev < 0) break;
                if (peakAt(static_cast<juce::int64>(prev)) < energyThreshold) {
                    if (++missCount >= 2) break;  // two consecutive misses = real silence
                } else {
                    missCount = 0;
                    gridAnchor = static_cast<uint64_t>(prev);
                }
            }

            firstBeatSample = static_cast<qint64>(gridAnchor);

            // ── STEP 4: Micro-snap firstBeatSample ───────────────────────────
            {
                const double snapWindowSec = 0.015; // ±15 ms
                const double secPerBin     = 1.0 / static_cast<double>(m_pointsPerSecond);
                int centerBin = static_cast<int>(
                    static_cast<double>(firstBeatSample) / sampleRate * m_pointsPerSecond);
                int halfWin = static_cast<int>(std::ceil(snapWindowSec / secPerBin));
                int lo = std::max(0, centerBin - halfWin);
                int hi = std::min(static_cast<int>(rawBins.size()) - 1, centerBin + halfWin);
                float bestPeak = -1.0f; int bestBin = centerBin;
                for (int b = lo; b <= hi; ++b) {
                    if (rawBins[static_cast<size_t>(b)].low > bestPeak) {
                        bestPeak = rawBins[static_cast<size_t>(b)].low; bestBin = b;
                    }
                }
                qint64 snapped = static_cast<qint64>(
                    (static_cast<double>(bestBin) + 0.5) / m_pointsPerSecond * sampleRate);
                qDebug() << "[WaveformAnalyzer] firstBeat micro-snap:"
                         << ((snapped - firstBeatSample) * 1000.0 / sampleRate) << "ms";
                firstBeatSample = snapped;
            }

            // ── STEP 5: Build elastic beat-grid with per-beat micro-snap ──────
            // Generate beat timestamps mathematically (firstBeat + n * period),
            // then snap each one to the nearest rawBin transient peak (±15 ms).
            // Result: an array where every grid line sits exactly on the kick.
            {
                const double snapWindowSec = 0.015;
                const double secPerBin     = 1.0 / static_cast<double>(m_pointsPerSecond);
                const int    halfWin       = static_cast<int>(std::ceil(snapWindowSec / secPerBin));
                const int    numRawBins    = static_cast<int>(rawBins.size());

                finalBeatGrid.reserve(static_cast<size_t>(
                    totalSamples / samplesPerBeat) + 4);

                // Walk forward from firstBeatSample to end of track.
                // Beat index n: isDownbeat = (n % 4 == 0), barNumber = n/4 + 1.
                for (int n = 0; ; ++n) {
                    double beatSec = static_cast<double>(firstBeatSample) / sampleRate
                                   + n * (60.0 / estimatedBpm);
                    if (beatSec >= duration) break;

                    // Convert to rawBin index and search ±15 ms for the highest
                    // low-band peak (kick transient).
                    int centerBin = static_cast<int>(beatSec * m_pointsPerSecond);
                    int lo = std::max(0, centerBin - halfWin);
                    int hi = std::min(numRawBins - 1, centerBin + halfWin);

                    float bestPeak = -1.0f;
                    int   bestBin  = centerBin;
                    for (int b = lo; b <= hi; ++b) {
                        if (rawBins[static_cast<size_t>(b)].low > bestPeak) {
                            bestPeak = rawBins[static_cast<size_t>(b)].low;
                            bestBin  = b;
                        }
                    }

                    // Snap to centre of best bin (sub-bin precision in seconds).
                    double snappedSec = (static_cast<double>(bestBin) + 0.5)
                                      / static_cast<double>(m_pointsPerSecond);

                    finalBeatGrid.push_back(TrackData::BeatMarker{
                        snappedSec,
                        true,
                        (n % 4 == 0),
                        n / 4,
                        (n / 4) + 1,
                        (n % 4) + 1,
                    });
                }

                // ── STEP 6: Downbeat phase correction (bar-aware) ─────────────
                // Select the strongest bar phase (mod 4) from low/low-mid energy,
                // then re-anchor downbeat flags so beat-1 markers are more stable.
                if (finalBeatGrid.size() >= 16 && numRawBins > 0) {
                    double phaseScore[4] = {0.0, 0.0, 0.0, 0.0};

                    for (size_t i = 0; i < finalBeatGrid.size(); ++i) {
                        const int bin = static_cast<int>(
                            std::lround(finalBeatGrid[i].positionSec * m_pointsPerSecond));
                        if (bin < 0 || bin >= numRawBins)
                            continue;

                        const auto& rb = rawBins[static_cast<size_t>(bin)];
                        const double w = static_cast<double>(rb.low)
                                       + 0.60 * static_cast<double>(rb.lowMid)
                                       + 0.20 * static_cast<double>(rb.mid);
                        phaseScore[i % 4] += w;
                    }

                    int bestPhase = 0;
                    for (int p = 1; p < 4; ++p) {
                        if (phaseScore[p] > phaseScore[bestPhase])
                            bestPhase = p;
                    }

                    // Apply only when the alternative phase is clearly stronger
                    // than the current assumption (phase 0).
                    if (bestPhase != 0 && phaseScore[bestPhase] > phaseScore[0] * 1.08) {
                        firstBeatSample = static_cast<qint64>(
                            std::llround(finalBeatGrid[static_cast<size_t>(bestPhase)].positionSec * sampleRate));

                        for (size_t i = 0; i < finalBeatGrid.size(); ++i) {
                            const int rel = static_cast<int>(i) - bestPhase;
                            const int mod4 = ((rel % 4) + 4) % 4;
                            finalBeatGrid[i].isDownbeat = (mod4 == 0);
                            finalBeatGrid[i].beatInBar = mod4 + 1;
                            finalBeatGrid[i].barIndex = static_cast<int>(
                                std::floor(static_cast<double>(rel) / 4.0));
                            finalBeatGrid[i].barNumber = finalBeatGrid[i].barIndex + 1;
                        }

                        qDebug() << "[WaveformAnalyzer] Downbeat phase corrected:"
                                 << "phase0=" << phaseScore[0]
                                 << "phaseBest=" << bestPhase
                                 << "score=" << phaseScore[bestPhase];
                    }
                }

                qDebug() << "[WaveformAnalyzer] Elastic beat-grid built:"
                         << finalBeatGrid.size() << "beats"
                         << "| firstBeat=" << firstBeatSample
                         << "(" << (static_cast<double>(firstBeatSample)/sampleRate*1000.0) << "ms)"
                         << "| samplesPerBeat=" << static_cast<uint64_t>(samplesPerBeat);
            }
        }

        if (estimatedBpm > 0.0) {
            // Do not overwrite a beatgrid that was already restored from DB
            // (e.g. manually shifted downbeat). Keep cached grid stable on reload.
            const bool hasExistingGrid = m_trackData->isBpmAnalyzed()
                && !m_trackData->getBeatGrid().empty();
            if (!hasExistingGrid) {
                m_trackData->setBpmData(estimatedBpm, firstBeatSample, sampleRate,
                                        std::move(finalBeatGrid));
            }
        }

        // -----------------------------------------------------------------
        // Stage 4.5: Phrase-based segment detection via 16-beat block analysis.
        // -----------------------------------------------------------------
        {
            std::vector<double> beatTimes;
            beatTimes.reserve(finalBeatGrid.size() + allBeats.size());
            if (!finalBeatGrid.empty()) {
                for (const auto& b : finalBeatGrid)
                    beatTimes.push_back(b.positionSec);
            } else {
                beatTimes = allBeats;
            }

            std::vector<TrackSegment> segments;
            if (beatTimes.size() >= 17 && duration > 12.0) {
                PhraseAnalyzer phraseAnalyzer;
                segments = phraseAnalyzer.analyze(*reader, beatTimes, duration);
            }

            m_trackData->setSegmentsData(std::move(segments));
        }
    }
#endif
    }

    {
        analysis::AnalysisFeatureExtractor::Options featOpts;
        featOpts.frameSize = 2048;
        featOpts.hopSize = 1024;
        featOpts.maxDurationSec = 420.0;
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
            auto candidateFit = gridFitter.fit(features, candidateTracked.beats, candidateBpm);

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
