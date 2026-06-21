#include "WaveformEnvelopePass.h"
#include "WaveformAnalyzer.h"
#include <QColor>
#include <juce_dsp/juce_dsp.h>
#include <QDebug>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

QVector<TrackData::RgbWaveformFrame> blendRgbPreferDynamics(
    const QVector<TrackData::RgbWaveformFrame>& current,
    const QVector<TrackData::RgbWaveformFrame>& candidate)
{
    if (candidate.isEmpty())
        return current;
    if (current.isEmpty())
        return candidate;

    const int n = std::min(current.size(), candidate.size());
    QVector<TrackData::RgbWaveformFrame> out;
    out.reserve(candidate.size());

    for (int i = 0; i < n; ++i) {
        TrackData::RgbWaveformFrame f = candidate[i];
        const auto& c = current[i];

        // Keep dynamic punch from the progressive pass.
        f.rms = std::max(c.rms, f.rms);
        f.low = std::max(c.low, f.low);
        f.mid = std::max(c.mid, f.mid);
        f.high = std::max(c.high, f.high);

        // Never darken strongly: candidate can refine hue, but not kill brightness.
        const int r = std::max(static_cast<int>(c.color.red() * 0.90f), f.color.red());
        const int g = std::max(static_cast<int>(c.color.green() * 0.90f), f.color.green());
        const int b = std::max(static_cast<int>(c.color.blue() * 0.90f), f.color.blue());
        f.color = QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), 230);

        out.push_back(f);
    }

    for (int i = n; i < candidate.size(); ++i)
        out.push_back(candidate[i]);

    return out;
}

#ifdef ESSENTIA_FOUND
    #include <essentia/essentia.h>
    #include <essentia/algorithmfactory.h>

QVector<TrackData::RgbWaveformFrame> analyzeRgbFramesWithEssentia(
    const std::vector<essentia::Real>& monoSignal,
    double sampleRate,
    int targetFrames,
    int frameSize = 2048,
    int hopSize = 1024)
{
    QVector<TrackData::RgbWaveformFrame> result;
    if (monoSignal.empty() || sampleRate <= 0.0 || frameSize < 128 || hopSize < 64)
        return result;

    using essentia::Real;
    using essentia::standard::Algorithm;
    using essentia::standard::AlgorithmFactory;

    std::vector<Real> frame;
    std::vector<Real> windowedFrame;
    std::vector<Real> spectrum;
    Real low = 0.0f;
    Real mid = 0.0f;
    Real high = 0.0f;

    std::unique_ptr<Algorithm> frameCutter(
        AlgorithmFactory::create("FrameCutter",
                                 "frameSize", frameSize,
                                 "hopSize", hopSize,
                                 "startFromZero", true,
                                 "lastFrameToEndOfFile", false));
    std::unique_ptr<Algorithm> windowing(
        AlgorithmFactory::create("Windowing",
                                 "type", std::string("hann"),
                                 "size", frameSize,
                                 "zeroPadding", 0));
    std::unique_ptr<Algorithm> spectrumAlg(
        AlgorithmFactory::create("Spectrum",
                                 "size", frameSize));

    // RGB bands: low 20-250 Hz, mid 250-4000 Hz, high 4000-20000 Hz.
    std::unique_ptr<Algorithm> bandLow(
        AlgorithmFactory::create("EnergyBand",
                                 "sampleRate", static_cast<Real>(sampleRate),
                                 "startCutoffFrequency", 20.0,
                                 "stopCutoffFrequency", 250.0));
    std::unique_ptr<Algorithm> bandMid(
        AlgorithmFactory::create("EnergyBand",
                                 "sampleRate", static_cast<Real>(sampleRate),
                                 "startCutoffFrequency", 250.0,
                                 "stopCutoffFrequency", 4000.0));
    std::unique_ptr<Algorithm> bandHigh(
        AlgorithmFactory::create("EnergyBand",
                                 "sampleRate", static_cast<Real>(sampleRate),
                                 "startCutoffFrequency", 4000.0,
                                 "stopCutoffFrequency", 20000.0));

    frameCutter->input("signal").set(monoSignal);
    frameCutter->output("frame").set(frame);

    windowing->input("frame").set(frame);
    windowing->output("frame").set(windowedFrame);

    spectrumAlg->input("frame").set(windowedFrame);
    spectrumAlg->output("spectrum").set(spectrum);

    bandLow->input("spectrum").set(spectrum);
    bandLow->output("energyBand").set(low);
    bandMid->input("spectrum").set(spectrum);
    bandMid->output("energyBand").set(mid);
    bandHigh->input("spectrum").set(spectrum);
    bandHigh->output("energyBand").set(high);

    std::vector<float> lows;
    std::vector<float> mids;
    std::vector<float> highs;
    std::vector<float> rmsVals;
    lows.reserve(monoSignal.size() / static_cast<size_t>(hopSize));
    mids.reserve(lows.capacity());
    highs.reserve(lows.capacity());
    rmsVals.reserve(lows.capacity());

    while (true) {
        frameCutter->compute();
        if (frame.empty())
            break;

        windowing->compute();
        spectrumAlg->compute();
        bandLow->compute();
        bandMid->compute();
        bandHigh->compute();

        double sumSq = 0.0;
        for (Real s : frame)
            sumSq += static_cast<double>(s) * static_cast<double>(s);
        const float rms = static_cast<float>(std::sqrt(sumSq / std::max<size_t>(1, frame.size())));

        lows.push_back(std::max(0.0f, static_cast<float>(low)));
        mids.push_back(std::max(0.0f, static_cast<float>(mid)));
        highs.push_back(std::max(0.0f, static_cast<float>(high)));
        rmsVals.push_back(rms);
    }

    if (lows.empty())
        return result;

    const float maxLow = std::max(1e-9f, *std::max_element(lows.begin(), lows.end()));
    const float maxMid = std::max(1e-9f, *std::max_element(mids.begin(), mids.end()));
    const float maxHigh = std::max(1e-9f, *std::max_element(highs.begin(), highs.end()));
    const float maxRms = std::max(1e-9f, *std::max_element(rmsVals.begin(), rmsVals.end()));

    result.reserve(static_cast<int>(lows.size()));
    for (size_t i = 0; i < lows.size(); ++i) {
        const float ln = std::clamp(lows[i] / maxLow, 0.0f, 1.0f);
        const float mn = std::clamp(mids[i] / maxMid, 0.0f, 1.0f);
        const float hn = std::clamp(highs[i] / maxHigh, 0.0f, 1.0f);
        const float rmsN = std::clamp(rmsVals[i] / maxRms, 0.0f, 1.0f);

        // Color mixing: R=low, G=mid, B=high with stronger lift for readability.
        int r = std::clamp(static_cast<int>(std::pow(ln, 0.62f) * 255.0f * 1.12f + 10.0f), 0, 255);
        int g = std::clamp(static_cast<int>(std::pow(mn, 0.62f) * 255.0f * 1.12f + 10.0f), 0, 255);
        int b = std::clamp(static_cast<int>(std::pow(hn, 0.62f) * 255.0f * 1.12f + 10.0f), 0, 255);
        if (ln + mn + hn > 0.06f) {
            r = std::max(r, 18);
            g = std::max(g, 18);
            b = std::max(b, 18);
        }

        const float amp = std::clamp(0.55f * std::max({ln, mn, hn}) + 0.45f * rmsN, 0.0f, 1.0f);

        TrackData::RgbWaveformFrame f;
        f.color = QColor(r, g, b, 230);
        f.rms = amp;
        f.low = ln;
        f.mid = mn;
        f.high = hn;
        result.push_back(f);
    }

    // Keep RGB frame count aligned with waveform timeline resolution so
    // scrolling waveform length stays correct.
    if (targetFrames > 0 && result.size() != targetFrames) {
        QVector<TrackData::RgbWaveformFrame> resampled;
        resampled.reserve(targetFrames);
        const int srcN = result.size();
        for (int i = 0; i < targetFrames; ++i) {
            const int s0 = static_cast<int>((static_cast<int64_t>(i) * srcN) / targetFrames);
            int s1 = static_cast<int>((static_cast<int64_t>(i + 1) * srcN) / targetFrames);
            s1 = std::max(s0 + 1, std::min(s1, srcN));

            float maxRms = 0.0f;
            float low = 0.0f;
            float midV = 0.0f;
            float highV = 0.0f;
            float wr = 0.0f;
            float wg = 0.0f;
            float wb = 0.0f;
            float wsum = 0.0f;

            for (int j = s0; j < s1; ++j) {
                const auto& f = result[j];
                maxRms = std::max(maxRms, f.rms);
                low = std::max(low, f.low);
                midV = std::max(midV, f.mid);
                highV = std::max(highV, f.high);
                const float w = std::max(0.08f, f.rms);
                wr += static_cast<float>(f.color.red()) * w;
                wg += static_cast<float>(f.color.green()) * w;
                wb += static_cast<float>(f.color.blue()) * w;
                wsum += w;
            }

            TrackData::RgbWaveformFrame out;
            out.rms = maxRms;
            out.low = low;
            out.mid = midV;
            out.high = highV;
            if (wsum > 0.0f)
                out.color = QColor(static_cast<int>(wr / wsum), static_cast<int>(wg / wsum), static_cast<int>(wb / wsum), 230);
            resampled.push_back(out);
        }
        return resampled;
    }

    return result;
}

#endif

} // namespace

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

struct FiltState {
    std::vector<float> lp110;
    std::vector<float> hp150s1, hp150s2, lp160;
    std::vector<float> hp180s1, hp180s2, lp800;
    std::vector<float> hp19k;
    std::vector<float> svfIc1, svfIc2;
    EnvelopeFollower envLow, envLowMid, envMid, envHigh;

    void reset(int numCh, double sampleRate) {
        const size_t n = static_cast<size_t>(numCh);
        lp110.assign(n, 0.0f);
        hp150s1.assign(n, 0.0f); hp150s2.assign(n, 0.0f); lp160.assign(n, 0.0f);
        hp180s1.assign(n, 0.0f); hp180s2.assign(n, 0.0f); lp800.assign(n, 0.0f);
        hp19k.assign(n, 0.0f);
        svfIc1.assign(n, 0.0f); svfIc2.assign(n, 0.0f);
        envLow.reset();    envLow.prepare(sampleRate, 0.0f, 35.0f);
        envLowMid.reset(); envLowMid.prepare(sampleRate, 0.0f, 25.0f);
        envMid.reset();    envMid.prepare(sampleRate, 0.0f, 15.0f);
        envHigh.reset();   envHigh.prepare(sampleRate, 0.0f, 5.0f);
    }
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

QVector<TrackData::RgbWaveformFrame> waveform_internal::buildInstantOverview(
    juce::AudioFormatReader* reader, int maxBins)
{
    QVector<TrackData::RgbWaveformFrame> out;
    if (!reader || reader->lengthInSamples <= 0 || maxBins <= 0)
        return out;

    const juce::int64 totalSamples = reader->lengthInSamples;
    const int numCh = static_cast<int>(std::max<unsigned int>(reader->numChannels, 1u));
    maxBins = std::clamp(maxBins, 64, TrackData::kProgressiveBins);
    out.resize(maxBins);

    constexpr int kMaxSamplesPerBin = 128;
    juce::AudioBuffer<float> buf(numCh, kMaxSamplesPerBin);

    for (int bin = 0; bin < maxBins; ++bin) {
        const juce::int64 binStart = (static_cast<juce::int64>(bin) * totalSamples) / maxBins;
        const juce::int64 binEnd   = (static_cast<juce::int64>(bin + 1) * totalSamples) / maxBins;
        const juce::int64 span     = std::max<juce::int64>(1, binEnd - binStart);
        const int stride = static_cast<int>(std::max<juce::int64>(1, span / kMaxSamplesPerBin));

        float peak = 0.0f;
        float rmsAcc = 0.0f;
        float lowAcc = 0.0f;
        float highAcc = 0.0f;
        int sampleCount = 0;
        float prevMono = 0.0f;

        for (juce::int64 pos = binStart; pos < binEnd; pos += stride) {
            const int requested = static_cast<int>(std::min<juce::int64>(stride, binEnd - pos));
            const int toRead = std::min(requested, kMaxSamplesPerBin);
            buf.clear();
            reader->read(&buf, 0, toRead, pos, true, true);

            for (int i = 0; i < toRead; ++i) {
                float mono = 0.0f;
                for (int ch = 0; ch < numCh; ++ch)
                    mono += buf.getSample(ch, i);
                mono /= static_cast<float>(numCh);

                const float absS = std::abs(mono);
                peak = std::max(peak, absS);
                rmsAcc += absS * absS;
                lowAcc += absS;
                highAcc += std::abs(mono - prevMono);
                prevMono = mono;
                ++sampleCount;
            }
        }

        if (sampleCount <= 0)
            continue;

        const float rms = std::sqrt(rmsAcc / static_cast<float>(sampleCount));
        const float lowN = std::clamp(lowAcc / static_cast<float>(sampleCount) * 3.5f, 0.0f, 1.0f);
        const float highN = std::clamp(highAcc / static_cast<float>(sampleCount) * 8.0f, 0.0f, 1.0f);
        const float midN = std::clamp((lowN + highN) * 0.45f, 0.0f, 1.0f);
        const float lowMidN = std::clamp(lowN * 0.75f, 0.0f, 1.0f);
        const float rmsN = std::clamp(std::log1p(rms * 12.0f) / std::log1p(12.0f), 0.0f, 1.0f);

        auto& frame = out[bin];
        frame.rms = rmsN;
        frame.low = lowN;
        frame.lowMid = lowMidN;
        frame.mid = midN;
        frame.high = highN;
    }

    return out;
}


namespace waveform_internal {

bool runEnvelopePass(const EnvelopePassInput& input)
{
    auto* m_trackData = input.trackData;
    auto& reader = input.reader;
    auto& thread = input.thread;
    const int m_pointsPerSecond = input.pointsPerSecond;
    const double activeSeekHint = input.seekHintSec;
    const juce::int64 totalSamples = input.totalSamples;
    const double sampleRate = input.sampleRate;
    const int numPoints = input.numPoints;

    auto threadShouldExit = [&]() { return thread.threadShouldExit(); };

    m_trackData->reportAnalysisProgress(0.02, true);

    // Use exact integer-ratio partitioning per bin to avoid cumulative timeline
    // drift on long tracks (which otherwise degrades quality toward the end).
    const juce::int64 maxSamplesPerBin = std::max<juce::int64>(
        1,
        (totalSamples + static_cast<juce::int64>(numPoints) - 1)
            / static_cast<juce::int64>(numPoints));

    // -------------------------------------------------------------------------
    // DSP-Kette: Parallel 4-Band Filterbank (overlapping)
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

    const int numCh = static_cast<int>(reader.numChannels);
    const float sr  = static_cast<float>(sampleRate);

    // ── 1-pole LP coefficient: a = 2π·fc / (2π·fc + sr) ─────────────────────
    auto lpCoef1 = [&](float fc) -> float {
        const float w = 2.0f * juce::MathConstants<float>::pi * fc / sr;
        return w / (w + 1.0f);
    };

    // ── Band 1: LP @ 110 Hz (1st order = 6 dB/oct) ──────────────────────────
    const float aLP110 = lpCoef1(110.0f);

    // ── Band 2: HP @ 150 Hz (2nd order) + LP @ 160 Hz (1st order) ───────────
    //    2nd order HP = two cascaded 1st-order HP stages.
    //    HP coefficient: same as LP but applied as HP (out = in - lp).
    const float aHP150 = lpCoef1(150.0f);
    const float aLP160 = lpCoef1(160.0f);

    // ── Band 3: HP @ 180 Hz (2nd order) + LP @ 800 Hz (1st order) ───────────
    const float aHP180 = lpCoef1(180.0f);
    const float aLP800 = lpCoef1(800.0f);

    // ── Band 4: BP @ 2750 Hz (resonant) + HP @ 19000 Hz ─────────────────────
    //    Sub-path A: 2nd-order resonant BP at 2750 Hz using SVF (State Variable TPT).
    //    Sub-path B: HP @ 19000 Hz (1st order) for extreme hi-hat ticks.
    //    Final = abs(A) + abs(B).
    const float aHP19k = lpCoef1(19000.0f);

    // SVF (State Variable Filter) for the 2750 Hz resonant BP.
    // g = tan(π·fc/sr), R = 1/(2·Q) — Q=2 for moderate resonance.
    const float svfG = std::tan(juce::MathConstants<float>::pi * 2750.0f / sr);
    const float svfR = 1.0f / (2.0f * 2.0f);  // Q = 2
    const float svfD = 1.0f / (1.0f + 2.0f * svfR * svfG + svfG * svfG);

    // ── Filter state (main sequential pass) ──────────────────────────────────
    FiltState mainFilt;
    mainFilt.reset(numCh, sampleRate);

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
    std::vector<RawBin> rawBins(static_cast<size_t>(numPoints));

    // Peak mipmap: signed min/max per high-res bin (4× analysis rate).
    // Allows the renderer to show actual audio oscillations at high zoom.
    struct RawPeak { float minRaw = 0.0f; float maxRaw = 0.0f; };
    const int PEAK_RATIO = TrackData::PEAK_POINTS_PER_SECOND / m_pointsPerSecond;  // = 4
    const int numPeakPoints = numPoints * PEAK_RATIO;
    std::vector<RawPeak> rawPeakBuf(static_cast<size_t>(numPeakPoints));
    float globalMaxSample = 0.001f;

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

    constexpr int kChunk = 128;
    const bool enableForwardPrefetch = activeSeekHint > 3.0;
    QVector<TrackData::WaveformBin> previewBatch;
    previewBatch.reserve(kChunk);
    QVector<TrackData::RgbWaveformFrame> previewRgbBatch;
    previewRgbBatch.reserve(kChunk);

    // Shared shaping helper — used identically in preview AND final pass.
    auto shapeBin = [](float norm, float expo, float gain) -> float {
        return std::min(1.0f, std::pow(std::clamp(norm, 0.0f, 1.0f), expo) * gain);
    };

    juce::AudioBuffer<float> readBuf(static_cast<int>(reader.numChannels), static_cast<int>(maxSamplesPerBin));

    // ─── Priority window: analyze around seek hint first ─────────────────
    // This gives immediate waveform feedback at the seek position before the
    // sequential fill pass reaches it. Uses cold-start filters (tiny warmup
    // error in first ~20 bins, invisible at display scale).
    constexpr int kWarmupBins    = 30;   // ~50 ms at 600 pps
    constexpr int kPriorityBins  = 1800; // 3 s ahead of hint

    m_trackData->preallocateRgbWaveform(numPoints);

    const int hintBin = std::clamp(
        static_cast<int>(activeSeekHint * m_pointsPerSecond),
        0, numPoints - 1);
    const int priorityWarmupStart = std::max(0, hintBin - kWarmupBins);
    const int priorityEnd         = std::min(numPoints, hintBin + kPriorityBins);
    const bool hasPriority = hintBin > kPriorityBins; // only if actually ahead

    if (hasPriority) {
        FiltState pFilt;
        pFilt.reset(numCh, sampleRate);
        juce::AudioBuffer<float> pBuf(static_cast<int>(reader.numChannels), static_cast<int>(maxSamplesPerBin));

        float pRunMaxLow = 0.1f, pRunMaxLowMid = 0.1f, pRunMaxMid = 0.1f, pRunMaxHigh = 0.1f;
        QVector<TrackData::RgbWaveformFrame> pChunk;
        pChunk.reserve(kChunk);
        int pChunkStart = hintBin;

        // Warmup (no emit)
        for (int bin = priorityWarmupStart; bin < hintBin && !threadShouldExit(); ++bin) {
            const juce::int64 binStart = (static_cast<juce::int64>(bin) * totalSamples) / numPoints;
            juce::int64 binEnd = (static_cast<juce::int64>(bin + 1) * totalSamples) / numPoints;
            if (binEnd <= binStart) binEnd = std::min(totalSamples, binStart + 1);
            const int toRead = static_cast<int>(std::max<juce::int64>(1, binEnd - binStart));
            reader.read(&pBuf, 0, toRead, binStart, true, false);

            for (int s = 0; s < toRead; ++s) {
                float bL = 0, bLM = 0, bM = 0, bH = 0;
                for (int ch = 0; ch < numCh; ++ch) {
                    const size_t ci = static_cast<size_t>(ch);
                    const float in = pBuf.getReadPointer(ch)[s];
                    pFilt.lp110[ci] = aLP110 * in + (1.0f - aLP110) * pFilt.lp110[ci];
                    const float b1 = std::abs(pFilt.lp110[ci]);
                    pFilt.hp150s1[ci] = aHP150 * in + (1.0f - aHP150) * pFilt.hp150s1[ci];
                    const float hp1o = in - pFilt.hp150s1[ci];
                    pFilt.hp150s2[ci] = aHP150 * hp1o + (1.0f - aHP150) * pFilt.hp150s2[ci];
                    pFilt.lp160[ci] = aLP160 * (hp1o - pFilt.hp150s2[ci]) + (1.0f - aLP160) * pFilt.lp160[ci];
                    const float b2 = std::abs(pFilt.lp160[ci]);
                    pFilt.hp180s1[ci] = aHP180 * in + (1.0f - aHP180) * pFilt.hp180s1[ci];
                    const float hp3o = in - pFilt.hp180s1[ci];
                    pFilt.hp180s2[ci] = aHP180 * hp3o + (1.0f - aHP180) * pFilt.hp180s2[ci];
                    pFilt.lp800[ci] = aLP800 * (hp3o - pFilt.hp180s2[ci]) + (1.0f - aLP800) * pFilt.lp800[ci];
                    const float b3 = std::abs(pFilt.lp800[ci]);
                    const float v3 = in - pFilt.svfIc2[ci];
                    const float v1 = svfD * (pFilt.svfIc1[ci] + svfG * v3);
                    const float v2 = pFilt.svfIc2[ci] + svfG * v1;
                    pFilt.svfIc1[ci] = 2.0f * v1 - pFilt.svfIc1[ci];
                    pFilt.svfIc2[ci] = 2.0f * v2 - pFilt.svfIc2[ci];
                    pFilt.hp19k[ci] = aHP19k * in + (1.0f - aHP19k) * pFilt.hp19k[ci];
                    const float b4 = std::abs(v1) + std::abs(in - pFilt.hp19k[ci]);
                    if (b1 > bL) bL = b1; if (b2 > bLM) bLM = b2;
                    if (b3 > bM) bM = b3; if (b4 > bH)  bH  = b4;
                }
                pFilt.envLow.process(bL); pFilt.envLowMid.process(bLM);
                pFilt.envMid.process(bM); pFilt.envHigh.process(bH);
            }
            RawBin rb; rb.low = pFilt.envLow.state; rb.lowMid = pFilt.envLowMid.state;
            rb.mid = pFilt.envMid.state; rb.high = pFilt.envHigh.state;
            rawBins[static_cast<size_t>(bin)] = rb;
        }

        // Priority window — emit immediately
        for (int bin = hintBin; bin < priorityEnd && !threadShouldExit(); ++bin) {
            const juce::int64 binStart = (static_cast<juce::int64>(bin) * totalSamples) / numPoints;
            juce::int64 binEnd = (static_cast<juce::int64>(bin + 1) * totalSamples) / numPoints;
            if (binEnd <= binStart) binEnd = std::min(totalSamples, binStart + 1);
            const int toRead = static_cast<int>(std::max<juce::int64>(1, binEnd - binStart));
            reader.read(&pBuf, 0, toRead, binStart, true, false);

            for (int s = 0; s < toRead; ++s) {
                float bL = 0, bLM = 0, bM = 0, bH = 0;
                for (int ch = 0; ch < numCh; ++ch) {
                    const size_t ci = static_cast<size_t>(ch);
                    const float in = pBuf.getReadPointer(ch)[s];
                    pFilt.lp110[ci] = aLP110 * in + (1.0f - aLP110) * pFilt.lp110[ci];
                    const float b1 = std::abs(pFilt.lp110[ci]);
                    pFilt.hp150s1[ci] = aHP150 * in + (1.0f - aHP150) * pFilt.hp150s1[ci];
                    const float hp1o = in - pFilt.hp150s1[ci];
                    pFilt.hp150s2[ci] = aHP150 * hp1o + (1.0f - aHP150) * pFilt.hp150s2[ci];
                    pFilt.lp160[ci] = aLP160 * (hp1o - pFilt.hp150s2[ci]) + (1.0f - aLP160) * pFilt.lp160[ci];
                    const float b2 = std::abs(pFilt.lp160[ci]);
                    pFilt.hp180s1[ci] = aHP180 * in + (1.0f - aHP180) * pFilt.hp180s1[ci];
                    const float hp3o = in - pFilt.hp180s1[ci];
                    pFilt.hp180s2[ci] = aHP180 * hp3o + (1.0f - aHP180) * pFilt.hp180s2[ci];
                    pFilt.lp800[ci] = aLP800 * (hp3o - pFilt.hp180s2[ci]) + (1.0f - aLP800) * pFilt.lp800[ci];
                    const float b3 = std::abs(pFilt.lp800[ci]);
                    const float v3 = in - pFilt.svfIc2[ci];
                    const float v1 = svfD * (pFilt.svfIc1[ci] + svfG * v3);
                    const float v2 = pFilt.svfIc2[ci] + svfG * v1;
                    pFilt.svfIc1[ci] = 2.0f * v1 - pFilt.svfIc1[ci];
                    pFilt.svfIc2[ci] = 2.0f * v2 - pFilt.svfIc2[ci];
                    pFilt.hp19k[ci] = aHP19k * in + (1.0f - aHP19k) * pFilt.hp19k[ci];
                    const float b4 = std::abs(v1) + std::abs(in - pFilt.hp19k[ci]);
                    if (b1 > bL) bL = b1; if (b2 > bLM) bLM = b2;
                    if (b3 > bM) bM = b3; if (b4 > bH)  bH  = b4;
                }
                pFilt.envLow.process(bL); pFilt.envLowMid.process(bLM);
                pFilt.envMid.process(bM); pFilt.envHigh.process(bH);
            }
            RawBin rb; rb.low = pFilt.envLow.state; rb.lowMid = pFilt.envLowMid.state;
            rb.mid = pFilt.envMid.state; rb.high = pFilt.envHigh.state;
            rawBins[static_cast<size_t>(bin)] = rb;

            if (rb.low > pRunMaxLow) pRunMaxLow = rb.low;
            if (rb.lowMid > pRunMaxLowMid) pRunMaxLowMid = rb.lowMid;
            if (rb.mid > pRunMaxMid) pRunMaxMid = rb.mid;
            if (rb.high > pRunMaxHigh) pRunMaxHigh = rb.high;

            TrackData::RgbWaveformFrame rgb;
            const float lowN    = shapeBin(rb.low    / pRunMaxLow,    1.8f, 1.0f);
            const float lowMidN = shapeBin(rb.lowMid / pRunMaxLowMid, 1.6f, 0.9f);
            const float midN    = shapeBin(rb.mid    / pRunMaxMid,    1.5f, 0.7f);
            const float highN   = shapeBin(rb.high   / pRunMaxHigh,   1.3f, 0.5f);
            const float rmsN    = std::clamp(0.5f * std::max({lowN, lowMidN, midN, highN}) + 0.5f * ((lowN + lowMidN + midN + highN) / 4.0f), 0.0f, 1.0f);
            rgb.color = QColor(255, 255, 255, 230);
            rgb.rms = rmsN; rgb.low = lowN; rgb.lowMid = lowMidN; rgb.mid = midN; rgb.high = highN;
            if (pChunk.isEmpty()) pChunkStart = bin;
            pChunk.append(rgb);
            if (pChunk.size() >= kChunk) {
                m_trackData->writeRgbWaveformRange(pChunkStart, pChunk);
                pChunk.clear();
            }
        }
        if (!pChunk.isEmpty())
            m_trackData->writeRgbWaveformRange(pChunkStart, pChunk);
    }
    // ─────────────────────────────────────────────────────────────────────

    int mainChunkStart = 0;

    // Bins before the priority region are buffered here and flushed only after
    // the forward region (priorityEnd..N) begins, so the user sees waveform
    // continue forward from the seek point before the earlier section fills in.
    QVector<TrackData::RgbWaveformFrame> earlyRgbBuf;
    if (hasPriority) earlyRgbBuf.reserve(priorityWarmupStart);
    int earlyRgbStart = 0;
    bool earlyFlushed = !hasPriority;

    // Tracks how far ahead we've pre-rendered via cold-start priority passes.
    // Advances by kPriorityBins every 200 main-loop iterations so the waveform
    // continues filling forward from the seek point instead of restarting at 0.
    int forwardFrontier = hasPriority ? priorityEnd : 0;
    // Tracks which hint position the current frontier run was started from.
    // When the user seeks significantly (forward or backward), frontier resets.
    int lastActedHint = hasPriority ? hintBin : 0;

    for (int bin = 0; bin < numPoints; ++bin)
    {
        if (threadShouldExit()) break;

        // Yield occasionally without sleeping — keeps UI/audio responsive without
        // throttling analysis to "grandma speed".
        if ((bin & 0xFFF) == 0)
            juce::Thread::yield();

        if ((bin & 0x7F) == 0)
            m_trackData->reportAnalysisProgress(
                (static_cast<double>(bin) / static_cast<double>(numPoints)) * 0.50, true);

        // When entering the priority region: flush only the appendData batch
        // (overview waveform). RGB for early bins stays in earlyRgbBuf.
        if (hasPriority && bin == priorityWarmupStart && !previewBatch.isEmpty()) {
            m_trackData->appendData(previewBatch);
            previewBatch.clear();
            previewRgbBatch.clear();
        }

        // When leaving the priority region: flush the early-bin RGB buffer so
        // the forward fill is visible before going back to fill the beginning.
        if (!earlyFlushed && bin >= priorityEnd) {
            if (!earlyRgbBuf.isEmpty())
                m_trackData->writeRgbWaveformRange(earlyRgbStart, earlyRgbBuf);
            earlyRgbBuf.clear();
            earlyFlushed = true;
        }

        // Every 200 bins, advance the forward frontier ahead of the main loop.
        // Each pass extends the pre-rendered region by kPriorityBins, so the waveform
        // continues filling forward continuously until the main loop catches up.
        if (enableForwardPrefetch && bin > 0 && bin % 400 == 0) {
            // Check for a new user seek in either direction — must happen before
            // the frontier gate so a backward seek (or cold start) still takes effect.
            {
                const int latestHint = std::clamp(
                    static_cast<int>(activeSeekHint * m_pointsPerSecond),
                    0, numPoints - 1);
                if (std::abs(latestHint - lastActedHint) > kPriorityBins) {
                    forwardFrontier = latestHint;
                    lastActedHint   = latestHint;
                }
            }
            if (forwardFrontier < numPoints && forwardFrontier > bin) {
            const int nWarmStart   = std::max(0, forwardFrontier - kWarmupBins);
            const int nPriorityEnd = std::min(numPoints, forwardFrontier + kPriorityBins);
            if (nWarmStart > bin) {
                    FiltState nFilt;
                    nFilt.reset(numCh, sampleRate);
                    juce::AudioBuffer<float> nBuf(static_cast<int>(reader.numChannels), static_cast<int>(maxSamplesPerBin));
                    float nRunMaxLow = 0.1f, nRunMaxLowMid = 0.1f, nRunMaxMid = 0.1f, nRunMaxHigh = 0.1f;
                    QVector<TrackData::RgbWaveformFrame> nChunk;
                    nChunk.reserve(kChunk);
                    int nChunkStart = forwardFrontier;

                    // Warmup
                    for (int wb = nWarmStart; wb < forwardFrontier && !threadShouldExit(); ++wb) {
                        const juce::int64 wBinStart = (static_cast<juce::int64>(wb) * totalSamples) / numPoints;
                        juce::int64 wBinEnd = (static_cast<juce::int64>(wb + 1) * totalSamples) / numPoints;
                        if (wBinEnd <= wBinStart) wBinEnd = std::min(totalSamples, wBinStart + 1);
                        const int wToRead = static_cast<int>(std::max<juce::int64>(1, wBinEnd - wBinStart));
                        reader.read(&nBuf, 0, wToRead, wBinStart, true, false);
                        for (int s = 0; s < wToRead; ++s) {
                            float bL = 0, bLM = 0, bM = 0, bH = 0;
                            for (int ch = 0; ch < numCh; ++ch) {
                                const size_t ci = static_cast<size_t>(ch);
                                const float in = nBuf.getReadPointer(ch)[s];
                                nFilt.lp110[ci] = aLP110 * in + (1.0f - aLP110) * nFilt.lp110[ci];
                                const float b1 = std::abs(nFilt.lp110[ci]);
                                nFilt.hp150s1[ci] = aHP150 * in + (1.0f - aHP150) * nFilt.hp150s1[ci];
                                const float hp1o = in - nFilt.hp150s1[ci];
                                nFilt.hp150s2[ci] = aHP150 * hp1o + (1.0f - aHP150) * nFilt.hp150s2[ci];
                                nFilt.lp160[ci] = aLP160 * (hp1o - nFilt.hp150s2[ci]) + (1.0f - aLP160) * nFilt.lp160[ci];
                                const float b2 = std::abs(nFilt.lp160[ci]);
                                nFilt.hp180s1[ci] = aHP180 * in + (1.0f - aHP180) * nFilt.hp180s1[ci];
                                const float hp3o = in - nFilt.hp180s1[ci];
                                nFilt.hp180s2[ci] = aHP180 * hp3o + (1.0f - aHP180) * nFilt.hp180s2[ci];
                                nFilt.lp800[ci] = aLP800 * (hp3o - nFilt.hp180s2[ci]) + (1.0f - aLP800) * nFilt.lp800[ci];
                                const float b3 = std::abs(nFilt.lp800[ci]);
                                const float v3 = in - nFilt.svfIc2[ci];
                                const float v1 = svfD * (nFilt.svfIc1[ci] + svfG * v3);
                                const float v2 = nFilt.svfIc2[ci] + svfG * v1;
                                nFilt.svfIc1[ci] = 2.0f * v1 - nFilt.svfIc1[ci];
                                nFilt.svfIc2[ci] = 2.0f * v2 - nFilt.svfIc2[ci];
                                nFilt.hp19k[ci] = aHP19k * in + (1.0f - aHP19k) * nFilt.hp19k[ci];
                                const float b4 = std::abs(v1) + std::abs(in - nFilt.hp19k[ci]);
                                if (b1 > bL) bL = b1; if (b2 > bLM) bLM = b2;
                                if (b3 > bM) bM = b3; if (b4 > bH)  bH  = b4;
                            }
                            nFilt.envLow.process(bL); nFilt.envLowMid.process(bLM);
                            nFilt.envMid.process(bM); nFilt.envHigh.process(bH);
                        }
                        RawBin nrb; nrb.low = nFilt.envLow.state; nrb.lowMid = nFilt.envLowMid.state;
                        nrb.mid = nFilt.envMid.state; nrb.high = nFilt.envHigh.state;
                        rawBins[static_cast<size_t>(wb)] = nrb;
                    }

                    // Priority window
                    for (int pb = forwardFrontier; pb < nPriorityEnd && !threadShouldExit(); ++pb) {
                        const juce::int64 pBinStart = (static_cast<juce::int64>(pb) * totalSamples) / numPoints;
                        juce::int64 pBinEnd = (static_cast<juce::int64>(pb + 1) * totalSamples) / numPoints;
                        if (pBinEnd <= pBinStart) pBinEnd = std::min(totalSamples, pBinStart + 1);
                        const int pToRead = static_cast<int>(std::max<juce::int64>(1, pBinEnd - pBinStart));
                        reader.read(&nBuf, 0, pToRead, pBinStart, true, false);
                        for (int s = 0; s < pToRead; ++s) {
                            float bL = 0, bLM = 0, bM = 0, bH = 0;
                            for (int ch = 0; ch < numCh; ++ch) {
                                const size_t ci = static_cast<size_t>(ch);
                                const float in = nBuf.getReadPointer(ch)[s];
                                nFilt.lp110[ci] = aLP110 * in + (1.0f - aLP110) * nFilt.lp110[ci];
                                const float b1 = std::abs(nFilt.lp110[ci]);
                                nFilt.hp150s1[ci] = aHP150 * in + (1.0f - aHP150) * nFilt.hp150s1[ci];
                                const float hp1o = in - nFilt.hp150s1[ci];
                                nFilt.hp150s2[ci] = aHP150 * hp1o + (1.0f - aHP150) * nFilt.hp150s2[ci];
                                nFilt.lp160[ci] = aLP160 * (hp1o - nFilt.hp150s2[ci]) + (1.0f - aLP160) * nFilt.lp160[ci];
                                const float b2 = std::abs(nFilt.lp160[ci]);
                                nFilt.hp180s1[ci] = aHP180 * in + (1.0f - aHP180) * nFilt.hp180s1[ci];
                                const float hp3o = in - nFilt.hp180s1[ci];
                                nFilt.hp180s2[ci] = aHP180 * hp3o + (1.0f - aHP180) * nFilt.hp180s2[ci];
                                nFilt.lp800[ci] = aLP800 * (hp3o - nFilt.hp180s2[ci]) + (1.0f - aLP800) * nFilt.lp800[ci];
                                const float b3 = std::abs(nFilt.lp800[ci]);
                                const float v3 = in - nFilt.svfIc2[ci];
                                const float v1 = svfD * (nFilt.svfIc1[ci] + svfG * v3);
                                const float v2 = nFilt.svfIc2[ci] + svfG * v1;
                                nFilt.svfIc1[ci] = 2.0f * v1 - nFilt.svfIc1[ci];
                                nFilt.svfIc2[ci] = 2.0f * v2 - nFilt.svfIc2[ci];
                                nFilt.hp19k[ci] = aHP19k * in + (1.0f - aHP19k) * nFilt.hp19k[ci];
                                const float b4 = std::abs(v1) + std::abs(in - nFilt.hp19k[ci]);
                                if (b1 > bL) bL = b1; if (b2 > bLM) bLM = b2;
                                if (b3 > bM) bM = b3; if (b4 > bH)  bH  = b4;
                            }
                            nFilt.envLow.process(bL); nFilt.envLowMid.process(bLM);
                            nFilt.envMid.process(bM); nFilt.envHigh.process(bH);
                        }
                        RawBin nrb; nrb.low = nFilt.envLow.state; nrb.lowMid = nFilt.envLowMid.state;
                        nrb.mid = nFilt.envMid.state; nrb.high = nFilt.envHigh.state;
                        rawBins[static_cast<size_t>(pb)] = nrb;

                        if (nrb.low > nRunMaxLow) nRunMaxLow = nrb.low;
                        if (nrb.lowMid > nRunMaxLowMid) nRunMaxLowMid = nrb.lowMid;
                        if (nrb.mid > nRunMaxMid) nRunMaxMid = nrb.mid;
                        if (nrb.high > nRunMaxHigh) nRunMaxHigh = nrb.high;

                        TrackData::RgbWaveformFrame rgb;
                        const float lowN    = shapeBin(nrb.low    / nRunMaxLow,    1.8f, 1.0f);
                        const float lowMidN = shapeBin(nrb.lowMid / nRunMaxLowMid, 1.6f, 0.9f);
                        const float midN    = shapeBin(nrb.mid    / nRunMaxMid,    1.5f, 0.7f);
                        const float highN   = shapeBin(nrb.high   / nRunMaxHigh,   1.3f, 0.5f);
                        const float rmsN    = std::clamp(0.5f * std::max({lowN, lowMidN, midN, highN}) + 0.5f * ((lowN + lowMidN + midN + highN) / 4.0f), 0.0f, 1.0f);
                        rgb.color = QColor(255, 255, 255, 230);
                        rgb.rms = rmsN; rgb.low = lowN; rgb.lowMid = lowMidN; rgb.mid = midN; rgb.high = highN;
                        if (nChunk.isEmpty()) nChunkStart = pb;
                        nChunk.append(rgb);
                        if (nChunk.size() >= kChunk) {
                            m_trackData->writeRgbWaveformRange(nChunkStart, nChunk);
                            nChunk.clear();
                        }
                    }
                    if (!nChunk.isEmpty())
                        m_trackData->writeRgbWaveformRange(nChunkStart, nChunk);

                    forwardFrontier = nPriorityEnd; // advance frontier for next iteration
                }
            }
        }

        const juce::int64 binStart = (static_cast<juce::int64>(bin) * totalSamples)
                                   / static_cast<juce::int64>(numPoints);
        juce::int64 binEnd = (static_cast<juce::int64>(bin + 1) * totalSamples)
                           / static_cast<juce::int64>(numPoints);
        if (binEnd <= binStart)
            binEnd = std::min(totalSamples, binStart + 1);

        const int toRead = static_cast<int>(std::max<juce::int64>(1, binEnd - binStart));
        reader.read(&readBuf, 0, toRead, binStart, true, false);

        struct SubPeak { float min = 0.0f; float max = 0.0f; bool init = false; };
        std::array<SubPeak, 8> subPeaks{};
        const int peakRatioClamped = std::clamp(PEAK_RATIO, 1, static_cast<int>(subPeaks.size()));

        for (int s = 0; s < toRead; ++s)
        {
            float bestLow = 0.0f, bestLowMid = 0.0f, bestMid = 0.0f, bestHigh = 0.0f;
            float monoAcc = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
            {
                const size_t ci = static_cast<size_t>(ch);
                const float in = readBuf.getReadPointer(ch)[s];
                monoAcc += in;

                // ── Band 1: LP @ 110 Hz (1st order) ─────────────────────────
                mainFilt.lp110[ci] = aLP110 * in + (1.0f - aLP110) * mainFilt.lp110[ci];
                const float b1 = std::abs(mainFilt.lp110[ci]);

                // ── Band 2: HP @ 150 Hz (2nd order) → LP @ 160 Hz ───────────
                mainFilt.hp150s1[ci] = aHP150 * in + (1.0f - aHP150) * mainFilt.hp150s1[ci];
                const float hp1out = in - mainFilt.hp150s1[ci];
                mainFilt.hp150s2[ci] = aHP150 * hp1out + (1.0f - aHP150) * mainFilt.hp150s2[ci];
                const float hp2out = hp1out - mainFilt.hp150s2[ci];
                mainFilt.lp160[ci] = aLP160 * hp2out + (1.0f - aLP160) * mainFilt.lp160[ci];
                const float b2 = std::abs(mainFilt.lp160[ci]);

                // ── Band 3: HP @ 180 Hz (2nd order) → LP @ 800 Hz ───────────
                mainFilt.hp180s1[ci] = aHP180 * in + (1.0f - aHP180) * mainFilt.hp180s1[ci];
                const float hp3out = in - mainFilt.hp180s1[ci];
                mainFilt.hp180s2[ci] = aHP180 * hp3out + (1.0f - aHP180) * mainFilt.hp180s2[ci];
                const float hp4out = hp3out - mainFilt.hp180s2[ci];
                mainFilt.lp800[ci] = aLP800 * hp4out + (1.0f - aLP800) * mainFilt.lp800[ci];
                const float b3 = std::abs(mainFilt.lp800[ci]);

                // ── Band 4: Resonant BP @ 2750 Hz + HP @ 19 kHz ─────────────
                const float v3 = in - mainFilt.svfIc2[ci];
                const float v1 = svfD * (mainFilt.svfIc1[ci] + svfG * v3);
                const float v2 = mainFilt.svfIc2[ci] + svfG * v1;
                mainFilt.svfIc1[ci] = 2.0f * v1 - mainFilt.svfIc1[ci];
                mainFilt.svfIc2[ci] = 2.0f * v2 - mainFilt.svfIc2[ci];
                const float bp2750 = v1;

                mainFilt.hp19k[ci] = aHP19k * in + (1.0f - aHP19k) * mainFilt.hp19k[ci];
                const float hp19kVal = in - mainFilt.hp19k[ci];

                const float b4 = std::abs(bp2750) + std::abs(hp19kVal);

                if (b1 > bestLow)    bestLow    = b1;
                if (b2 > bestLowMid) bestLowMid = b2;
                if (b3 > bestMid)    bestMid    = b3;
                if (b4 > bestHigh)   bestHigh   = b4;
            }

            const float mono = monoAcc / static_cast<float>(numCh);
            const int pb = std::min(peakRatioClamped - 1,
                                    (s * peakRatioClamped) / std::max(1, toRead));
            auto& sp = subPeaks[static_cast<size_t>(pb)];
            if (!sp.init) {
                sp.min = mono;
                sp.max = mono;
                sp.init = true;
            } else {
                if (mono < sp.min) sp.min = mono;
                if (mono > sp.max) sp.max = mono;
            }

            mainFilt.envLow   .process(bestLow);
            mainFilt.envLowMid.process(bestLowMid);
            mainFilt.envMid   .process(bestMid);
            mainFilt.envHigh  .process(bestHigh);
        }

        {
            const int peakBinBase = bin * PEAK_RATIO;
            for (int pb = 0; pb < peakRatioClamped; ++pb) {
                const auto& sp = subPeaks[static_cast<size_t>(pb)];
                const float pMin = sp.init ? sp.min : 0.0f;
                const float pMax = sp.init ? sp.max : 0.0f;
                rawPeakBuf[static_cast<size_t>(peakBinBase + pb)] = { pMin, pMax };
                if (pMax > globalMaxSample) globalMaxSample = pMax;
                if (-pMin > globalMaxSample) globalMaxSample = -pMin;
            }
        }

        // Store RAW envelope values for the final pass.
        RawBin rb;
        rb.low    = mainFilt.envLow   .state;
        rb.lowMid = mainFilt.envLowMid.state;
        rb.mid    = mainFilt.envMid   .state;
        rb.high   = mainFilt.envHigh  .state;
        rawBins[static_cast<size_t>(bin)] = rb;

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

        previewBatch.append(pbin);

        // Skip emitting RGB for bins already covered by the priority pass.
        const bool inPriorityRegion = hasPriority && bin >= priorityWarmupStart && bin < priorityEnd;
        const bool beforePriority   = hasPriority && !earlyFlushed && bin < priorityWarmupStart;
        if (!inPriorityRegion) {
            TrackData::RgbWaveformFrame rgb;
            const float lowN    = std::clamp(pbin.low,    0.0f, 1.0f);
            const float lowMidN = std::clamp(pbin.lowMid, 0.0f, 1.0f);
            const float midN    = std::clamp(pbin.mid,    0.0f, 1.0f);
            const float highN   = std::clamp(pbin.high,   0.0f, 1.0f);
            const float rmsN    = std::clamp(0.5f * std::max({lowN, lowMidN, midN, highN}) + 0.5f * ((lowN + lowMidN + midN + highN) / 4.0f), 0.0f, 1.0f);
            rgb.color = QColor(255, 255, 255, 230);
            rgb.rms = rmsN; rgb.low = lowN; rgb.lowMid = lowMidN; rgb.mid = midN; rgb.high = highN;
            if (beforePriority) {
                // Defer: emit the early section only after the forward fill starts.
                if (earlyRgbBuf.isEmpty()) earlyRgbStart = bin;
                earlyRgbBuf.append(rgb);
            } else {
                if (previewRgbBatch.isEmpty()) mainChunkStart = bin;
                previewRgbBatch.append(rgb);
            }
        }

        if (previewBatch.size() >= kChunk) {
            m_trackData->appendData(previewBatch);
            if (!previewRgbBatch.isEmpty()) {
                m_trackData->writeRgbWaveformRange(mainChunkStart, previewRgbBatch);
                previewRgbBatch.clear();
            }
            previewBatch.clear();
        }
    }
    if (!previewBatch.isEmpty()) {
        m_trackData->appendData(previewBatch);
        if (!previewRgbBatch.isEmpty())
            m_trackData->writeRgbWaveformRange(mainChunkStart, previewRgbBatch);
    }
    // Flush early-region buffer if priorityEnd was never reached (e.g. hint near end of track).
    if (!earlyFlushed && !earlyRgbBuf.isEmpty())
        m_trackData->writeRgbWaveformRange(earlyRgbStart, earlyRgbBuf);

    if (threadShouldExit()) return false;

    m_trackData->reportAnalysisProgress(0.52, true);

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
    QVector<TrackData::RgbWaveformFrame> polishedRgbData;
    polishedRgbData.reserve(static_cast<int>(rawBins.size()));

    int pass2Idx = 0;
    const int pass2Total = static_cast<int>(rawBins.size());
    for (const RawBin& rb : rawBins)
    {
        if (threadShouldExit()) break;

        if ((pass2Idx & 0x3FFF) == 0 && pass2Total > 0)
            m_trackData->reportAnalysisProgress(
                0.52 + (static_cast<double>(pass2Idx) / static_cast<double>(pass2Total)) * 0.06, true);
        ++pass2Idx;

        // ── Global normalization → shaping → gain → base floor ──────────────
        TrackData::WaveformBin wbin;
        wbin.low    = shapeBin(rb.low    / globalMaxLow,    1.8f, 1.0f);
        wbin.lowMid = shapeBin(rb.lowMid / globalMaxLowMid, 1.6f, 0.9f);
        wbin.mid    = shapeBin(rb.mid    / globalMaxMid,    1.5f, 0.7f);
        wbin.high   = shapeBin(rb.high   / globalMaxHigh,   1.3f, 0.5f);

        finalData.append(wbin);

        TrackData::RgbWaveformFrame rgb;
        const float lowN    = std::clamp(wbin.low,    0.0f, 1.0f);
        const float lowMidN = std::clamp(wbin.lowMid, 0.0f, 1.0f);
        const float midN    = std::clamp(wbin.mid,    0.0f, 1.0f);
        const float highN   = std::clamp(wbin.high,   0.0f, 1.0f);
        const float rmsN    = std::clamp(0.5f * std::max({lowN, lowMidN, midN, highN}) + 0.5f * ((lowN + lowMidN + midN + highN) / 4.0f), 0.0f, 1.0f);
        rgb.color = QColor(255, 255, 255, 230);
        rgb.rms = rmsN; rgb.low = lowN; rgb.lowMid = lowMidN; rgb.mid = midN; rgb.high = highN;
        polishedRgbData.append(rgb);
    }

    if (!threadShouldExit()) {
        m_trackData->replaceAllData(std::move(finalData), globalMaxPeak);
        // Pass 2 output is authoritative — skip the expensive full-vector blend copy.
        m_trackData->setRgbWaveformData(std::move(polishedRgbData));

        if (!rawPeakBuf.empty()) {
            m_trackData->reportAnalysisProgress(0.58, true);
            const float normScale = 127.0f / std::max(0.001f, globalMaxSample);
            QVector<TrackData::PeakFrame> peakMip(static_cast<int>(rawPeakBuf.size()));
            const int peakTotal = static_cast<int>(rawPeakBuf.size());
            for (int i = 0; i < peakTotal; ++i) {
                if ((i & 0x1FFFF) == 0 && peakTotal > 0)
                    m_trackData->reportAnalysisProgress(
                        0.58 + (static_cast<double>(i) / static_cast<double>(peakTotal)) * 0.02, true);
                const auto& raw = rawPeakBuf[static_cast<size_t>(i)];
                peakMip[i].minSample = static_cast<qint8>(
                    std::clamp(static_cast<int>(raw.minRaw * normScale), -127, 127));
                peakMip[i].maxSample = static_cast<qint8>(
                    std::clamp(static_cast<int>(raw.maxRaw * normScale), -127, 127));
            }
            m_trackData->setPeakMipData(std::move(peakMip));
        }
    }

    m_trackData->reportAnalysisProgress(0.60, true);


    return !thread.threadShouldExit();
}

} // namespace waveform_internal

QVector<TrackData::RgbWaveformFrame> WaveformAnalyzer::buildInstantOverview(
    juce::AudioFormatReader* reader, int maxBins)
{
    return waveform_internal::buildInstantOverview(reader, maxBins);
}
