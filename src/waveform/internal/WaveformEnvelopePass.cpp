#include "WaveformEnvelopePass.h"
#include "waveform/WaveformAnalyzer.h"
#include <QColor>
#include <juce_dsp/juce_dsp.h>
#include <QDebug>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

namespace {

// Raw (un-normalized) band envelope state at the end of one waveform bin.
struct RawBin {
    float low    = 0.0f;
    float lowMid = 0.0f;
    float mid    = 0.0f;
    float high   = 0.0f;
};

// Peak mipmap entry: signed min/max of the mono downmix over a sub-bin.
struct RawPeak { float minRaw = 0.0f; float maxRaw = 0.0f; };

// Fixed filter coefficients for the 4-band bank described below. They depend
// only on the sample rate, so every segment worker shares one instance while
// keeping its own FiltState.
struct BandCoefficients {
    float lp110 = 0.0f;
    float hp150 = 0.0f;
    float lp160 = 0.0f;
    float hp180 = 0.0f;
    float lp800 = 0.0f;
    float hp19k = 0.0f;
    float svfG  = 0.0f;
    float svfD  = 0.0f;

    static BandCoefficients forSampleRate(double sampleRate)
    {
        const float sr = static_cast<float>(sampleRate);
        // 1-pole LP coefficient: a = 2*pi*fc / (2*pi*fc + sr).
        const auto lpCoef1 = [sr](float fc) {
            const float w = 2.0f * juce::MathConstants<float>::pi * fc / sr;
            return w / (w + 1.0f);
        };
        BandCoefficients c;
        c.lp110 = lpCoef1(110.0f);
        c.hp150 = lpCoef1(150.0f);
        c.lp160 = lpCoef1(160.0f);
        c.hp180 = lpCoef1(180.0f);
        c.lp800 = lpCoef1(800.0f);
        c.hp19k = lpCoef1(19000.0f);
        // SVF (state-variable, TPT) for the resonant 2750 Hz band, Q = 2.
        c.svfG = std::tan(juce::MathConstants<float>::pi * 2750.0f / sr);
        const float svfR = 1.0f / (2.0f * 2.0f);
        c.svfD = 1.0f / (1.0f + 2.0f * svfR * c.svfG + c.svfG * c.svfG);
        return c;
    }
};

// Sequential block window over a decoder.
//
// The pass used to issue one AudioFormatReader::read() per waveform bin. At
// 1200 bins/s that is ~1200 decoder entries per second of audio — over half a
// million for a seven-minute track — each re-entering the reader's seek and
// reservoir bookkeeping to hand back roughly forty samples. One 64k-sample
// block now serves about 1600 bins.
class DecodeWindow final
{
public:
    static constexpr int kCapacitySamples = 1 << 16;

    DecodeWindow(juce::AudioFormatReader& reader, juce::int64 totalSamples)
        : m_reader(reader)
        , m_totalSamples(totalSamples)
        , m_buffer(std::max(1, static_cast<int>(reader.numChannels)),
                   kCapacitySamples)
    {
    }

    // Makes [start, start + length) resident and returns its offset inside the
    // window, or -1 when the range cannot be served.
    int acquire(juce::int64 start, int length)
    {
        if (start < 0 || length <= 0 || length > kCapacitySamples)
            return -1;
        if (start < m_start || start + length > m_start + m_length) {
            const int toRead = static_cast<int>(std::min<juce::int64>(
                kCapacitySamples,
                std::max<juce::int64>(length, m_totalSamples - start)));
            // Both channels are decoded on purpose: the per-channel maximum
            // taken below is only meaningful when the channels differ. Reading
            // with useReaderRightChan = false makes JUCE duplicate the left
            // channel, which hid right-panned material from the waveform and
            // made the filter bank run twice over identical samples.
            if (!m_reader.read(&m_buffer, 0, toRead, start, true, true))
                return -1;
            m_start = start;
            m_length = toRead;
        }
        return static_cast<int>(start - m_start);
    }

    const float* channel(int index) const
    { return m_buffer.getReadPointer(index); }

private:
    juce::AudioFormatReader& m_reader;
    juce::int64 m_totalSamples = 0;
    juce::AudioBuffer<float> m_buffer;
    juce::int64 m_start = 0;
    int m_length = 0;
};

// Advances the filter bank across one bin and returns its envelope state.
// `peaks`, when non-null, receives `peakRatio` signed min/max sub-bins for the
// peak mipmap. Returns false when the bin's samples could not be decoded.
bool processBin(const BandCoefficients& c,
                FiltState& filt,
                DecodeWindow& window,
                int numCh,
                juce::int64 binStart,
                int numSamples,
                RawBin& out,
                RawPeak* peaks,
                int peakRatio)
{
    const int offset = window.acquire(binStart, numSamples);
    if (offset < 0)
        return false;

    std::array<const float*, 8> channels{};
    const int channelCount = std::min<int>(numCh, static_cast<int>(channels.size()));
    for (int ch = 0; ch < channelCount; ++ch)
        channels[static_cast<std::size_t>(ch)] = window.channel(ch) + offset;

    if (peaks != nullptr) {
        for (int pb = 0; pb < peakRatio; ++pb)
            peaks[pb] = RawPeak{};
    }
    std::array<bool, 8> peakSeen{};

    for (int s = 0; s < numSamples; ++s) {
        float bestLow = 0.0f, bestLowMid = 0.0f, bestMid = 0.0f, bestHigh = 0.0f;
        float monoAcc = 0.0f;

        for (int ch = 0; ch < channelCount; ++ch) {
            const auto ci = static_cast<std::size_t>(ch);
            const float in = channels[ci][s];
            monoAcc += in;

            // Band 1: LP @ 110 Hz (1st order).
            filt.lp110[ci] = c.lp110 * in + (1.0f - c.lp110) * filt.lp110[ci];
            const float b1 = std::abs(filt.lp110[ci]);

            // Band 2: HP @ 150 Hz (2nd order) -> LP @ 160 Hz.
            filt.hp150s1[ci] = c.hp150 * in + (1.0f - c.hp150) * filt.hp150s1[ci];
            const float hp1out = in - filt.hp150s1[ci];
            filt.hp150s2[ci] = c.hp150 * hp1out + (1.0f - c.hp150) * filt.hp150s2[ci];
            const float hp2out = hp1out - filt.hp150s2[ci];
            filt.lp160[ci] = c.lp160 * hp2out + (1.0f - c.lp160) * filt.lp160[ci];
            const float b2 = std::abs(filt.lp160[ci]);

            // Band 3: HP @ 180 Hz (2nd order) -> LP @ 800 Hz.
            filt.hp180s1[ci] = c.hp180 * in + (1.0f - c.hp180) * filt.hp180s1[ci];
            const float hp3out = in - filt.hp180s1[ci];
            filt.hp180s2[ci] = c.hp180 * hp3out + (1.0f - c.hp180) * filt.hp180s2[ci];
            const float hp4out = hp3out - filt.hp180s2[ci];
            filt.lp800[ci] = c.lp800 * hp4out + (1.0f - c.lp800) * filt.lp800[ci];
            const float b3 = std::abs(filt.lp800[ci]);

            // Band 4: resonant BP @ 2750 Hz + HP @ 19 kHz.
            const float v3 = in - filt.svfIc2[ci];
            const float v1 = c.svfD * (filt.svfIc1[ci] + c.svfG * v3);
            const float v2 = filt.svfIc2[ci] + c.svfG * v1;
            filt.svfIc1[ci] = 2.0f * v1 - filt.svfIc1[ci];
            filt.svfIc2[ci] = 2.0f * v2 - filt.svfIc2[ci];
            filt.hp19k[ci] = c.hp19k * in + (1.0f - c.hp19k) * filt.hp19k[ci];
            const float b4 = std::abs(v1) + std::abs(in - filt.hp19k[ci]);

            if (b1 > bestLow)    bestLow    = b1;
            if (b2 > bestLowMid) bestLowMid = b2;
            if (b3 > bestMid)    bestMid    = b3;
            if (b4 > bestHigh)   bestHigh   = b4;
        }

        if (peaks != nullptr && peakRatio > 0) {
            const float mono = monoAcc / static_cast<float>(std::max(1, channelCount));
            const auto pb = static_cast<std::size_t>(std::min(
                peakRatio - 1, (s * peakRatio) / std::max(1, numSamples)));
            if (!peakSeen[pb]) {
                peaks[pb].minRaw = mono;
                peaks[pb].maxRaw = mono;
                peakSeen[pb] = true;
            } else {
                if (mono < peaks[pb].minRaw) peaks[pb].minRaw = mono;
                if (mono > peaks[pb].maxRaw) peaks[pb].maxRaw = mono;
            }
        }

        filt.envLow   .process(bestLow);
        filt.envLowMid.process(bestLowMid);
        filt.envMid   .process(bestMid);
        filt.envHigh  .process(bestHigh);
    }

    out.low    = filt.envLow.state;
    out.lowMid = filt.envLowMid.state;
    out.mid    = filt.envMid.state;
    out.high   = filt.envHigh.state;
    return true;
}

// Segments of the full-track pass decode and filter in parallel. The DSP chain
// is stateful, so each worker replays a short warm-up prefix before its own
// range; that costs a fraction of a percent of the bins and leaves no visible
// seam. The budget deliberately keeps cores free for audio, Qt and the Vulkan
// render thread, and a worker only pays for itself once it owns several store
// chunks.
int envelopeWorkerCount(int sourceChunkCount)
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 4 || sourceChunkCount <= 2)
        return 1;
    const int budget = static_cast<int>(std::min<unsigned>(4u, hw / 2u - 1u));
    return std::clamp(std::min(budget, sourceChunkCount / 2), 1, 4);
}

void lowerCurrentThreadPriority()
{
#ifdef __linux__
    (void)setpriority(PRIO_PROCESS,
                      static_cast<id_t>(syscall(SYS_gettid)), 12);
#endif
}

} // namespace

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
    const BandCoefficients coefficients =
        BandCoefficients::forSampleRate(sampleRate);

    // One robust normalization profile is fixed before the first detail chunk
    // is published. Every priority, seek and sequential job uses the same
    // values, so a completed immutable range never changes brightness later.

    // Peak mipmap: signed min/max per high-res bin (4× analysis rate).
    // Allows the renderer to show actual audio oscillations at high zoom.
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

    // Single conversion from raw band envelopes to a display frame. The
    // priority prologue and the full pass must agree exactly, otherwise a
    // region changes brightness when the pass overwrites a priority chunk.
    const auto makeRgbFrame = [&](const RawBin& raw) {
        TrackData::RgbWaveformFrame rgb;
        rgb.low = shapeBin(raw.low / normalization.low, 1.8f, 1.0f);
        rgb.lowMid = shapeBin(raw.lowMid / normalization.lowMid, 1.6f, 0.9f);
        rgb.mid = shapeBin(raw.mid / normalization.mid, 1.5f, 0.7f);
        rgb.high = shapeBin(raw.high / normalization.high, 1.3f, 0.5f);
        rgb.rms = std::clamp(
            0.5f * std::max({rgb.low, rgb.lowMid, rgb.mid, rgb.high})
                + 0.5f * ((rgb.low + rgb.lowMid + rgb.mid + rgb.high) / 4.0f),
            0.0f, 1.0f);
        rgb.color = QColor(255, 255, 255, 230);
        return rgb;
    };

    // Sample range covered by one waveform bin. Exact integer-ratio
    // partitioning avoids cumulative timeline drift on long tracks.
    const auto binSampleRange = [&](int bin, juce::int64& start, int& length) {
        start = (static_cast<juce::int64>(bin) * totalSamples) / numPoints;
        juce::int64 end =
            (static_cast<juce::int64>(bin + 1) * totalSamples) / numPoints;
        if (end <= start)
            end = std::min(totalSamples, start + 1);
        length = static_cast<int>(std::max<juce::int64>(1, end - start));
    };

    // ─── Interactive chunks: playhead first, then deterministic expansion ──
    const int warmupBins = std::max(24, m_pointsPerSecond / 20); // 50 ms
    const auto demandSnapshot = [&input]() {
        return input.currentDemand
            ? input.currentDemand() : waveform::WaveformDemand{};
    };
    const int sourceChunkCount = (numPoints + kChunk - 1) / kChunk;
    std::vector<bool> priorityAnalyzed(
        static_cast<std::size_t>(sourceChunkCount), false);

    // The prologue is strictly single-threaded and finishes before the parallel
    // pass starts, so it may keep one decode window on the caller's reader.
    DecodeWindow priorityWindow(reader, totalSamples);
    const auto processPriorityBin = [&](int bin, FiltState& filter,
                                        RawBin& raw) {
        juce::int64 binStart = 0;
        int length = 0;
        binSampleRange(bin, binStart, length);
        return processBin(coefficients, filter, priorityWindow, numCh,
                          binStart, length, raw, nullptr, 0);
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
        RawBin raw;
        for (int bin = std::max(0, begin - warmupBins);
             bin < begin && !threadShouldExit(); ++bin) {
            if ((bin & 0x1F) == 0)
                cooperateWithRealtime();
            if (!processPriorityBin(bin, filter, raw))
                return;
        }

        QVector<TrackData::RgbWaveformFrame> frames;
        frames.reserve(end - begin);
        for (int bin = begin; bin < end && !threadShouldExit(); ++bin) {
            if ((bin & 0x1F) == 0)
                cooperateWithRealtime();
            if (!processPriorityBin(bin, filter, raw))
                return;
            frames.push_back(makeRgbFrame(raw));
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

    // Publish the playhead window first. Short tracks keep the legacy vectors
    // for backward-compatible cache payloads. Long tracks build only canonical
    // immutable chunks and never allocate duration-sized legacy RGB arrays.
    // The legacy vectors are sized before the prologue runs: priority chunks
    // used to be dropped because the target vector was still empty.
    if (numPeakPoints > 0)
        rawPeakBuf.resize(static_cast<size_t>(numPeakPoints));
    if (input.retainLegacyWaveform) {
        m_trackData->preallocateRgbWaveform(numPoints);
        m_trackData->preallocateWaveform(numPoints);
    }

    const int hintBin = seekHintBin();
    analyzeDemand(hintBin, demandSnapshot());
    if (threadShouldExit())
        return false;
    if (input.acquireBackgroundSlot && !input.acquireBackgroundSlot())
        return false;
    // ─────────────────────────────────────────────────────────────────────

    // ─── Full-track pass, split into independently decoded segments ───────
    //
    // Each segment owns a whole number of store chunks, so its published
    // batches line up with the immutable store exactly as the old sequential
    // pass did. Before touching its own range a segment replays a warm-up
    // prefix, long enough for the filter bank and the envelope followers to
    // settle (the slowest release constant is 35 ms), which is what keeps the
    // segment boundaries invisible.
    //
    // The sequential pass used to re-check the seek hint every 128 bins and
    // interrupt itself with a priority chunk. That existed because a full pass
    // took long enough for the playhead to outrun it; with the whole track
    // finishing in a fraction of that time the playhead prologue above is
    // enough, and dropping it keeps the segments independent.
    struct Segment {
        int binBegin = 0;
        int binEnd = 0;
        float maxPeak = 0.001f;
        float maxSample = 0.001f;
    };

    std::vector<std::unique_ptr<juce::AudioFormatReader>> segmentReaders;
    if (input.createReader) {
        const int desiredWorkers = envelopeWorkerCount(sourceChunkCount);
        for (int i = 1; i < desiredWorkers; ++i) {
            auto extra = input.createReader();
            if (!extra)
                break;
            segmentReaders.push_back(std::move(extra));
        }
    }

    const int workerCount = 1 + static_cast<int>(segmentReaders.size());
    std::vector<Segment> segments;
    segments.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) {
        const int chunkBegin = static_cast<int>(
            (static_cast<juce::int64>(i) * sourceChunkCount) / workerCount);
        const int chunkEnd = static_cast<int>(
            (static_cast<juce::int64>(i + 1) * sourceChunkCount) / workerCount);
        if (chunkBegin >= chunkEnd)
            continue;
        segments.push_back(Segment{chunkBegin * kChunk,
                                   std::min(numPoints, chunkEnd * kChunk),
                                   0.001f, 0.001f});
    }

    const int segmentWarmupBins = std::max(warmupBins, m_pointsPerSecond / 2);
    std::mutex publishMutex;
    std::atomic<int> completedBins{0};
    std::atomic<bool> segmentFailed{false};

    const auto runSegment = [&](Segment& segment,
                                juce::AudioFormatReader& segmentReader) {
        FiltState filter;
        filter.reset(numCh, sampleRate);
        DecodeWindow window(segmentReader, totalSamples);
        RawBin raw;
        std::array<RawPeak, 8> peaks{};
        juce::int64 binStart = 0;
        int length = 0;

        for (int bin = std::max(0, segment.binBegin - segmentWarmupBins);
             bin < segment.binBegin; ++bin) {
            if (threadShouldExit())
                return;
            binSampleRange(bin, binStart, length);
            if (!processBin(coefficients, filter, window, numCh, binStart,
                            length, raw, nullptr, 0)) {
                segmentFailed.store(true, std::memory_order_relaxed);
                return;
            }
        }

        QVector<TrackData::WaveformBin> binBatch;
        QVector<TrackData::RgbWaveformFrame> rgbBatch;
        binBatch.reserve(kChunk);
        rgbBatch.reserve(kChunk);
        int batchStart = segment.binBegin;

        const auto flush = [&]() {
            if (rgbBatch.isEmpty())
                return;
            const std::lock_guard<std::mutex> lock(publishMutex);
            if (input.retainLegacyWaveform) {
                m_trackData->writeWaveformRange(batchStart, binBatch);
                m_trackData->writeRgbWaveformRange(batchStart, rgbBatch);
            }
            publishChunk(batchStart, binBatch, rgbBatch);
            m_trackData->reportAnalysisProgress(
                (static_cast<double>(
                     completedBins.load(std::memory_order_relaxed))
                 / static_cast<double>(numPoints)) * 0.50, true);
            binBatch.clear();
            rgbBatch.clear();
        };

        for (int bin = segment.binBegin; bin < segment.binEnd; ++bin) {
            if (threadShouldExit())
                return;

            if ((bin & 0x1F) == 0)
                cooperateWithRealtime();

            // Yield occasionally without sleeping — keeps UI/audio responsive
            // without throttling analysis to "grandma speed".
            if ((bin & 0xFFF) == 0)
                juce::Thread::yield();

            binSampleRange(bin, binStart, length);
            if (!processBin(coefficients, filter, window, numCh, binStart,
                            length, raw,
                            peakRatio > 0 ? peaks.data() : nullptr,
                            peakRatio)) {
                segmentFailed.store(true, std::memory_order_relaxed);
                return;
            }

            if (peakRatio > 0) {
                const int peakBinBase = bin * peakRatio;
                for (int pb = 0; pb < peakRatio; ++pb) {
                    const auto& peak = peaks[static_cast<std::size_t>(pb)];
                    rawPeakBuf[static_cast<std::size_t>(peakBinBase + pb)] = peak;
                    segment.maxSample = std::max(
                        {segment.maxSample, peak.maxRaw, -peak.minRaw});
                }
            }
            segment.maxPeak = std::max(
                segment.maxPeak,
                std::max({raw.low, raw.lowMid, raw.mid, raw.high}));

            if (rgbBatch.isEmpty())
                batchStart = bin;
            const auto frame = makeRgbFrame(raw);
            rgbBatch.append(frame);
            if (input.retainLegacyWaveform) {
                binBatch.append(TrackData::WaveformBin{
                    frame.low, frame.lowMid, frame.mid, frame.high});
            }
            completedBins.fetch_add(1, std::memory_order_relaxed);

            if (rgbBatch.size() >= kChunk)
                flush();
        }
        flush();
    };

    {
        std::vector<std::thread> workers;
        workers.reserve(segments.empty() ? 0 : segments.size() - 1);
        for (std::size_t i = 1; i < segments.size(); ++i) {
            workers.emplace_back([&, i]() {
                lowerCurrentThreadPriority();
                try {
                    runSegment(segments[i], *segmentReaders[i - 1]);
                } catch (const std::exception& e) {
                    segmentFailed.store(true, std::memory_order_relaxed);
                    qWarning() << "[WaveformAnalyzer] Envelope segment failed:"
                               << e.what();
                }
            });
        }
        // The caller's own segment must not escape with an exception while
        // workers are still running: unwinding past a joinable std::thread
        // terminates the process.
        try {
            if (!segments.empty())
                runSegment(segments.front(), reader);
        } catch (const std::exception& e) {
            segmentFailed.store(true, std::memory_order_relaxed);
            qWarning() << "[WaveformAnalyzer] Envelope segment failed:"
                       << e.what();
        }
        for (auto& worker : workers)
            worker.join();
    }

    if (segmentFailed.load(std::memory_order_relaxed))
        return false;

    for (const auto& segment : segments) {
        globalMaxPeak = std::max(globalMaxPeak, segment.maxPeak);
        globalMaxSample = std::max(globalMaxSample, segment.maxSample);
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
