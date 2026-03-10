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
    // DSP-Kette: Parallel 4-Band Filterbank (Rekordbox-style, overlapping)
    //
    //  Band 1 — LOW  (Dark Blue):  LP @ 110 Hz, 6 dB/oct (1st order)
    //     Sub-bass + Kick fundamental. Single 1-pole LP.
    //
    //  Band 2 — LOWMID (Gold):  BP 150–160 Hz, 12+6 dB/oct
    //     Bass body / warmth. HP @ 150 Hz (2nd order) + LP @ 160 Hz (1st order).
    //     Very narrow — captures the "weight" between kick and mids.
    //
    //  Band 3 — MID (Orange):  BP 180–800 Hz, 12+6 dB/oct
    //     Snare body, vocals, melodic content.
    //     HP @ 180 Hz (2nd order) + LP @ 800 Hz (1st order).
    //
    //  Band 4 — HIGH (White):  BP @ 2750 Hz (Q=2) + HP @ 19000 Hz
    //     Two parallel sub-paths added together — snare smack + extreme HiHat.
    //     The frequency gap (800–2750 Hz and 3500–19000 Hz) intentionally
    //     suppresses noise and vocal sibilance for a clean, sparse display.
    //
    //  All paths are PARALLEL (input → each filter independently), NOT serial.
    //  "Color bleeding" comes from the intentional overlap of the flat slopes.
    //
    //  Envelope: instant attack (0 ms), exponential release per band.
    //  Shaping:  pow() expansion + gain multiplier per band.
    // -------------------------------------------------------------------------

    const int numCh = static_cast<int>(reader->numChannels);
    const float sr  = static_cast<float>(sampleRate);

    // ── 1-pole LP coefficient: a = 2π·fc / (2π·fc + sr) ─────────────────────
    auto lpCoef1 = [&](float fc) -> float {
        const float w = 2.0f * juce::MathConstants<float>::pi * fc / sr;
        return w / (w + 1.0f);
    };

    // ── Band 1: LP @ 110 Hz (1st order = 6 dB/oct) ──────────────────────────
    const float aLP110 = lpCoef1(110.0f);
    std::vector<float> lp110state(static_cast<size_t>(numCh), 0.0f);

    // ── Band 2: HP @ 150 Hz (2nd order) + LP @ 160 Hz (1st order) ───────────
    //    2nd order HP = two cascaded 1st-order HP stages.
    //    HP coefficient: same as LP but applied as HP (out = in - lp).
    const float aHP150 = lpCoef1(150.0f);
    const float aLP160 = lpCoef1(160.0f);
    std::vector<float> hp150s1(static_cast<size_t>(numCh), 0.0f);  // HP stage 1
    std::vector<float> hp150s2(static_cast<size_t>(numCh), 0.0f);  // HP stage 2
    std::vector<float> lp160state(static_cast<size_t>(numCh), 0.0f);

    // ── Band 3: HP @ 180 Hz (2nd order) + LP @ 800 Hz (1st order) ───────────
    const float aHP180 = lpCoef1(180.0f);
    const float aLP800 = lpCoef1(800.0f);
    std::vector<float> hp180s1(static_cast<size_t>(numCh), 0.0f);
    std::vector<float> hp180s2(static_cast<size_t>(numCh), 0.0f);
    std::vector<float> lp800state(static_cast<size_t>(numCh), 0.0f);

    // ── Band 4: BP @ 2750 Hz (resonant) + HP @ 19000 Hz ─────────────────────
    //    Sub-path A: 2nd-order resonant BP at 2750 Hz using SVF (State Variable TPT).
    //    Sub-path B: HP @ 19000 Hz (1st order) for extreme hi-hat ticks.
    //    Final = abs(A) + abs(B).
    const float aHP19k = lpCoef1(19000.0f);
    std::vector<float> hp19kstate(static_cast<size_t>(numCh), 0.0f);

    // SVF (State Variable Filter) for the 2750 Hz resonant BP.
    // g = tan(π·fc/sr), R = 1/(2·Q) — Q=2 for moderate resonance.
    const float svfG = std::tan(juce::MathConstants<float>::pi * 2750.0f / sr);
    const float svfR = 1.0f / (2.0f * 2.0f);  // Q = 2
    const float svfD = 1.0f / (1.0f + 2.0f * svfR * svfG + svfG * svfG);
    // Per-channel SVF states (ic1eq, ic2eq)
    std::vector<float> svfIc1(static_cast<size_t>(numCh), 0.0f);
    std::vector<float> svfIc2(static_cast<size_t>(numCh), 0.0f);

    // ── Envelope followers (instant attack, exponential release) ─────────────
    EnvelopeFollower envLow;      // 0 ms attack, 35 ms release
    EnvelopeFollower envLowMid;   // 0 ms attack, 25 ms release
    EnvelopeFollower envMid;      // 0 ms attack, 15 ms release
    EnvelopeFollower envHigh;     // 0 ms attack,  5 ms release
    envLow   .prepare(sampleRate, 0.0f, 35.0f);
    envLowMid.prepare(sampleRate, 0.0f, 25.0f);
    envMid   .prepare(sampleRate, 0.0f, 15.0f);
    envHigh  .prepare(sampleRate, 0.0f,  5.0f);

    // =========================================================================
    // PASS 1 — Raw Analysis + Live Preview (progressive rendering)
    //
    //  • Collects raw envelope values in rawBins for the final pass.
    //  • Tracks global per-band maxima for true normalization later.
    //  • Simultaneously sends a live preview to QML using running-max
    //    normalization (so the waveform builds up on screen in real-time).
    // =========================================================================

    struct RawBin {
        float low    = 0.0f;   // raw envelope value, NOT normalized
        float lowMid = 0.0f;
        float mid    = 0.0f;
        float high   = 0.0f;
    };
    std::vector<RawBin> rawBins;
    rawBins.reserve(static_cast<size_t>(numPoints));

    // Global per-band maxima — tracked across the entire track (for Pass 2).
    float globalMaxLow    = 0.0f;
    float globalMaxLowMid = 0.0f;
    float globalMaxMid    = 0.0f;
    float globalMaxHigh   = 0.0f;
    float globalMaxPeak   = 0.001f;

    // Running maxima for live preview — start at 0.1 so the first quiet
    // sample doesn't explode to full height.
    float runMaxLow    = 0.1f;
    float runMaxLowMid = 0.1f;
    float runMaxMid    = 0.1f;
    float runMaxHigh   = 0.1f;

    constexpr int previewChunk = 50;
    QVector<TrackData::WaveformBin> previewBatch;
    previewBatch.reserve(previewChunk);

    // Shared shaping helper — used identically in preview AND final pass.
    auto shapeBin = [](float norm, float expo, float gain) -> float {
        return std::min(1.0f, std::pow(std::clamp(norm, 0.0f, 1.0f), expo) * gain);
    };

    juce::AudioBuffer<float> readBuf(static_cast<int>(reader->numChannels), samplesPerBin);

    for (int bin = 0; bin < numPoints; ++bin)
    {
        if (threadShouldExit()) break;

        reader->read(&readBuf, 0, samplesPerBin,
                     static_cast<juce::int64>(bin) * samplesPerBin, true, false);

        for (int s = 0; s < samplesPerBin; ++s)
        {
            float bestLow = 0.0f, bestLowMid = 0.0f, bestMid = 0.0f, bestHigh = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
            {
                const size_t ci = static_cast<size_t>(ch);
                const float in = readBuf.getReadPointer(ch)[s];

                // ── Band 1: LP @ 110 Hz (1st order) ─────────────────────────
                lp110state[ci] = aLP110 * in + (1.0f - aLP110) * lp110state[ci];
                const float b1 = std::abs(lp110state[ci]);

                // ── Band 2: HP @ 150 Hz (2nd order) → LP @ 160 Hz ───────────
                hp150s1[ci] = aHP150 * in + (1.0f - aHP150) * hp150s1[ci];
                const float hp1out = in - hp150s1[ci];
                hp150s2[ci] = aHP150 * hp1out + (1.0f - aHP150) * hp150s2[ci];
                const float hp2out = hp1out - hp150s2[ci];
                lp160state[ci] = aLP160 * hp2out + (1.0f - aLP160) * lp160state[ci];
                const float b2 = std::abs(lp160state[ci]);

                // ── Band 3: HP @ 180 Hz (2nd order) → LP @ 800 Hz ───────────
                hp180s1[ci] = aHP180 * in + (1.0f - aHP180) * hp180s1[ci];
                const float hp3out = in - hp180s1[ci];
                hp180s2[ci] = aHP180 * hp3out + (1.0f - aHP180) * hp180s2[ci];
                const float hp4out = hp3out - hp180s2[ci];
                lp800state[ci] = aLP800 * hp4out + (1.0f - aLP800) * lp800state[ci];
                const float b3 = std::abs(lp800state[ci]);

                // ── Band 4: Resonant BP @ 2750 Hz + HP @ 19 kHz ─────────────
                const float v3 = in - svfIc2[ci];
                const float v1 = svfD * (svfIc1[ci] + svfG * v3);
                const float v2 = svfIc2[ci] + svfG * v1;
                svfIc1[ci] = 2.0f * v1 - svfIc1[ci];
                svfIc2[ci] = 2.0f * v2 - svfIc2[ci];
                const float bp2750 = v1;

                hp19kstate[ci] = aHP19k * in + (1.0f - aHP19k) * hp19kstate[ci];
                const float hp19k = in - hp19kstate[ci];

                const float b4 = std::abs(bp2750) + std::abs(hp19k);

                if (b1 > bestLow)    bestLow    = b1;
                if (b2 > bestLowMid) bestLowMid = b2;
                if (b3 > bestMid)    bestMid    = b3;
                if (b4 > bestHigh)   bestHigh   = b4;
            }

            envLow   .process(bestLow);
            envLowMid.process(bestLowMid);
            envMid   .process(bestMid);
            envHigh  .process(bestHigh);
        }

        // Store RAW envelope values for the final pass.
        RawBin rb;
        rb.low    = envLow   .state;
        rb.lowMid = envLowMid.state;
        rb.mid    = envMid   .state;
        rb.high   = envHigh  .state;
        rawBins.push_back(rb);

        // Track global per-band maxima (for Pass 2+3).
        if (rb.low    > globalMaxLow)    globalMaxLow    = rb.low;
        if (rb.lowMid > globalMaxLowMid) globalMaxLowMid = rb.lowMid;
        if (rb.mid    > globalMaxMid)    globalMaxMid    = rb.mid;
        if (rb.high   > globalMaxHigh)   globalMaxHigh   = rb.high;
        const float binMax = std::max({rb.low, rb.lowMid, rb.mid, rb.high});
        if (binMax > globalMaxPeak) globalMaxPeak = binMax;

        // ── Live Preview: running-max normalization + shaping ────────────────
        if (rb.low    > runMaxLow)    runMaxLow    = rb.low;
        if (rb.lowMid > runMaxLowMid) runMaxLowMid = rb.lowMid;
        if (rb.mid    > runMaxMid)    runMaxMid    = rb.mid;
        if (rb.high   > runMaxHigh)   runMaxHigh   = rb.high;

        TrackData::WaveformBin pbin;
        pbin.low    = shapeBin(rb.low    / runMaxLow,    1.8f, 1.0f);
        pbin.lowMid = shapeBin(rb.lowMid / runMaxLowMid, 1.6f, 0.9f);
        pbin.mid    = shapeBin(rb.mid    / runMaxMid,    1.5f, 0.7f);
        pbin.high   = shapeBin(rb.high   / runMaxHigh,   1.3f, 0.5f);
        // Legacy fields
        pbin.lowEnv  = pbin.low;   pbin.lowPeak = pbin.low;   pbin.lowRms = pbin.low;
        pbin.midEnv  = pbin.mid;   pbin.midPeak = pbin.mid;   pbin.midRms = pbin.mid;
        pbin.highEnv = pbin.high;  pbin.highPeak = pbin.high;  pbin.highRms = pbin.high;
        pbin.transientDelta = 0.0f;

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
    // PASS 2+3 — Global Normalization → Anti-Crush Shaping → Final Output
    //
    //  Uses the TRUE global per-band maxima (known after full Pass 1) for
    //  perfect proportions.  Same soft exponents + 2 % base floor as preview.
    //  Atomically replaces the preview data so the renderer switches seamlessly.
    // =========================================================================

    // Guard against division by zero for silent bands.
    if (globalMaxLow    < 1e-8f) globalMaxLow    = 1e-8f;
    if (globalMaxLowMid < 1e-8f) globalMaxLowMid = 1e-8f;
    if (globalMaxMid    < 1e-8f) globalMaxMid    = 1e-8f;
    if (globalMaxHigh   < 1e-8f) globalMaxHigh   = 1e-8f;

    QVector<TrackData::WaveformBin> finalData;
    finalData.reserve(static_cast<int>(rawBins.size()));

    for (const RawBin& rb : rawBins)
    {
        if (threadShouldExit()) break;

        // ── Global normalization → shaping → gain → base floor ──────────────
        TrackData::WaveformBin wbin;
        wbin.low    = shapeBin(rb.low    / globalMaxLow,    1.8f, 1.0f);
        wbin.lowMid = shapeBin(rb.lowMid / globalMaxLowMid, 1.6f, 0.9f);
        wbin.mid    = shapeBin(rb.mid    / globalMaxMid,    1.5f, 0.7f);
        wbin.high   = shapeBin(rb.high   / globalMaxHigh,   1.3f, 0.5f);

        // Legacy fields
        wbin.lowEnv  = wbin.low;   wbin.lowPeak = wbin.low;   wbin.lowRms = wbin.low;
        wbin.midEnv  = wbin.mid;   wbin.midPeak = wbin.mid;   wbin.midRms = wbin.mid;
        wbin.highEnv = wbin.high;  wbin.highPeak = wbin.high;  wbin.highRms = wbin.high;
        wbin.transientDelta = 0.0f;

        finalData.append(wbin);
    }

    if (!threadShouldExit())
        m_trackData->replaceAllData(std::move(finalData), globalMaxPeak);

    if (threadShouldExit()) return;

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
                auto mid = rmsSamples.begin() + static_cast<ptrdiff_t>(rmsSamples.size() / 2);
                std::nth_element(rmsSamples.begin(), mid, rmsSamples.end());
                energyGate = *mid * 0.15f;
            }
            energyGate = std::max(energyGate, 0.003f);
        }
        qDebug() << "[WaveformAnalyzer] energyGate:" << energyGate;

        // ── Histogram + beat collection ──────────────────────────────────────
        std::map<double, int>                  bpmHistogram;
        std::map<double, std::vector<double>>  bpmRawValues;
        std::vector<uint64_t> beatPositions;
        std::vector<double>   allBeats;   // elastic: best aubio timestamps (seconds)

        // ── SINGLE dual-method pass over the full track ──────────────────────
        // One pass: no redundant duplication. Both specdiff and hfc contribute
        // beat timestamps; we keep the longer list as the elastic grid seed.
        {
            aubio_tempo_t* tempoA = new_aubio_tempo("specdiff", winSize, hopSize, aubioSr);
            aubio_tempo_t* tempoB = new_aubio_tempo("hfc",      winSize, hopSize, aubioSr);
            if (tempoA && tempoB) {
                fvec_t* in   = new_fvec(hopSize);
                fvec_t* outA = new_fvec(1);
                fvec_t* outB = new_fvec(1);
                juce::AudioBuffer<float> rb(static_cast<int>(reader->numChannels),
                                            static_cast<int>(hopSize));

                std::vector<double>   beatsA, beatsB;
                std::vector<uint64_t> posA,   posB;
                beatsA.reserve(4096); beatsB.reserve(4096);
                posA  .reserve(4096); posB  .reserve(4096);

                for (juce::int64 h = 0; h < totalHops; ++h) {
                    if (threadShouldExit()) break;
                    reader->read(&rb, 0, static_cast<int>(hopSize),
                                 h * static_cast<juce::int64>(hopSize), true, false);
                    const int nc = static_cast<int>(reader->numChannels);

                    float rmsSum = 0.0f;
                    for (uint_t s = 0; s < hopSize; ++s) {
                        float m = 0.0f;
                        for (int ch = 0; ch < nc; ++ch)
                            m += rb.getReadPointer(ch)[s];
                        m /= static_cast<float>(nc);
                        in->data[s] = m;
                        rmsSum += m * m;
                    }
                    if (std::sqrt(rmsSum / static_cast<float>(hopSize)) < energyGate) continue;

                    aubio_tempo_do(tempoA, in, outA);
                    aubio_tempo_do(tempoB, in, outB);

                    if (outA->data[0] != 0.0f) {
                        double t = static_cast<double>(aubio_tempo_get_last_s(tempoA));
                        beatsA.push_back(t);
                        posA.push_back(static_cast<uint64_t>(t * sampleRate));
                    }
                    if (outB->data[0] != 0.0f) {
                        double t = static_cast<double>(aubio_tempo_get_last_s(tempoB));
                        beatsB.push_back(t);
                        posB.push_back(static_cast<uint64_t>(t * sampleRate));
                    }
                }

                del_aubio_tempo(tempoA);
                del_aubio_tempo(tempoB);
                del_fvec(in); del_fvec(outA); del_fvec(outB);

                // Merge into a single sorted, deduplicated beatPositions list.
                // 50 ms dedup window: tight enough to remove true duplicates from
                // specdiff+hfc firing on the same transient, loose enough to keep
                // real 16th-note hi-hats at any BPM > 60.
                beatPositions.reserve(posA.size() + posB.size());
                beatPositions.insert(beatPositions.end(), posA.begin(), posA.end());
                beatPositions.insert(beatPositions.end(), posB.begin(), posB.end());
                std::sort(beatPositions.begin(), beatPositions.end());
                {
                    const uint64_t minSpacing = static_cast<uint64_t>(0.05 * sampleRate); // 50 ms
                    auto last = std::unique(beatPositions.begin(), beatPositions.end(),
                                            [minSpacing](uint64_t a, uint64_t b){
                                                return (b - a) < minSpacing; });
                    beatPositions.erase(last, beatPositions.end());
                }

                // Keep the longer timestamp list as the elastic grid seed.
                allBeats = (beatsA.size() >= beatsB.size())
                           ? std::move(beatsA) : std::move(beatsB);
                std::sort(allBeats.begin(), allBeats.end());
            } else {
                if (tempoA) del_aubio_tempo(tempoA);
                if (tempoB) del_aubio_tempo(tempoB);
            }
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

        // ── Grid alignment ───────────────────────────────────────────────────
        qint64 firstBeatSample = 0;
        std::vector<TrackData::BeatMarker> finalBeatGrid;

        if (estimatedBpm > 0.0 && beatPositions.size() >= 16) {
            // ── STEP 1: Aubio latency compensation ────────────────────────────
            const uint64_t latComp = hopSize;
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
            const uint_t  rmsHop          = 512;
            const float   energyThreshold = globalMaxPeak * 0.15f;
            juce::AudioBuffer<float> rmsBuf(static_cast<int>(reader->numChannels),
                                            static_cast<int>(rmsHop));

            auto peakAt = [&](juce::int64 sample) -> float {
                reader->read(&rmsBuf, 0, static_cast<int>(rmsHop), sample, true, false);
                float pk = 0.0f;
                const int nc = static_cast<int>(reader->numChannels);
                for (int ch = 0; ch < nc; ++ch) {
                    const float* p = rmsBuf.getReadPointer(ch);
                    for (uint_t s = 0; s < rmsHop; ++s)
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
                        (n % 4 == 0),   // isDownbeat: beat 1 of every bar
                        (n / 4) + 1     // barNumber:  1-based bar counter
                    });
                }

                qDebug() << "[WaveformAnalyzer] Elastic beat-grid built:"
                         << finalBeatGrid.size() << "beats"
                         << "| firstBeat=" << firstBeatSample
                         << "(" << (static_cast<double>(firstBeatSample)/sampleRate*1000.0) << "ms)"
                         << "| samplesPerBeat=" << static_cast<uint64_t>(samplesPerBeat);
            }
        }

        if (estimatedBpm > 0.0)
            m_trackData->setBpmData(estimatedBpm, firstBeatSample, sampleRate,
                                    std::move(finalBeatGrid));
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

