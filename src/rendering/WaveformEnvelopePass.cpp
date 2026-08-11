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

    // Keep this preview strictly O(maxBins) in decoder operations. The old
    // stride loop performed roughly 128 separate reads for every overview bin
    // (about 65k seeks for a 512-bin long track), despite being intended as the
    // cheap post-load preview. One short, centred window per bin is enough
    // until the progressive/full analysis replaces it.
    constexpr int kSamplesPerBin = 128;
    juce::AudioBuffer<float> buf(numCh, kSamplesPerBin);

    for (int bin = 0; bin < maxBins; ++bin) {
        const juce::int64 binStart = (static_cast<juce::int64>(bin) * totalSamples) / maxBins;
        const juce::int64 binEnd   = (static_cast<juce::int64>(bin + 1) * totalSamples) / maxBins;
        const juce::int64 span     = std::max<juce::int64>(1, binEnd - binStart);
        float peak = 0.0f;
        float rmsAcc = 0.0f;
        float lowAcc = 0.0f;
        float highAcc = 0.0f;
        int sampleCount = 0;
        float prevMono = 0.0f;

        const int toRead = static_cast<int>(
            std::min<juce::int64>(span, kSamplesPerBin));
        const juce::int64 readPosition = binStart
            + std::max<juce::int64>(0, (span - toRead) / 2);
        buf.clear();
        reader->read(&buf, 0, toRead, readPosition, true, true);

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
    const juce::int64 totalSamples = input.totalSamples;
    const double sampleRate = input.sampleRate;
    const int numPoints = input.numPoints;

    auto threadShouldExit = [&]() { return thread.threadShouldExit(); };
    const auto cooperateWithRealtime = [&]() {
        // Analysis owns a separate decoder, but can still compete for CPU and
        // storage bandwidth exactly when a cold scratch window needs both.
        // Cooperate with the scheduler without a timer-based sleep: sleeps made
        // seek latency depend on how many batches happened to precede P0 work.
        if (!threadShouldExit() && input.realtimeInteractionActive
            && input.realtimeInteractionActive())
            juce::Thread::yield();
    };

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

    // One robust normalization profile is fixed before the first detail chunk
    // is published. Every priority, seek and sequential job uses the same
    // values, so a completed immutable range never changes brightness later.

    struct RawBin {
        float low    = 0.0f;   // raw envelope value, NOT normalized
        float lowMid = 0.0f;
        float mid    = 0.0f;
        float high   = 0.0f;
    };
    // Peak mipmap: signed min/max per high-res bin (4× analysis rate).
    // Allows the renderer to show actual audio oscillations at high zoom.
    struct RawPeak { float minRaw = 0.0f; float maxRaw = 0.0f; };
    const int requestedPeakRatio = TrackData::PEAK_POINTS_PER_SECOND / m_pointsPerSecond;
    constexpr double kHighResolutionPeakMaxDurationSec = 10.0 * 60.0;
    const double trackDurationSec = static_cast<double>(totalSamples) / sampleRate;
    const int peakRatio = trackDurationSec <= kHighResolutionPeakMaxDurationSec
        ? std::clamp(requestedPeakRatio, 1, 8) : 0;
    const int numPeakPoints = numPoints * peakRatio;
    std::vector<RawPeak> rawPeakBuf;
    float globalMaxSample = 0.001f;

    float globalMaxPeak   = 0.001f;

    struct NormalizationProfile final {
        float low = 0.1f;
        float lowMid = 0.1f;
        float mid = 0.1f;
        float high = 0.1f;
    };
    const auto overview = m_trackData->getOverviewRgbData();
    const auto robustBand = [&overview](auto member) {
        std::vector<float> values;
        values.reserve(static_cast<std::size_t>(overview.size()));
        for (const auto& frame : overview) {
            const float value = std::max(0.0f, frame.*member);
            if (std::isfinite(value))
                values.push_back(value);
        }
        if (values.empty())
            return 0.1f;
        const auto percentileIndex = std::min(
            values.size() - 1,
            static_cast<std::size_t>(std::floor(
                static_cast<double>(values.size() - 1) * 0.98)));
        std::nth_element(values.begin(), values.begin() + percentileIndex,
                         values.end());
        return std::max(0.08f, values[percentileIndex]);
    };
    const NormalizationProfile normalization{
        robustBand(&TrackData::RgbWaveformFrame::low),
        robustBand(&TrackData::RgbWaveformFrame::lowMid),
        robustBand(&TrackData::RgbWaveformFrame::mid),
        robustBand(&TrackData::RgbWaveformFrame::high)};

    // Progressive publication uses exactly the immutable store chunk size.
    // Smaller 128-bin messages used to expose a Loading source chunk eight
    // times before it became renderable, delaying first detail even though
    // analysis had already reached the playhead.
    constexpr int kChunk = static_cast<int>(WaveformLineStore::kChunkSize);
    const auto seekHintBin = [&input, m_pointsPerSecond, numPoints]() {
        const double hint = input.currentSeekHintSec
            ? input.currentSeekHintSec() : input.seekHintSec;
        return std::clamp(static_cast<int>(std::max(0.0, hint) * m_pointsPerSecond),
                          0, numPoints - 1);
    };
    QVector<TrackData::WaveformBin> previewBatch;
    previewBatch.reserve(kChunk);
    QVector<TrackData::RgbWaveformFrame> previewRgbBatch;
    previewRgbBatch.reserve(kChunk);
    const auto publishChunk = [&input](int firstBin,
                                       const QVector<TrackData::WaveformBin>& waveform,
                                       const QVector<TrackData::RgbWaveformFrame>& rgb,
                                       WaveformNormalizationState state
                                           = WaveformNormalizationState::Final) {
        if (!input.retainLegacyWaveform && !rgb.isEmpty())
            input.trackData->writePreparedWaveformRange(firstBin, rgb);
        if (input.publishChunk && (!waveform.isEmpty() || !rgb.isEmpty()))
            input.publishChunk(firstBin, input.numPoints, waveform, rgb, state);
    };

    // Shared shaping helper — used identically in preview AND final pass.
    auto shapeBin = [](float norm, float expo, float gain) -> float {
        return std::min(1.0f, std::pow(std::clamp(norm, 0.0f, 1.0f), expo) * gain);
    };

    juce::AudioBuffer<float> readBuf(static_cast<int>(reader.numChannels), static_cast<int>(maxSamplesPerBin));

    // ─── Interactive chunks: playhead first, then deterministic expansion ──
    const int warmupBins = std::max(24, m_pointsPerSecond / 20); // 50 ms
    const auto demandSnapshot = [&input]() {
        return input.currentDemand
            ? input.currentDemand() : waveform::WaveformDemand{};
    };
    const int sourceChunkCount = (numPoints + kChunk - 1) / kChunk;
    std::vector<bool> priorityAnalyzed(
        static_cast<std::size_t>(sourceChunkCount), false);

    const auto processPriorityBin = [&](int bin, FiltState& filter,
                                        juce::AudioBuffer<float>& buffer) {
        const juce::int64 binStart =
            (static_cast<juce::int64>(bin) * totalSamples) / numPoints;
        juce::int64 binEnd =
            (static_cast<juce::int64>(bin + 1) * totalSamples) / numPoints;
        if (binEnd <= binStart)
            binEnd = std::min(totalSamples, binStart + 1);
        const int toRead = static_cast<int>(
            std::max<juce::int64>(1, binEnd - binStart));
        reader.read(&buffer, 0, toRead, binStart, true, false);

        for (int sample = 0; sample < toRead; ++sample) {
            float low = 0.0f, lowMid = 0.0f, mid = 0.0f, high = 0.0f;
            for (int channel = 0; channel < numCh; ++channel) {
                const auto index = static_cast<std::size_t>(channel);
                const float in = buffer.getReadPointer(channel)[sample];
                filter.lp110[index] = aLP110 * in
                    + (1.0f - aLP110) * filter.lp110[index];
                const float bandLow = std::abs(filter.lp110[index]);
                filter.hp150s1[index] = aHP150 * in
                    + (1.0f - aHP150) * filter.hp150s1[index];
                const float hp1 = in - filter.hp150s1[index];
                filter.hp150s2[index] = aHP150 * hp1
                    + (1.0f - aHP150) * filter.hp150s2[index];
                filter.lp160[index] = aLP160 * (hp1 - filter.hp150s2[index])
                    + (1.0f - aLP160) * filter.lp160[index];
                const float bandLowMid = std::abs(filter.lp160[index]);
                filter.hp180s1[index] = aHP180 * in
                    + (1.0f - aHP180) * filter.hp180s1[index];
                const float hp3 = in - filter.hp180s1[index];
                filter.hp180s2[index] = aHP180 * hp3
                    + (1.0f - aHP180) * filter.hp180s2[index];
                filter.lp800[index] = aLP800 * (hp3 - filter.hp180s2[index])
                    + (1.0f - aLP800) * filter.lp800[index];
                const float bandMid = std::abs(filter.lp800[index]);
                const float v3 = in - filter.svfIc2[index];
                const float v1 = svfD * (filter.svfIc1[index] + svfG * v3);
                const float v2 = filter.svfIc2[index] + svfG * v1;
                filter.svfIc1[index] = 2.0f * v1 - filter.svfIc1[index];
                filter.svfIc2[index] = 2.0f * v2 - filter.svfIc2[index];
                filter.hp19k[index] = aHP19k * in
                    + (1.0f - aHP19k) * filter.hp19k[index];
                const float bandHigh = std::abs(v1)
                    + std::abs(in - filter.hp19k[index]);
                low = std::max(low, bandLow);
                lowMid = std::max(lowMid, bandLowMid);
                mid = std::max(mid, bandMid);
                high = std::max(high, bandHigh);
            }
            filter.envLow.process(low);
            filter.envLowMid.process(lowMid);
            filter.envMid.process(mid);
            filter.envHigh.process(high);
        }
        return RawBin{filter.envLow.state, filter.envLowMid.state,
                      filter.envMid.state, filter.envHigh.state};
    };

    const auto analyzePriorityChunk = [&](int chunkIndex) {
        if (chunkIndex < 0 || chunkIndex >= sourceChunkCount
            || priorityAnalyzed[static_cast<std::size_t>(chunkIndex)]
            || threadShouldExit()) {
            return;
        }
        const int begin = chunkIndex * kChunk;
        const int end = std::min(numPoints, begin + kChunk);
        FiltState filter;
        filter.reset(numCh, sampleRate);
        juce::AudioBuffer<float> buffer(
            static_cast<int>(reader.numChannels),
            static_cast<int>(maxSamplesPerBin));
        for (int bin = std::max(0, begin - warmupBins);
             bin < begin && !threadShouldExit(); ++bin) {
            if ((bin & 0x1F) == 0)
                cooperateWithRealtime();
            (void)processPriorityBin(bin, filter, buffer);
        }

        QVector<TrackData::RgbWaveformFrame> frames;
        frames.reserve(end - begin);
        for (int bin = begin; bin < end && !threadShouldExit(); ++bin) {
            if ((bin & 0x1F) == 0)
                cooperateWithRealtime();
            const auto raw = processPriorityBin(bin, filter, buffer);
            TrackData::RgbWaveformFrame rgb;
            rgb.low = shapeBin(raw.low / normalization.low, 1.8f, 1.0f);
            rgb.lowMid = shapeBin(
                raw.lowMid / normalization.lowMid, 1.6f, 0.9f);
            rgb.mid = shapeBin(raw.mid / normalization.mid, 1.5f, 0.7f);
            rgb.high = shapeBin(
                raw.high / normalization.high, 1.3f, 0.5f);
            rgb.rms = std::clamp(
                0.5f * std::max({rgb.low, rgb.lowMid, rgb.mid, rgb.high})
                    + 0.5f * ((rgb.low + rgb.lowMid + rgb.mid + rgb.high)
                              / 4.0f),
                0.0f, 1.0f);
            rgb.color = QColor(255, 255, 255, 230);
            frames.push_back(rgb);
        }
        if (frames.size() != end - begin)
            return;
        if (input.retainLegacyWaveform)
            m_trackData->writeRgbWaveformRange(begin, frames);
        publishChunk(begin, {}, frames);
        priorityAnalyzed[static_cast<std::size_t>(chunkIndex)] = true;
    };

    const auto analyzeDemand = [&](int centreBin,
                                   const waveform::WaveformDemand& demand) {
        const int current = std::clamp(centreBin / kChunk,
                                       0, sourceChunkCount - 1);
        analyzePriorityChunk(current);
        constexpr int kPreferredLookaheadChunks = 3;
        constexpr int kOppositeGuardChunks = 2;
        const bool scratching = demand.scratching
            || (input.realtimeInteractionActive
                && input.realtimeInteractionActive());
        if (scratching) {
            for (int distance = 1; distance <= kPreferredLookaheadChunks;
                 ++distance) {
                analyzePriorityChunk(current - distance);
                analyzePriorityChunk(current + distance);
            }
            return;
        }
        const int direction = demand.reverse ? -1 : 1;
        for (int distance = 1; distance <= kPreferredLookaheadChunks;
             ++distance) {
            analyzePriorityChunk(current + direction * distance);
        }
        for (int distance = 1; distance <= kOppositeGuardChunks; ++distance)
            analyzePriorityChunk(current - direction * distance);
    };

    const int hintBin = seekHintBin();
    analyzeDemand(hintBin, demandSnapshot());
    if (threadShouldExit())
        return false;
    if (input.acquireBackgroundSlot && !input.acquireBackgroundSlot())
        return false;
    // ─────────────────────────────────────────────────────────────────────

    // Publish the playhead window first. Short tracks keep the legacy vectors
    // for backward-compatible cache payloads. Long tracks build only canonical
    // immutable chunks and never allocate duration-sized legacy RGB arrays.
    if (numPeakPoints > 0)
        rawPeakBuf.resize(static_cast<size_t>(numPeakPoints));
    if (input.retainLegacyWaveform)
        m_trackData->preallocateRgbWaveform(numPoints);

    int mainChunkStart = 0;

    int lastDemandChunk = hintBin / kChunk;

    for (int bin = 0; bin < numPoints; ++bin)
    {
        if (threadShouldExit()) break;

        if ((bin & 0x1F) == 0)
            cooperateWithRealtime();

        // Yield occasionally without sleeping — keeps UI/audio responsive without
        // throttling analysis to "grandma speed".
        if ((bin & 0xFFF) == 0)
            juce::Thread::yield();

        if ((bin & 0x7F) == 0)
            m_trackData->reportAnalysisProgress(
                (static_cast<double>(bin) / static_cast<double>(numPoints)) * 0.50, true);

        // A changed source chunk means playback, seek or scratch crossed a
        // meaningful demand boundary.  The new cursor chunk is analyzed
        // synchronously before background sequential work resumes; no timer or
        // FIFO of old background requests can sit in front of it.
        if (bin > 0 && bin % 128 == 0) {
            const int latestHint = seekHintBin();
            const int latestDemandChunk = latestHint / kChunk;
            if (latestDemandChunk != lastDemandChunk) {
                analyzeDemand(latestHint, demandSnapshot());
                lastDemandChunk = latestDemandChunk;
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
        const int peakRatioClamped = peakRatio;

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

            if (peakRatioClamped > 0) {
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
            }

            mainFilt.envLow   .process(bestLow);
            mainFilt.envLowMid.process(bestLowMid);
            mainFilt.envMid   .process(bestMid);
            mainFilt.envHigh  .process(bestHigh);
        }

        if (peakRatioClamped > 0) {
            const int peakBinBase = bin * peakRatioClamped;
            for (int pb = 0; pb < peakRatioClamped; ++pb) {
                const auto& sp = subPeaks[static_cast<size_t>(pb)];
                const float pMin = sp.init ? sp.min : 0.0f;
                const float pMax = sp.init ? sp.max : 0.0f;
                rawPeakBuf[static_cast<size_t>(peakBinBase + pb)] = { pMin, pMax };
                if (pMax > globalMaxSample) globalMaxSample = pMax;
                if (-pMin > globalMaxSample) globalMaxSample = -pMin;
            }
        }

        RawBin rb;
        rb.low    = mainFilt.envLow   .state;
        rb.lowMid = mainFilt.envLowMid.state;
        rb.mid    = mainFilt.envMid   .state;
        rb.high   = mainFilt.envHigh  .state;
        const float binMax = std::max({rb.low, rb.lowMid, rb.mid, rb.high});
        if (binMax > globalMaxPeak) globalMaxPeak = binMax;

        TrackData::WaveformBin pbin;
        pbin.low = shapeBin(rb.low / normalization.low, 1.8f, 1.0f);
        pbin.lowMid = shapeBin(
            rb.lowMid / normalization.lowMid, 1.6f, 0.9f);
        pbin.mid = shapeBin(rb.mid / normalization.mid, 1.5f, 0.7f);
        pbin.high = shapeBin(rb.high / normalization.high, 1.3f, 0.5f);

        if (input.retainLegacyWaveform)
            previewBatch.append(pbin);

        TrackData::RgbWaveformFrame rgb;
        const float lowN = std::clamp(pbin.low, 0.0f, 1.0f);
        const float lowMidN = std::clamp(pbin.lowMid, 0.0f, 1.0f);
        const float midN = std::clamp(pbin.mid, 0.0f, 1.0f);
        const float highN = std::clamp(pbin.high, 0.0f, 1.0f);
        const float rmsN = std::clamp(
            0.5f * std::max({lowN, lowMidN, midN, highN})
                + 0.5f * ((lowN + lowMidN + midN + highN) / 4.0f),
            0.0f, 1.0f);
        rgb.color = QColor(255, 255, 255, 230);
        rgb.rms = rmsN;
        rgb.low = lowN;
        rgb.lowMid = lowMidN;
        rgb.mid = midN;
        rgb.high = highN;
        if (previewRgbBatch.isEmpty())
            mainChunkStart = bin;
        previewRgbBatch.append(rgb);

        if (previewRgbBatch.size() >= kChunk) {
            const int firstBin = mainChunkStart;
            if (input.retainLegacyWaveform) {
                m_trackData->appendData(previewBatch);
                m_trackData->writeRgbWaveformRange(mainChunkStart, previewRgbBatch);
            }
            publishChunk(firstBin, previewBatch, previewRgbBatch);
            previewRgbBatch.clear();
            previewBatch.clear();
        }
    }
    if (!previewRgbBatch.isEmpty()) {
        const int firstBin = mainChunkStart;
        if (input.retainLegacyWaveform) {
            m_trackData->appendData(previewBatch);
            m_trackData->writeRgbWaveformRange(mainChunkStart, previewRgbBatch);
        }
        publishChunk(firstBin, previewBatch, previewRgbBatch);
    }

    if (threadShouldExit()) return false;

    m_trackData->reportAnalysisProgress(0.52, true);

    if (!threadShouldExit()) {
        m_trackData->setGlobalMaxPeak(globalMaxPeak);
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
