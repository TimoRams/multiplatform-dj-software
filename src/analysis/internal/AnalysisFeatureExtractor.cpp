#include "AnalysisFeatureExtractor.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis {
namespace {

void normalize(std::vector<float>& values)
{
    if (values.empty())
        return;

    // A single drop, clipping transient or decoder discontinuity must not
    // shrink the onset curve for the remaining track. Scale against a robust
    // high percentile and merely saturate the few louder frames.
    std::vector<float> ordered = values;
    const std::size_t percentileIndex = std::min(
        ordered.size() - 1,
        static_cast<std::size_t>(std::floor(
            static_cast<double>(ordered.size() - 1) * 0.98)));
    std::nth_element(ordered.begin(),
                     ordered.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
                     ordered.end());
    const float scale = ordered[percentileIndex];
    if (scale <= 1.0e-8f)
        return;
    const float inv = 1.0f / scale;
    for (float& v : values)
        v = std::clamp(v * inv, 0.0f, 1.0f);
}

float positiveFlux(const std::vector<float>& current,
                   const std::vector<float>& previous,
                   int firstBin,
                   int lastBin)
{
    double sum = 0.0;
    for (int i = firstBin; i <= lastBin; ++i)
        sum += std::max(0.0f, current[static_cast<size_t>(i)] - previous[static_cast<size_t>(i)]);
    return static_cast<float>(sum);
}

// Sliding mono window over a decoder.
//
// Frames overlap 4:1 (2048-sample frame, 512-sample hop), so reading one frame
// per hop decoded every sample four times and re-ran the channel downmix four
// times with it. Analysis positions only ever move forward, so one large block
// serves many frames: a seven-minute track now costs ~340 reader calls and one
// decode of each sample instead of ~43k calls and four.
class MonoWindow final
{
public:
    static constexpr int kCapacitySamples = 1 << 16;

    MonoWindow(juce::AudioFormatReader& reader, juce::int64 totalSamples)
        : m_reader(reader)
        , m_totalSamples(totalSamples)
        , m_channels(std::max(1, static_cast<int>(reader.numChannels)))
        , m_buffer(m_channels, kCapacitySamples)
        , m_mono(static_cast<std::size_t>(kCapacitySamples), 0.0f)
    {
    }

    // Returns a pointer to `length` mono samples starting at `start`, or
    // nullptr when the range cannot be served.
    const float* acquire(juce::int64 start, int length)
    {
        if (start < 0 || length <= 0 || length > kCapacitySamples)
            return nullptr;
        if (start < m_start || start + length > m_start + m_length) {
            const int toRead = static_cast<int>(std::min<juce::int64>(
                kCapacitySamples,
                std::max<juce::int64>(length, m_totalSamples - start)));
            m_buffer.clear();
            if (!m_reader.read(&m_buffer, 0, toRead, start, true, true))
                return nullptr;
            const float invChannels = 1.0f / static_cast<float>(m_channels);
            for (int s = 0; s < toRead; ++s) {
                float mono = 0.0f;
                for (int ch = 0; ch < m_channels; ++ch)
                    mono += m_buffer.getSample(ch, s);
                m_mono[static_cast<std::size_t>(s)] = mono * invChannels;
            }
            m_start = start;
            m_length = toRead;
        }
        return m_mono.data() + (start - m_start);
    }

private:
    juce::AudioFormatReader& m_reader;
    juce::int64 m_totalSamples = 0;
    int m_channels = 1;
    juce::AudioBuffer<float> m_buffer;
    std::vector<float> m_mono;
    juce::int64 m_start = 0;
    int m_length = 0;
};

} // namespace

AnalysisFeatureExtractor::AnalysisFeatureExtractor()
    : AnalysisFeatureExtractor(Options{})
{
}

AnalysisFeatureExtractor::AnalysisFeatureExtractor(Options options)
    : m_options(options)
{
    m_options.frameSize = std::max(512, m_options.frameSize);
    m_options.hopSize = std::clamp(m_options.hopSize, 128, m_options.frameSize);
}

AnalysisFeatures AnalysisFeatureExtractor::extract(juce::AudioFormatReader& reader,
                                                   juce::Thread* cancelThread,
                                                   const std::function<void(double)>& onProgress) const
{
    AnalysisFeatures out;
    out.sampleRate = reader.sampleRate > 0.0 ? reader.sampleRate : 44100.0;
    out.frameSize = m_options.frameSize;
    out.hopSize = m_options.hopSize;
    out.durationSec = out.sampleRate > 0.0
        ? static_cast<double>(reader.lengthInSamples) / out.sampleRate
        : 0.0;

    const int channels = static_cast<int>(reader.numChannels);
    if (channels <= 0 || reader.lengthInSamples <= 0)
        return out;

    const int order = static_cast<int>(std::ceil(std::log2(static_cast<double>(out.frameSize))));
    juce::dsp::FFT fft(order);
    const int fftSize = 1 << order;
    const int bins = fftSize / 2;

    MonoWindow monoWindow(reader, reader.lengthInSamples);
    std::vector<float> window(static_cast<size_t>(fftSize), 0.0f);
    for (int i = 0; i < out.frameSize; ++i)
        window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / std::max(1, out.frameSize - 1));

    std::vector<float> fftData(static_cast<size_t>(fftSize * 2), 0.0f);
    std::vector<float> magnitudes(static_cast<size_t>(bins + 1), 0.0f);
    std::vector<float> previousMagnitudes(static_cast<size_t>(bins + 1), 0.0f);

    const auto binForHz = [&](double hz) {
        return std::clamp(static_cast<int>(std::round((hz * fftSize) / out.sampleRate)), 1, bins);
    };
    const int lowLo = binForHz(20.0);
    const int lowHi = binForHz(180.0);
    const int midLo = binForHz(180.0);
    const int midHi = binForHz(2200.0);
    const int highLo = binForHz(2200.0);
    const int highHi = binForHz(std::min(12000.0, out.sampleRate * 0.45));

    const juce::int64 analysisSamples = (m_options.maxDurationSec > 0.0 && out.sampleRate > 0.0)
        ? std::min(reader.lengthInSamples,
                   static_cast<juce::int64>(m_options.maxDurationSec * out.sampleRate))
        : reader.lengthInSamples;
    const juce::int64 lastStart = std::max<juce::int64>(0, analysisSamples - out.frameSize);
    juce::int64 hopCount = 0;
    const juce::int64 totalHops = std::max<juce::int64>(1, (lastStart / out.hopSize) + 1);

    for (juce::int64 pos = 0; pos <= lastStart; pos += out.hopSize) {
        if (cancelThread != nullptr && cancelThread->threadShouldExit())
            break;

        if (onProgress && (hopCount & 0x3F) == 0)
            onProgress(static_cast<double>(hopCount) / static_cast<double>(totalHops));
        ++hopCount;

        const float* frame = monoWindow.acquire(pos, out.frameSize);
        if (frame == nullptr)
            break;

        std::fill(fftData.begin(), fftData.end(), 0.0f);
        double sumSq = 0.0;
        for (int s = 0; s < out.frameSize; ++s) {
            const float mono = frame[s];
            sumSq += static_cast<double>(mono) * static_cast<double>(mono);
            fftData[static_cast<size_t>(s)] = mono * window[static_cast<size_t>(s)];
        }

        fft.performRealOnlyForwardTransform(fftData.data());
        for (int b = 1; b <= bins; ++b) {
            const float re = fftData[static_cast<size_t>(2 * b)];
            const float im = fftData[static_cast<size_t>(2 * b + 1)];
            // Log compression makes spectral flux respond to musical changes
            // instead of being dominated by the loudest kick in the file.
            magnitudes[static_cast<size_t>(b)] = std::log1p(
                std::sqrt(re * re + im * im));
        }

        auto bandEnergy = [&](int lo, int hi) {
            double sum = 0.0;
            for (int b = lo; b <= hi; ++b) {
                const float mag = magnitudes[static_cast<size_t>(b)];
                sum += static_cast<double>(mag) * static_cast<double>(mag);
            }
            return static_cast<float>(std::sqrt(sum / static_cast<double>(std::max(1, hi - lo + 1))));
        };

        const float rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(out.frameSize)));
        const float low = bandEnergy(lowLo, lowHi);
        const float mid = bandEnergy(midLo, midHi);
        const float high = bandEnergy(highLo, highHi);
        const float flux = positiveFlux(magnitudes, previousMagnitudes, 1, bins);
        const float lowFlux = positiveFlux(magnitudes, previousMagnitudes, lowLo, lowHi);

        out.rms.push_back(rms);
        out.lowEnergy.push_back(low);
        out.midEnergy.push_back(mid);
        out.highEnergy.push_back(high);
        out.spectralFlux.push_back(flux);
        out.lowSpectralFlux.push_back(lowFlux);

        // Every bin from 1..bins is rewritten next iteration and bin 0 is never
        // written, so swapping is equivalent to the copy this used to make.
        std::swap(previousMagnitudes, magnitudes);
    }

    const size_t n = out.rms.size();
    out.energyNovelty.assign(n, 0.0f);
    out.transientStrength.assign(n, 0.0f);
    out.onsetStrength.assign(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        out.energyNovelty[i] = std::max(0.0f, out.rms[i] - out.rms[i - 1]);
        out.transientStrength[i] = 0.55f * out.spectralFlux[i]
                                 + 0.30f * out.lowSpectralFlux[i]
                                 + 0.15f * out.energyNovelty[i];
    }

    normalize(out.rms);
    normalize(out.lowEnergy);
    normalize(out.midEnergy);
    normalize(out.highEnergy);
    normalize(out.spectralFlux);
    normalize(out.lowSpectralFlux);
    normalize(out.energyNovelty);
    normalize(out.transientStrength);

    for (size_t i = 0; i < n; ++i) {
        out.onsetStrength[i] = std::clamp(0.45f * out.spectralFlux[i]
                                        + 0.30f * out.lowSpectralFlux[i]
                                        + 0.15f * out.transientStrength[i]
                                        + 0.10f * out.energyNovelty[i],
                                        0.0f, 1.0f);
    }
    normalize(out.onsetStrength);

    return out;
}

} // namespace analysis
