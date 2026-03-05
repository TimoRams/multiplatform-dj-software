#include "WaveformAnalyzer.h"
#include <QDebug>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <complex>
#include <map>

// aubio: BPM / beat tracking
#include <aubio/aubio.h>

// libKeyFinder: key detection
#include <keyfinder/keyfinder.h>
#include <keyfinder/audiodata.h>

WaveformAnalyzer::WaveformAnalyzer(TrackData* trackData, juce::AudioFormatManager* formatManager, int pointsPerSecond)
    : juce::Thread("WaveformAnalyzerThread"), m_trackData(trackData), m_formatManager(formatManager), m_pointsPerSecond(pointsPerSecond)
{
}

WaveformAnalyzer::~WaveformAnalyzer()
{
    stopAnalysis();
}

void WaveformAnalyzer::startAnalysis(const QString& filePath)
{
    stopAnalysis();
    m_filePath = filePath;
    startThread();
}

void WaveformAnalyzer::stopAnalysis()
{
    signalThreadShouldExit();
    stopThread(2000);
}

// Envelope follower with separate attack and release time constants.
// Used for transient detection: a fast follower tracks peaks while a slow
// follower tracks the sustained RMS body.  Their difference isolates
// sharp drum hits (positive crest-factor spikes).
struct EnvelopeFollower {
    float state = 0.0f;
    float attackCoef  = 0.0f;
    float releaseCoef = 0.0f;

    void prepare(double sampleRate, float attackMs, float releaseMs) {
        // attackMs == 0 → instant attack (coefficient = 0 → state = input immediately)
        attackCoef  = (attackMs  > 0.0f)
            ? std::exp(-1.0f / (static_cast<float>(sampleRate) * attackMs  * 0.001f))
            : 0.0f;
        releaseCoef = (releaseMs > 0.0f)
            ? std::exp(-1.0f / (static_cast<float>(sampleRate) * releaseMs * 0.001f))
            : 0.0f;
    }

    float process(float rectified) {
        float coef = rectified > state ? attackCoef : releaseCoef;
        state = rectified + coef * (state - rectified);
        return state;
    }

    void reset() { state = 0.0f; }
};

// Linkwitz-Riley 4th-order crossover (LR4, -24 dB/oct).
// Uses juce::dsp::LinkwitzRileyFilter with the two-output processSample()
// overload that returns phase-aligned LP and HP in one call.
//
// Phase-compensated 3-band architecture (perfect reconstruction):
//   xoverLow  (150 Hz):   mono → rawLow + midHigh
//   xoverHigh (2500 Hz):  midHigh → mid + high
//   allpassComp (2500 Hz): rawLow → low  (phase-aligned with mid/high)
//
// Without the allpass on the LOW band, bass transients arrive earlier
// than the mid/high components of the same drum hit, causing temporal
// smearing in the visual display.

void WaveformAnalyzer::run()
{
    juce::File file(m_filePath.toStdString());
    if (!file.existsAsFile()) return;

    std::unique_ptr<juce::AudioFormatReader> reader(m_formatManager->createReaderFor(file));
    if (!reader) return;

    m_trackData->clear();

    const juce::int64 totalSamples = reader->lengthInSamples;
    const double      sampleRate   = reader->sampleRate;
    const double      duration     = totalSamples / sampleRate;
    const int         numPoints    = static_cast<int>(duration * m_pointsPerSecond);

    if (numPoints <= 0) return;

    m_trackData->setTotalExpected(numPoints);
    m_trackData->reserve(numPoints);

    // Samples per waveform bin (one x-pixel in the overview).
    const int samplesPerBin = static_cast<int>(totalSamples / numPoints);
    if (samplesPerBin < 1) return;

    // -------------------------------------------------------------------------
    // DSP-Kette: Phase-Compensated LR4 3-Band Crossover + Transient Detection
    //
    //  1. CROSSOVER (LR4, -24 dB/oct) with allpass phase compensation
     //     xoverLow  @ 150 Hz:   stereo → rawLow + midHigh
     //     xoverHigh @ 2500 Hz:  midHigh → mid + high
     //     allpassComp @ 2500 Hz: rawLow → low  (phase-aligned)
     //
     //     Rekordbox / Serato convention:
     //       LOW  = Kick + Sub-Bass (0–150 Hz)   → fat blue blocks
     //       MID  = Bass + Vocals + Snare (150–2500 Hz) → amber spikes
     //       HIGH = HiHat + Percussion (2500 Hz+) → white needles
     //
     //     The allpass filter on the LOW band compensates the phase shift
     //     introduced by xoverHigh on the MID+HIGH path.  Without it,
     //     bass transients would arrive earlier than mid/high transients
     //     of the same drum hit → temporal smearing in the visual display.
    //
    //  2. PEAK & RMS per band per block
    //     Peak = max |sample| in the block  (transients, drum hits)
    //     RMS  = sqrt(mean(sample²))        (sustained energy, body)
    //
    //  3. TRANSIENT DETECTION (dual envelope followers on LOW band)
    //     Fast peak envelope (attack 0.5 ms, release 5 ms) tracks transients
    //     Slow RMS envelope  (attack 5 ms,   release 80 ms) tracks body
    //     Δ_transient = fast − slow  → positive spike on drum hits
    //
    //  4. ASYMMETRIC GAIN WEIGHTING
    //     LOW  × 1.5  →  bass visually dominates
    //     MID  × 0.7  →  mids sit behind
    //     HIGH × 0.3  →  highs are tiny spikes / nebel
    // -------------------------------------------------------------------------

    // Crossover filters (mono, 1 channel)
    juce::dsp::LinkwitzRileyFilter<float> xoverLow;
    xoverLow.setType(juce::dsp::LinkwitzRileyFilterType::allpass);  // type doesn't matter for 2-output overload
    xoverLow.setCutoffFrequency(150.0f);   // LOW/MID split: Kick+Sub vs everything else

    juce::dsp::LinkwitzRileyFilter<float> xoverHigh;
    xoverHigh.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
    xoverHigh.setCutoffFrequency(2500.0f); // MID/HIGH split: Snare+Vocals vs HiHat+Perc

    // Allpass phase compensator for the LOW band.
    // Matches the phase delay that xoverHigh introduces on the MID+HIGH path.
    juce::dsp::LinkwitzRileyFilter<float> allpassComp;
    allpassComp.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
    allpassComp.setCutoffFrequency(2500.0f);

    {
        // Prepare filters with the actual number of channels so the stereo-max
        // path can call processSample(ch, ...) for ch = 0 and ch = 1.
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBin);
        spec.numChannels      = static_cast<juce::uint32>(
            std::max(1, static_cast<int>(reader->numChannels)));
        xoverLow.prepare(spec);
        xoverHigh.prepare(spec);
        allpassComp.prepare(spec);
    }

    // Dual envelope followers for bass transient detection.
    // The difference (fast − slow) isolates sharp drum hits.
    EnvelopeFollower envFastPeak;  // tracks transients (short ballistics)
    EnvelopeFollower envSlowRms;   // tracks sustained body (long ballistics)
    envFastPeak.prepare(sampleRate, 0.5f, 5.0f);    // attack 0.5 ms, release 5 ms
    envSlowRms.prepare (sampleRate, 5.0f, 80.0f);   // attack 5 ms,   release 80 ms

    // Rekordbox-style triangle envelope followers.
    // Instant attack (0 ms): every sample peak is tracked immediately.
    // Asymmetric decay: LOW decays slowly (fat blocks), MID/HIGH quickly (sharp spikes).
    // The renderer uses these as the primary bar height — their exponential decay
    // directly produces the characteristic "right-falling triangle" on each transient.
    EnvelopeFollower envLow;   // 0 ms attack, 40 ms release → fat bass blocks, snappy at 130+ BPM
    EnvelopeFollower envMid;   // 0 ms attack, 15 ms release → razor-sharp snare spikes
    EnvelopeFollower envHigh;  // 0 ms attack, 15 ms release → bright hihat needles
    envLow .prepare(sampleRate, 0.0f, 40.0f);
    envMid .prepare(sampleRate, 0.0f, 15.0f);
    envHigh.prepare(sampleRate, 0.0f, 15.0f);

    // =========================================================================
    // PASS 1 — Live Preview  (progressive rendering while audio is read)
    //
    //   Running maxima (floored at 0.2) normalise each chunk on-the-fly so the
    //   waveform builds up live on screen.  Starting at 0.2 prevents the very
    //   first quiet sample from exploding to 1.0 before the running max settles.
    //
    //   All raw envelope states are also stored in rawBins[] for Pass 2.
    // =========================================================================

    struct RawBin {
        float peakLow  = 0.0f, peakMid  = 0.0f, peakHigh  = 0.0f;
        float rmsLow   = 0.0f, rmsMid   = 0.0f, rmsHigh   = 0.0f;
        float envLowRaw  = 0.0f, envMidRaw  = 0.0f, envHighRaw = 0.0f;
        float transientDelta = 0.0f;
    };
    std::vector<RawBin> rawBins;
    rawBins.reserve(static_cast<size_t>(numPoints));

    // Running maxima for live preview — start at 0.2 to avoid initial explosion.
    float runMaxEnvLow  = 0.2f;
    float runMaxEnvMid  = 0.2f;
    float runMaxEnvHigh = 0.2f;
    float runMaxPeak    = 0.2f;

    // True global maxima, updated in Pass 1, used for the final-polish in Pass 2.
    float globalMaxEnvLow  = 0.001f;
    float globalMaxEnvMid  = 0.001f;
    float globalMaxEnvHigh = 0.001f;
    float globalMaxPeak    = 0.001f;

    // Helper: shape one raw envelope value → final visual value.
    // Called identically in both passes so they produce the same result given
    // the same normalised input.
    auto shapeEnv = [](float normLow, float normMid, float normHigh,
                       float& outLow,  float& outMid,  float& outHigh) {
        outLow  = std::pow(juce::jlimit(0.0f, 1.0f, normLow),  1.5f);
        outMid  = std::pow(juce::jlimit(0.0f, 1.0f, normMid),  1.2f) * 0.75f;
        outHigh = std::pow(juce::jlimit(0.0f, 1.0f, normHigh), 1.8f) * 0.90f;
    };

    constexpr int previewChunk = 50;   // emit to UI every N bins
    QVector<TrackData::WaveformBin> previewBatch;
    previewBatch.reserve(previewChunk);

    juce::AudioBuffer<float> readBuf(static_cast<int>(reader->numChannels), samplesPerBin);

    for (int bin = 0; bin < numPoints; ++bin)
    {
        if (threadShouldExit()) break;

        reader->read(&readBuf, 0, samplesPerBin,
                     static_cast<juce::int64>(bin) * samplesPerBin, true, false);

        const int numCh = static_cast<int>(reader->numChannels);

        float peakLow = 0.0f, peakMid = 0.0f, peakHigh = 0.0f;
        double sqSumLow = 0.0, sqSumMid = 0.0, sqSumHigh = 0.0;
        float maxTransientDelta = 0.0f;

        for (int s = 0; s < samplesPerBin; ++s)
        {
            float bestLow = 0.0f, bestMid = 0.0f, bestHigh = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                float in = readBuf.getReadPointer(ch)[s];

                float rawLow, midHigh;
                xoverLow.processSample(ch, in, rawLow, midHigh);

                float sigMid, sigHigh;
                xoverHigh.processSample(ch, midHigh, sigMid, sigHigh);

                float sigLow = allpassComp.processSample(ch, rawLow);

                float aL = std::abs(sigLow);
                float aM = std::abs(sigMid);
                float aH = std::abs(sigHigh);

                if (aL > bestLow)  bestLow  = aL;
                if (aM > bestMid)  bestMid  = aM;
                if (aH > bestHigh) bestHigh = aH;
            }

            if (bestLow  > peakLow)  peakLow  = bestLow;
            if (bestMid  > peakMid)  peakMid  = bestMid;
            if (bestHigh > peakHigh) peakHigh = bestHigh;

            sqSumLow  += static_cast<double>(bestLow  * bestLow);
            sqSumMid  += static_cast<double>(bestMid  * bestMid);
            sqSumHigh += static_cast<double>(bestHigh * bestHigh);

            float envFastVal = envFastPeak.process(bestLow);
            float envSlowVal = envSlowRms .process(bestLow);
            float delta      = envFastVal - envSlowVal;
            if (delta > maxTransientDelta) maxTransientDelta = delta;

            envLow .process(bestLow);
            envMid .process(bestMid);
            envHigh.process(bestHigh);
        }

        const float inv = 1.0f / static_cast<float>(samplesPerBin);

        RawBin rb;
        rb.peakLow  = peakLow;
        rb.peakMid  = peakMid;
        rb.peakHigh = peakHigh;
        rb.rmsLow   = std::sqrt(static_cast<float>(sqSumLow)  * inv);
        rb.rmsMid   = std::sqrt(static_cast<float>(sqSumMid)  * inv);
        rb.rmsHigh  = std::sqrt(static_cast<float>(sqSumHigh) * inv);
        rb.envLowRaw  = envLow .state;
        rb.envMidRaw  = envMid .state;
        rb.envHighRaw = envHigh.state;
        rb.transientDelta = maxTransientDelta;
        rawBins.push_back(rb);

        // Update true global maxima (used by Pass 2)
        if (rb.envLowRaw  > globalMaxEnvLow)  globalMaxEnvLow  = rb.envLowRaw;
        if (rb.envMidRaw  > globalMaxEnvMid)  globalMaxEnvMid  = rb.envMidRaw;
        if (rb.envHighRaw > globalMaxEnvHigh) globalMaxEnvHigh = rb.envHighRaw;
        float binPeakMax = std::max({peakLow, peakMid, peakHigh});
        if (binPeakMax > globalMaxPeak) globalMaxPeak = binPeakMax;

        // Update running maxima for live preview (only grows, never shrinks)
        if (rb.envLowRaw  > runMaxEnvLow)  runMaxEnvLow  = rb.envLowRaw;
        if (rb.envMidRaw  > runMaxEnvMid)  runMaxEnvMid  = rb.envMidRaw;
        if (rb.envHighRaw > runMaxEnvHigh) runMaxEnvHigh = rb.envHighRaw;
        if (binPeakMax    > runMaxPeak)    runMaxPeak    = binPeakMax;

        // Build preview WaveformBin with running-max normalisation
        TrackData::WaveformBin pbin;
        pbin.lowPeak  = juce::jlimit(0.0f, 1.0f, rb.peakLow  / runMaxPeak);
        pbin.lowRms   = juce::jlimit(0.0f, 1.0f, rb.rmsLow   / runMaxPeak);
        pbin.midPeak  = juce::jlimit(0.0f, 1.0f, rb.peakMid  / runMaxPeak);
        pbin.midRms   = juce::jlimit(0.0f, 1.0f, rb.rmsMid   / runMaxPeak);
        pbin.highPeak = juce::jlimit(0.0f, 1.0f, rb.peakHigh / runMaxPeak);
        pbin.highRms  = juce::jlimit(0.0f, 1.0f, rb.rmsHigh  / runMaxPeak);
        pbin.transientDelta = juce::jlimit(0.0f, 1.0f, rb.transientDelta / runMaxPeak);
        shapeEnv(rb.envLowRaw  / runMaxEnvLow,
                 rb.envMidRaw  / runMaxEnvMid,
                 rb.envHighRaw / runMaxEnvHigh,
                 pbin.lowEnv, pbin.midEnv, pbin.highEnv);

        previewBatch.append(pbin);
        if (previewBatch.size() >= previewChunk) {
            m_trackData->appendData(previewBatch);
            previewBatch.clear();
        }
    }
    if (!previewBatch.isEmpty())
        m_trackData->appendData(previewBatch);

    if (threadShouldExit()) return;

    // =========================================================================
    // PASS 2 — Final Polish  (runs immediately after Pass 1, ~few ms for a 5-min track)
    //
    //   Re-normalise every bin against the TRUE global per-band maxima and
    //   atomically replace the preview data in one call.  The renderer picks
    //   up the polished version on its next timer tick — no flicker.
    // =========================================================================

    m_trackData->setGlobalMaxPeak(globalMaxPeak);

    QVector<TrackData::WaveformBin> finalData;
    finalData.reserve(static_cast<int>(rawBins.size()));

    for (const RawBin& rb : rawBins)
    {
        if (threadShouldExit()) break;

        TrackData::WaveformBin wbin;
        wbin.lowPeak  = juce::jlimit(0.0f, 1.0f, rb.peakLow  / globalMaxPeak);
        wbin.lowRms   = juce::jlimit(0.0f, 1.0f, rb.rmsLow   / globalMaxPeak);
        wbin.midPeak  = juce::jlimit(0.0f, 1.0f, rb.peakMid  / globalMaxPeak);
        wbin.midRms   = juce::jlimit(0.0f, 1.0f, rb.rmsMid   / globalMaxPeak);
        wbin.highPeak = juce::jlimit(0.0f, 1.0f, rb.peakHigh / globalMaxPeak);
        wbin.highRms  = juce::jlimit(0.0f, 1.0f, rb.rmsHigh  / globalMaxPeak);
        wbin.transientDelta = juce::jlimit(0.0f, 1.0f, rb.transientDelta / globalMaxPeak);
        shapeEnv(rb.envLowRaw  / globalMaxEnvLow,
                 rb.envMidRaw  / globalMaxEnvMid,
                 rb.envHighRaw / globalMaxEnvHigh,
                 wbin.lowEnv, wbin.midEnv, wbin.highEnv);
        finalData.append(wbin);
    }

    if (!threadShouldExit())
        m_trackData->replaceAllData(std::move(finalData), globalMaxPeak);

    if (threadShouldExit()) return;

    // -------------------------------------------------------------------------
    // Stage 4: BPM detection — strict 0.5-BPM binned histogram.
    //
    //  1. CONFIDENCE GATING
    //     Pre-scan computes median RMS; hops below 15 % of that are skipped.
    //
    //  2. DUAL-METHOD (specdiff + hfc)
    //     Two aubio_tempo instances run in parallel, both contributing
    //     beat timestamps.
    //
    //  3. INSTANT-BPM → 0.5-BPM BINNED HISTOGRAM
    //     For every consecutive beat pair we compute the instantaneous BPM:
    //       instantBpm = (sampleRate × 60) / deltaSamples
    //     Values outside [60, 200] BPM are discarded immediately.
    //     The remaining value is snapped to the nearest 0.5 BPM grid:
    //       binnedBpm = round(instantBpm * 2) / 2
    //     and voted into a std::map<double,int>.  The key with the highest
    //     count is the "True BPM" — immune to syncopation, breakdowns, and
    //     ambient sections because each IBI votes independently and the
    //     majority wins.
    //
    //  4. HALF-TIME / DOUBLE-TIME CORRECTION
    //     If the winning BPM > 130, check whether exactly half of it also
    //     has histogram votes.  If yes AND the half value is in [60,130],
    //     prefer the lower value (DJ convention for Future Bass, Trap, etc.).
    // -------------------------------------------------------------------------
    {
        const uint_t aubioSr  = static_cast<uint_t>(sampleRate);
        const uint_t hopSize  = 512;
        const uint_t winSize  = 1024;
        const juce::int64 totalHops = totalSamples / static_cast<juce::int64>(hopSize);

        // ── Pre-scan: energy gate ────────────────────────────────────────────
        float energyGate = 0.0f;
        {
            juce::AudioBuffer<float> scanBuf(static_cast<int>(reader->numChannels),
                                             static_cast<int>(hopSize));
            const juce::int64 step = 16;
            std::vector<float> rmsSamples;
            rmsSamples.reserve(static_cast<size_t>(totalHops / step + 1));

            for (juce::int64 h = 0; h < totalHops; h += step) {
                if (threadShouldExit()) break;
                reader->read(&scanBuf, 0, static_cast<int>(hopSize),
                             h * static_cast<juce::int64>(hopSize), true, false);
                float sum = 0.0f;
                const int nc = static_cast<int>(reader->numChannels);
                for (int ch = 0; ch < nc; ++ch) {
                    const float* p = scanBuf.getReadPointer(ch);
                    for (uint_t s = 0; s < hopSize; ++s) sum += p[s] * p[s];
                }
                rmsSamples.push_back(std::sqrt(sum / static_cast<float>(hopSize * nc)));
            }
            if (!rmsSamples.empty()) {
                auto mid = rmsSamples.begin() + rmsSamples.size() / 2;
                std::nth_element(rmsSamples.begin(), mid, rmsSamples.end());
                energyGate = *mid * 0.15f;
            }
            energyGate = std::max(energyGate, 0.003f);
        }
        qDebug() << "[WaveformAnalyzer] energyGate:" << energyGate;

        // ── Shared 0.5-BPM histogram ─────────────────────────────────────────
        std::map<double, int> bpmHistogram;

        // Beat sample positions from the full-track pass (used for grid alignment
        // and for 16-beat macro-distance BPM measurement).
        std::vector<uint64_t> beatPositions;

        // Beat timestamps from the full-track pass (for grid alignment).
        std::vector<double> allBeats;

        // ── Dual-method pass runner ──────────────────────────────────────────
        auto runPass = [&](juce::int64 startHop, juce::int64 endHop, bool keepBeats) {
            aubio_tempo_t* tempoA = new_aubio_tempo("specdiff", winSize, hopSize, aubioSr);
            aubio_tempo_t* tempoB = new_aubio_tempo("hfc",      winSize, hopSize, aubioSr);
            if (!tempoA || !tempoB) {
                if (tempoA) del_aubio_tempo(tempoA);
                if (tempoB) del_aubio_tempo(tempoB);
                return;
            }

            fvec_t* in   = new_fvec(hopSize);
            fvec_t* outA = new_fvec(1);
            fvec_t* outB = new_fvec(1);
            juce::AudioBuffer<float> rb(static_cast<int>(reader->numChannels),
                                        static_cast<int>(hopSize));

            std::vector<double> beatsA, beatsB;
            std::vector<uint64_t> positionsA, positionsB;
            beatsA.reserve(2048);
            beatsB.reserve(2048);
            positionsA.reserve(2048);
            positionsB.reserve(2048);

            for (juce::int64 h = startHop; h < endHop; ++h) {
                if (threadShouldExit()) break;
                reader->read(&rb, 0, static_cast<int>(hopSize),
                             h * static_cast<juce::int64>(hopSize), true, false);
                const int nc = static_cast<int>(reader->numChannels);

                float rmsSum = 0.0f;
                for (uint_t s = 0; s < hopSize; ++s) {
                    float m = 0.0f;
                    for (int ch = 0; ch < nc; ++ch) m += rb.getReadPointer(ch)[s];
                    m /= static_cast<float>(nc);
                    in->data[s] = m;
                    rmsSum += m * m;
                }
                float rms = std::sqrt(rmsSum / static_cast<float>(hopSize));

                if (rms < energyGate) continue;

                aubio_tempo_do(tempoA, in, outA);
                aubio_tempo_do(tempoB, in, outB);

                if (outA->data[0] != 0.0f) {
                    double beatSec = static_cast<double>(aubio_tempo_get_last_s(tempoA));
                    beatsA.push_back(beatSec);
                    positionsA.push_back(static_cast<uint64_t>(beatSec * sampleRate));
                }
                if (outB->data[0] != 0.0f) {
                    double beatSec = static_cast<double>(aubio_tempo_get_last_s(tempoB));
                    beatsB.push_back(beatSec);
                    positionsB.push_back(static_cast<uint64_t>(beatSec * sampleRate));
                }
            }

            del_aubio_tempo(tempoA);
            del_aubio_tempo(tempoB);
            del_fvec(in);
            del_fvec(outA);
            del_fvec(outB);

            // Merge beat positions and timestamps from both methods.
            beatPositions.insert(beatPositions.end(), positionsA.begin(), positionsA.end());
            beatPositions.insert(beatPositions.end(), positionsB.begin(), positionsB.end());

            if (keepBeats) {
                allBeats = (beatsA.size() >= beatsB.size())
                           ? std::move(beatsA)
                           : std::move(beatsB);
            }
        };

        // Three passes: first 40 %, middle 40 %, full track.
        juce::int64 h1 = totalHops * 4 / 10;
        juce::int64 h2 = totalHops * 3 / 10;
        juce::int64 h3 = h2 + totalHops * 4 / 10;

        runPass(0,  h1,        false);
        runPass(h2, h3,        false);
        runPass(0,  totalHops, true);

        // ── 16-beat macro-distance BPM measurement ──────────────────────────
        // Measure the distance across 16-beat intervals to average out the
        // 512-sample hop-size quantization error. This reduces quantisation
        // noise from ±1.3 BPM (at 100 BPM) to ±0.08 BPM.
        if (beatPositions.size() >= 17) {
            // Sort beat positions to ensure chronological order (since we merged
            // from both methods).
            std::sort(beatPositions.begin(), beatPositions.end());

            // Deduplicate very close beats (< 100 ms apart) — likely duplicates
            // from specdiff and hfc firing on the same transient.
            {
                const uint64_t minSpacing = static_cast<uint64_t>(0.1 * sampleRate);
                auto last = std::unique(beatPositions.begin(), beatPositions.end(),
                                       [minSpacing](uint64_t a, uint64_t b) {
                                           return (b - a) < minSpacing;
                                       });
                beatPositions.erase(last, beatPositions.end());
            }

            // Compute 16-beat macro-distances and vote them into the histogram.
            for (size_t i = 16; i < beatPositions.size(); ++i) {
                uint64_t deltaSamples = beatPositions[i] - beatPositions[i - 16];
                if (deltaSamples == 0) continue;

                // BPM over 16 beats: multiply by 16 to get the true beat period.
                double instantBpm = (16.0 * sampleRate * 60.0) / static_cast<double>(deltaSamples);

                // Hard range gate: discard anything outside [60, 200].
                if (instantBpm < 60.0 || instantBpm > 200.0) continue;

                // Snap to nearest 0.5 BPM and vote.
                double binnedBpm = std::round(instantBpm * 2.0) / 2.0;
                bpmHistogram[binnedBpm]++;
            }
        }

        // ── Peak finding ─────────────────────────────────────────────────────
        double estimatedBpm = 0.0;
        if (!bpmHistogram.empty()) {
            // Find the bin with the most votes.
            double peakBpm   = 0.0;
            int    peakCount = 0;
            for (auto& [bpm, count] : bpmHistogram) {
                if (count > peakCount) {
                    peakCount = count;
                    peakBpm   = bpm;
                }
            }

            // ── Future Bass / Double-Time Correction ─────────────────────────
            // If the peak is above 130 BPM (outside the comfortable DJ range),
            // check whether exactly half of it exists in the histogram and falls
            // in [60, 130]. If so, prefer the halved value.
            // This catches Future Bass (75 BPM, detected as 150), Trap (85 BPM
            // detected as 170), and other common false-doubling cases.
            if (peakBpm > 130.0) {
                double halfBpm = peakBpm / 2.0;
                // Re-snap to 0.5 grid (in case peakBpm was e.g. 150.2 before snapping).
                halfBpm = std::round(halfBpm * 2.0) / 2.0;

                if (halfBpm >= 60.0 && halfBpm <= 130.0) {
                    auto it = bpmHistogram.find(halfBpm);
                    if (it != bpmHistogram.end() && it->second > 0) {
                        qDebug() << "[WaveformAnalyzer] Future Bass correction:"
                                 << peakBpm << "->" << halfBpm
                                 << "(peak votes:" << peakCount
                                 << "half votes:" << it->second << ")";
                        peakBpm = halfBpm;
                    }
                }
            }

            estimatedBpm = peakBpm;

            // Debug: print top 5 histogram entries.
            std::vector<std::pair<double, int>> sorted(bpmHistogram.begin(),
                                                       bpmHistogram.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
            int show = std::min(static_cast<int>(sorted.size()), 5);
            QString top5;
            for (int i = 0; i < show; ++i)
                top5 += QString("%1(%2) ").arg(sorted[i].first).arg(sorted[i].second);
            qDebug() << "[WaveformAnalyzer] 16-beat BPM histogram top-5:" << top5
                     << "-> winner:" << estimatedBpm
                     << "(totalBeats:" << beatPositions.size() << ")";
        }

        // ── Beat-grid alignment ───────────────────────────────────────────────
        //
        // Goal: find the exact sample position of the first beat so that the
        // grid  firstBeat + n * period  lands precisely on every beat in the
        // track.
        //
        // Strategy:
        // 1. AUBIO LATENCY COMPENSATION: aubio's FFT-based beat detection reports
        //    beats with a fixed delay (due to window size). We subtract a fixed
        //    offset (hop_size) from all beatPositions to shift them left to the
        //    true transient peaks.
        //
        // 2. COMB FILTER / PHASE ALIGNMENT (Downbeat-Finder): We don't trust
        //    beatPositions[0] as the start. Instead, we compute the beat period
        //    in samples and run a comb filter: test all possible phase offsets
        //    (0 to period in regular steps), and score each offset by how many
        //    aubio-detected beats align with the virtual grid lines. The offset
        //    with the highest score is the true downbeat.
        //
        // 3. WALK BACK TO TRACK START: Once the grid anchor is locked, we walk
        //    backwards to find the first beat in the track (handling silent intros).
        // ─────────────────────────────────────────────────────────────────────
        qint64 firstBeatSample = 0;
        if (estimatedBpm > 0.0 && beatPositions.size() >= 16) {
            // ── STEP 1: Aubio latency compensation ───────────────────────────
            // aubio's tempo detector reports beats quantized to the hop boundary,
            // plus a fixed delay due to FFT windowing. Subtract hop_size from all
            // beat positions to shift them left to the true transient peaks.
            const uint64_t latencyCompensation = hopSize;
            for (auto& pos : beatPositions) {
                pos = (pos >= latencyCompensation) ? (pos - latencyCompensation) : 0;
            }
            qDebug() << "[WaveformAnalyzer] Applied aubio latency compensation:"
                     << "–" << latencyCompensation << "samples";

            // ── STEP 2: Comb filter / phase alignment (downbeat-finder) ──────
            // Compute the beat period in samples.
            double samplesPerBeat = (sampleRate * 60.0) / estimatedBpm;

            // Test phase offsets from 0 to samplesPerBeat in regular steps (256-sample granularity).
            const uint64_t phaseStep = 256;
            uint64_t bestPhaseOffset = 0;
            int      bestPhaseScore  = -1;
            double   bestPhaseAnchor = 0.0;

            // We scan up to the first beat position found, but at least 4 beat periods
            // to account for aubio's warm-up period (~80 ms detection latency).
            uint64_t scanLimit = static_cast<uint64_t>(4.0 * samplesPerBeat);
            if (!beatPositions.empty())
                scanLimit = std::max(scanLimit, beatPositions[0] + static_cast<uint64_t>(samplesPerBeat));

            for (uint64_t phase = 0; phase < scanLimit; phase += phaseStep) {
                int score = 0;
                const double tolerance = 0.005 * sampleRate; // ±5 ms tolerance

                // Test how many beatPositions align with the virtual grid anchored at `phase`.
                for (uint64_t beat : beatPositions) {
                    // Distance to nearest grid line: beat - phase - n * samplesPerBeat.
                    double diff  = static_cast<double>(beat - phase);
                    double phase_err = diff - samplesPerBeat * std::round(diff / samplesPerBeat);
                    if (std::abs(phase_err) <= tolerance) ++score;
                }

                if (score > bestPhaseScore) {
                    bestPhaseScore  = score;
                    bestPhaseOffset = phase;
                    bestPhaseAnchor = static_cast<double>(phase) / sampleRate;
                }
            }

            // Refine the best phase offset: do a fine-grained sub-phase scan
            // in ±256 samples around the best coarse offset.
            if (bestPhaseScore >= 0) {
                uint64_t refineStart = (bestPhaseOffset >= 64) ? (bestPhaseOffset - 64) : 0;
                uint64_t refineEnd   = bestPhaseOffset + 64;
                for (uint64_t phase = refineStart; phase <= refineEnd; phase += 16) {
                    int score = 0;
                    const double tolerance = 0.005 * sampleRate;
                    for (uint64_t beat : beatPositions) {
                        double diff  = static_cast<double>(beat - phase);
                        double phase_err = diff - samplesPerBeat * std::round(diff / samplesPerBeat);
                        if (std::abs(phase_err) <= tolerance) ++score;
                    }
                    if (score > bestPhaseScore) {
                        bestPhaseScore  = score;
                        bestPhaseOffset = phase;
                        bestPhaseAnchor = static_cast<double>(phase) / sampleRate;
                    }
                }
            }

            qDebug() << "[WaveformAnalyzer] Comb filter phase alignment:"
                     << "bestPhaseOffset=" << bestPhaseOffset << "samples"
                     << "bestPhaseAnchor=" << bestPhaseAnchor << "seconds"
                     << "gridScore=" << bestPhaseScore << "/" << beatPositions.size();

            // ── STEP 3: Walk back to the first beat in the track ────────────
            // From the phase-aligned anchor, walk backwards in beat periods until:
            //   (a) We'd go below sample 0, OR
            //   (b) The region is near-silent (RMS < –60 dBFS), indicating pre-intro
            const uint_t rmsHop = 512;
            juce::AudioBuffer<float> rmsBuf(static_cast<int>(reader->numChannels),
                                            static_cast<int>(rmsHop));

            uint64_t gridAnchor = bestPhaseOffset;
            while (true) {
                // Next step: go back one beat period.
                int64_t prev = static_cast<int64_t>(gridAnchor) - static_cast<int64_t>(samplesPerBeat);
                if (prev < 0) break;

                // Measure RMS at prev.
                juce::int64 prevSample = static_cast<juce::int64>(prev);
                reader->read(&rmsBuf, 0, static_cast<int>(rmsHop), prevSample, true, false);
                float rmsSum = 0.0f;
                const int nc = static_cast<int>(reader->numChannels);
                for (int ch = 0; ch < nc; ++ch) {
                    const float* p = rmsBuf.getReadPointer(ch);
                    for (uint_t s = 0; s < rmsHop; ++s) rmsSum += p[s] * p[s];
                }
                float rms = std::sqrt(rmsSum / static_cast<float>(rmsHop * nc));

                // –60 dBFS ≈ 0.001 linear. If near-silent, we've hit the intro: stop.
                if (rms < 0.001f) break;

                gridAnchor = static_cast<uint64_t>(prev);
            }

            firstBeatSample = static_cast<qint64>(gridAnchor);
            qDebug() << "[WaveformAnalyzer] Beat-grid alignment complete:"
                     << "firstBeatSample=" << firstBeatSample
                     << "(" << (static_cast<double>(firstBeatSample) / sampleRate * 1000.0) << "ms)"
                     << "samplesPerBeat=" << static_cast<uint64_t>(samplesPerBeat);
        }

        if (estimatedBpm > 0.0)
            m_trackData->setBpmData(estimatedBpm, firstBeatSample, sampleRate);
    }

    if (threadShouldExit()) return;

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
        juce::AudioBuffer<float> kfReadBuf(static_cast<int>(reader->numChannels), kfBlockSize);

        for (juce::int64 offset = 0; offset < keySamples; offset += kfBlockSize) {
            if (threadShouldExit()) break;

            const int toRead = static_cast<int>(
                std::min(static_cast<juce::int64>(kfBlockSize), keySamples - offset));

            reader->read(&kfReadBuf, 0, toRead, offset, true, false);

            const int numCh2 = static_cast<int>(reader->numChannels);
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
}

