#include "WaveformEnvelopePass.h"
#include "waveform/WaveformAnalyzer.h"
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

struct FiltState {
    std::vector<float> low1, low2;
    std::vector<float> mid1, mid2;
    std::vector<float> high1, high2;

    void reset(int numCh, double) {
        const size_t n = static_cast<size_t>(numCh);
        low1.assign(n, 0.0f); low2.assign(n, 0.0f);
        mid1.assign(n, 0.0f); mid2.assign(n, 0.0f);
        high1.assign(n, 0.0f); high2.assign(n, 0.0f);
    }
};

namespace {

// Raw (un-normalized) band envelope state at the end of one waveform bin.
struct RawBin {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float bass = 0.0f;
    float mid = 0.0f;
    float treble = 0.0f;
};

// Peak mipmap entry: signed min/max of the mono downmix over a sub-bin.
struct RawPeak { float minRaw = 0.0f; float maxRaw = 0.0f; };

// Fixed filter coefficients for the 4-band bank described below. They depend
// only on the sample rate, so every segment worker shares one instance while
// keeping its own FiltState.
struct BandCoefficients {
    float low = 0.0f;
    float high = 0.0f;

    static BandCoefficients forSampleRate(double sampleRate)
    {
        const float sr = static_cast<float>(sampleRate);
        // 1-pole LP coefficient: a = 2*pi*fc / (2*pi*fc + sr).
        const auto lpCoef1 = [sr](float fc) {
            const float w = 2.0f * juce::MathConstants<float>::pi * fc / sr;
            return w / (w + 1.0f);
        };
        BandCoefficients c;
        c.low = lpCoef1(400.0f);
        c.high = lpCoef1(4000.0f);
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

    double overallSquares = 0.0;
    double bassSquares = 0.0;
    double midSquares = 0.0;
    double trebleSquares = 0.0;
    bool sampleSeen = false;
    float minimum = 0.0f;
    float maximum = 0.0f;

    for (int s = 0; s < numSamples; ++s) {
        float bestBass = 0.0f, bestMid = 0.0f, bestTreble = 0.0f;
        float signedPeak = 0.0f;
        float overall = 0.0f;

        for (int ch = 0; ch < channelCount; ++ch) {
            const auto ci = static_cast<std::size_t>(ch);
            const float in = channels[ci][s];
            if (std::abs(in) > std::abs(signedPeak))
                signedPeak = in;
            overall = std::max(overall, std::abs(in));

            filt.low1[ci] += c.low * (in - filt.low1[ci]);
            filt.low2[ci] += c.low * (filt.low1[ci] - filt.low2[ci]);
            filt.mid1[ci] += c.high * (in - filt.mid1[ci]);
            filt.mid2[ci] += c.high * (filt.mid1[ci] - filt.mid2[ci]);
            filt.high1[ci] += c.high * (in - filt.high1[ci]);
            const float highPass1 = in - filt.high1[ci];
            filt.high2[ci] += c.high * (highPass1 - filt.high2[ci]);
            const float highPass2 = highPass1 - filt.high2[ci];
            bestBass = std::max(bestBass, std::abs(filt.low2[ci]));
            bestMid = std::max(
                bestMid, std::abs(filt.mid2[ci] - filt.low2[ci]));
            bestTreble = std::max(bestTreble, std::abs(highPass2));
        }

        const float mono = signedPeak;
        if (!sampleSeen) {
            minimum = maximum = mono;
            sampleSeen = true;
        } else {
            minimum = std::min(minimum, mono);
            maximum = std::max(maximum, mono);
        }
        overallSquares += static_cast<double>(overall) * overall;
        bassSquares += static_cast<double>(bestBass) * bestBass;
        midSquares += static_cast<double>(bestMid) * bestMid;
        trebleSquares += static_cast<double>(bestTreble) * bestTreble;

        if (peaks != nullptr && peakRatio > 0) {
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

    }

    const float inverseCount = 1.0f / static_cast<float>(std::max(1, numSamples));
    out.minimum = minimum;
    out.maximum = maximum;
    out.peak = std::max(-minimum, maximum);
    out.rms = std::sqrt(static_cast<float>(overallSquares) * inverseCount);
    out.bass = std::sqrt(static_cast<float>(bassSquares) * inverseCount);
    out.mid = std::sqrt(static_cast<float>(midSquares) * inverseCount);
    out.treble = std::sqrt(static_cast<float>(trebleSquares) * inverseCount);
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
    // WaveformAnalyzer already limits concurrent tracks. Two workers per pass
    // keep progressive detail responsive without the 4 x 2 worker burst that
    // previously saturated a many-core machine as soon as deck 2 was loaded.
    const int budget = static_cast<int>(std::min<unsigned>(2u, hw / 2u - 1u));
    return std::clamp(std::min(budget, sourceChunkCount / 2), 1, 2);
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
        const float bass = std::clamp(
            lowAcc / static_cast<float>(sampleCount), 0.0f, 1.0f);
        const float treble = std::clamp(
            highAcc / static_cast<float>(sampleCount), 0.0f, 1.0f);
        const float mid = std::clamp(rms - 0.5f * bass - 0.25f * treble,
                                     0.0f, 1.0f);

        auto& frame = out[bin];
        frame.peak = peak;
        frame.rms = rms;
        frame.bass = bass;
        frame.mid = mid;
        frame.treble = treble;
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

    const auto overview = m_trackData->getOverviewWaveformData();
    const auto robustValue = [&overview](auto member, float floor) {
        std::vector<float> values;
        values.reserve(static_cast<std::size_t>(overview.size()));
        for (const auto& frame : overview) {
            const float value = std::max(0.0f, frame.*member);
            if (std::isfinite(value))
                values.push_back(value);
        }
        if (values.empty())
            return floor;
        const auto percentileIndex = std::min(
            values.size() - 1,
            static_cast<std::size_t>(std::floor(
                static_cast<double>(values.size() - 1) * 0.98)));
        std::nth_element(values.begin(), values.begin() + percentileIndex,
                         values.end());
        return std::max(floor, values[percentileIndex]);
    };
    const float amplitudeScale = robustValue(
        &TrackData::SpectralWaveformPoint::rms, 0.02f);
    const float peakScale = robustValue(
        &TrackData::SpectralWaveformPoint::peak, 0.05f);
    const int spectralRatio = std::max(
        1, static_cast<int>(std::lround(
            static_cast<double>(m_pointsPerSecond)
            / TrackData::SPECTRAL_POINTS_PER_SECOND)));
    const int totalSpectralBins = (numPoints + spectralRatio - 1)
        / spectralRatio;
    m_trackData->initializeWaveformOverview(totalSpectralBins);

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
    const auto publishChunk = [&input, spectralRatio, totalSpectralBins](
                                       int firstBin,
                                       const QVector<TrackData::WaveformBin>& waveform,
                                       const QVector<TrackData::SpectralWaveformPoint>& spectral,
                                       WaveformNormalizationState state
                                           = WaveformNormalizationState::Final) {
        if (!input.retainLegacyWaveform && !waveform.isEmpty() && !spectral.isEmpty())
            input.trackData->writePreparedWaveformRange(firstBin, waveform, spectral);
        input.trackData->writeWaveformOverviewRange(
            firstBin / spectralRatio, spectral);
        if (input.publishChunk && (!waveform.isEmpty() || !spectral.isEmpty()))
            input.publishChunk(firstBin, input.numPoints, waveform,
                firstBin / spectralRatio, totalSpectralBins, spectral, state);
    };

    // Shared shaping helper — used identically in preview AND final pass.
    auto shapeBin = [](float norm) -> float {
        return std::pow(std::clamp(norm, 0.0f, 1.0f), 0.72f);
    };

    // Single conversion from raw band envelopes to a display frame. The
    // priority prologue and the full pass must agree exactly, otherwise a
    // region changes brightness when the pass overwrites a priority chunk.
    const auto makeGeometry = [&](const RawBin& raw) {
        TrackData::WaveformBin geometry;
        geometry.minimum = std::clamp(raw.minimum / peakScale, -1.0f, 0.0f);
        geometry.maximum = std::clamp(raw.maximum / peakScale, 0.0f, 1.0f);
        geometry.peak = shapeBin(raw.peak / peakScale);
        geometry.rms = shapeBin(raw.rms / amplitudeScale);
        return geometry;
    };
    const auto makeSpectral = [&](const RawBin& raw) {
        TrackData::SpectralWaveformPoint spectral;
        spectral.peak = shapeBin(raw.peak / peakScale);
        spectral.rms = shapeBin(raw.rms / amplitudeScale);
        spectral.bass = shapeBin(raw.bass / amplitudeScale);
        spectral.mid = shapeBin(raw.mid / amplitudeScale);
        spectral.treble = shapeBin(raw.treble / amplitudeScale);
        return spectral;
    };
    const auto downsampleSpectral = [spectralRatio](
        const QVector<TrackData::SpectralWaveformPoint>& source) {
        QVector<TrackData::SpectralWaveformPoint> result;
        result.reserve((source.size() + spectralRatio - 1) / spectralRatio);
        for (int begin = 0; begin < source.size(); begin += spectralRatio) {
            const int end = std::min(
                static_cast<int>(source.size()), begin + spectralRatio);
            TrackData::SpectralWaveformPoint point;
            double rmsSquares = 0.0;
            double bassSquares = 0.0;
            double midSquares = 0.0;
            double trebleSquares = 0.0;
            for (int index = begin; index < end; ++index) {
                const auto& value = source[index];
                point.peak = std::max(point.peak, value.peak);
                rmsSquares += value.rms * value.rms;
                bassSquares += value.bass * value.bass;
                midSquares += value.mid * value.mid;
                trebleSquares += value.treble * value.treble;
            }
            const float inverse = 1.0f / static_cast<float>(end - begin);
            point.rms = std::sqrt(static_cast<float>(rmsSquares) * inverse);
            point.bass = std::sqrt(static_cast<float>(bassSquares) * inverse);
            point.mid = std::sqrt(static_cast<float>(midSquares) * inverse);
            point.treble = std::sqrt(static_cast<float>(trebleSquares) * inverse);
            result.push_back(point);
        }
        return result;
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

        QVector<TrackData::WaveformBin> geometry;
        QVector<TrackData::SpectralWaveformPoint> fullRateSpectral;
        geometry.reserve(end - begin);
        fullRateSpectral.reserve(end - begin);
        for (int bin = begin; bin < end && !threadShouldExit(); ++bin) {
            if ((bin & 0x1F) == 0)
                cooperateWithRealtime();
            if (!processPriorityBin(bin, filter, raw))
                return;
            geometry.push_back(makeGeometry(raw));
            fullRateSpectral.push_back(makeSpectral(raw));
        }
        if (geometry.size() != end - begin)
            return;
        const auto spectral = downsampleSpectral(fullRateSpectral);
        if (input.retainLegacyWaveform) {
            m_trackData->writeWaveformRange(begin, geometry);
            m_trackData->writeSpectralWaveformRange(begin / spectralRatio, spectral);
        }
        publishChunk(begin, geometry, spectral);
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
        m_trackData->preallocateSpectralWaveform(totalSpectralBins);
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
        QVector<TrackData::SpectralWaveformPoint> fullRateSpectralBatch;
        binBatch.reserve(kChunk);
        fullRateSpectralBatch.reserve(kChunk);
        int batchStart = segment.binBegin;

        const auto flush = [&]() {
            if (binBatch.isEmpty())
                return;
            const auto spectralBatch = downsampleSpectral(fullRateSpectralBatch);
            const std::lock_guard<std::mutex> lock(publishMutex);
            if (input.retainLegacyWaveform) {
                m_trackData->writeWaveformRange(batchStart, binBatch);
                m_trackData->writeSpectralWaveformRange(
                    batchStart / spectralRatio, spectralBatch);
            }
            publishChunk(batchStart, binBatch, spectralBatch);
            m_trackData->reportAnalysisProgress(
                (static_cast<double>(
                     completedBins.load(std::memory_order_relaxed))
                 / static_cast<double>(numPoints)) * 0.50, true);
            binBatch.clear();
            fullRateSpectralBatch.clear();
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
                std::max({raw.bass, raw.mid, raw.treble}));

            if (binBatch.isEmpty())
                batchStart = bin;
            binBatch.append(makeGeometry(raw));
            fullRateSpectralBatch.append(makeSpectral(raw));
            completedBins.fetch_add(1, std::memory_order_relaxed);

            if (binBatch.size() >= kChunk)
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
