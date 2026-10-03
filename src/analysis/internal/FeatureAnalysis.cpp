#include "FeatureAnalysis.h"

#include <juce_audio_basics/juce_audio_basics.h>
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
                                                   const std::function<void(double)>& onProgress,
                                                   const std::function<bool()>& shouldPause) const
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
        if ((hopCount & 0x3F) == 0) {
            while (shouldPause && shouldPause()
                   && (cancelThread == nullptr || !cancelThread->threadShouldExit())) {
                juce::Thread::sleep(4);
            }
        }

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

namespace {

constexpr int kPhraseBeats = 16;
constexpr float kLowPassCutoffHz = 250.0f;
constexpr float kSignalFloorNorm = 0.06f;

float computeRms(const std::vector<float>& mono) {
    if (mono.empty())
        return 0.0f;

    double sumSq = 0.0;
    for (float s : mono)
        sumSq += static_cast<double>(s) * static_cast<double>(s);

    return static_cast<float>(std::sqrt(sumSq / static_cast<double>(mono.size())));
}

QString colorForLabel(const QString& label) {
    if (label == QStringLiteral("HighEnergy"))
        return QStringLiteral("#D8563F");
    if (label == QStringLiteral("MediumEnergy"))
        return QStringLiteral("#7E9A58");
    if (label == QStringLiteral("LowEnergy"))
        return QStringLiteral("#496C96");
    if (label == QStringLiteral("SilenceOrOutro"))
        return QStringLiteral("#555555");
    return QStringLiteral("#00000000");
}

} // namespace

std::vector<TrackSegment> PhraseAnalyzer::analyze(juce::AudioFormatReader& reader,
                                                  const std::vector<double>& beatTimestamps,
                                                  double durationSec) const
{
    auto blocks = buildBlocks(beatTimestamps, durationSec);
    if (blocks.empty())
        return {};

    extractFeatures(reader, blocks);
    normalizeAndLabel(blocks);
    return smoothAndMergeSegments(blocks, durationSec);
}

std::vector<TrackSegment> PhraseAnalyzer::analyze(const analysis::AnalysisFeatures& features,
                                                  const std::vector<TrackData::BeatMarker>& beats,
                                                  double durationSec) const
{
    std::vector<double> downbeatAligned;
    downbeatAligned.reserve(beats.size());

    size_t start = 0;
    for (size_t i = 0; i < beats.size(); ++i) {
        if (beats[i].isDownbeat) {
            start = i;
            break;
        }
    }
    for (size_t i = start; i < beats.size(); ++i)
        downbeatAligned.push_back(beats[i].positionSec);

    auto blocks = buildBlocks(downbeatAligned, durationSec);
    if (blocks.empty())
        return {};

    for (auto& block : blocks) {
        const size_t startFrame = features.secondsToFrame(block.startTime);
        const size_t endFrame = std::min(features.rms.size(), features.secondsToFrame(block.endTime) + 1);
        if (startFrame >= endFrame || endFrame > features.rms.size())
            continue;

        double rms = 0.0;
        double low = 0.0;
        double onset = 0.0;
        for (size_t i = startFrame; i < endFrame; ++i) {
            rms += features.rms[i];
            low += features.lowEnergy[i];
            onset += features.onsetStrength[i];
        }
        const double inv = 1.0 / static_cast<double>(endFrame - startFrame);
        block.overallRms = static_cast<float>(rms * inv);
        block.lowBandRms = static_cast<float>(low * inv);
        block.hasSignal = (block.overallRms > 0.04f || onset * inv > 0.05);
    }

    normalizeAndLabel(blocks);
    auto segments = smoothAndMergeSegments(blocks, durationSec);
    for (auto& segment : segments) {
        const size_t startFrame = features.secondsToFrame(segment.startTime);
        const size_t endFrame = std::min(features.rms.size(), features.secondsToFrame(segment.endTime) + 1);
        if (startFrame >= endFrame)
            continue;
        double energy = 0.0;
        for (size_t i = startFrame; i < endFrame; ++i)
            energy += features.rms[i];
        segment.confidence = static_cast<float>(std::clamp(energy / static_cast<double>(endFrame - startFrame), 0.0, 1.0));
    }
    return segments;
}

std::vector<PhraseAnalyzer::PhraseBlock> PhraseAnalyzer::buildBlocks(
    const std::vector<double>& beatTimestamps, double durationSec) const
{
    std::vector<PhraseBlock> blocks;
    if (beatTimestamps.size() <= static_cast<size_t>(kPhraseBeats) || durationSec <= 0.0)
        return blocks;

    const size_t phraseBeats = static_cast<size_t>(kPhraseBeats);
    blocks.reserve((beatTimestamps.size() - 1) / phraseBeats);

    // A block needs both its start marker and the marker kPhraseBeats later.
    // Checking the end index directly avoids accepting index size() when the
    // number of supplied beat markers is an exact multiple of kPhraseBeats.
    for (size_t i = 0; i < beatTimestamps.size()
         && phraseBeats < beatTimestamps.size() - i;
         i += phraseBeats) {
        const float start = static_cast<float>(beatTimestamps[i]);
        const float end = static_cast<float>(beatTimestamps[i + phraseBeats]);
        if (!std::isfinite(start) || !std::isfinite(end)
            || end <= start + 0.01f)
            continue;

        PhraseBlock block;
        block.startTime = std::clamp(start, 0.0f, static_cast<float>(durationSec));
        block.endTime = std::clamp(end, block.startTime, static_cast<float>(durationSec));
        block.label = QStringLiteral("Phrase");
        block.colorHex = colorForLabel(block.label);
        blocks.push_back(block);
    }

    return blocks;
}

void PhraseAnalyzer::extractFeatures(juce::AudioFormatReader& reader,
                                     std::vector<PhraseBlock>& blocks) const
{
    const double sampleRate = reader.sampleRate;
    const int channels = static_cast<int>(reader.numChannels);

    if (sampleRate <= 0.0 || channels <= 0)
        return;

    juce::IIRFilter lowPass;
    lowPass.setCoefficients(juce::IIRCoefficients::makeLowPass(sampleRate, kLowPassCutoffHz));

    for (auto& block : blocks) {
        const juce::int64 startSample = static_cast<juce::int64>(std::floor(block.startTime * sampleRate));
        const juce::int64 endSample = static_cast<juce::int64>(std::ceil(block.endTime * sampleRate));
        const juce::int64 sampleCount64 = std::max<juce::int64>(0, endSample - startSample);

        if (sampleCount64 <= 16 || startSample >= reader.lengthInSamples)
            continue;

        const int sampleCount = static_cast<int>(std::min<juce::int64>(sampleCount64, reader.lengthInSamples - startSample));
        juce::AudioBuffer<float> readBuf(channels, sampleCount);
        if (!reader.read(&readBuf, 0, sampleCount, startSample, true, true))
            continue;

        std::vector<float> mono(static_cast<size_t>(sampleCount), 0.0f);
        const float invCh = 1.0f / static_cast<float>(channels);
        for (int s = 0; s < sampleCount; ++s) {
            float sum = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                sum += readBuf.getSample(ch, s);
            mono[static_cast<size_t>(s)] = sum * invCh;
        }

        block.overallRms = computeRms(mono);

        lowPass.reset();
        for (int s = 0; s < sampleCount; ++s)
            mono[static_cast<size_t>(s)] = lowPass.processSingleSampleRaw(mono[static_cast<size_t>(s)]);

        block.lowBandRms = computeRms(mono);
    }
}

void PhraseAnalyzer::normalizeAndLabel(std::vector<PhraseBlock>& blocks) const
{
    if (blocks.empty())
        return;

    float maxLow = 0.0f;
    float maxOverall = 0.0f;
    for (const auto& block : blocks) {
        maxLow = std::max(maxLow, block.lowBandRms);
        maxOverall = std::max(maxOverall, block.overallRms);
    }

    maxLow = std::max(maxLow, 1e-6f);
    maxOverall = std::max(maxOverall, 1e-6f);

    for (auto& block : blocks) {
        block.lowBandNorm = std::clamp(block.lowBandRms / maxLow, 0.0f, 1.0f);
        block.overallNorm = std::clamp(block.overallRms / maxOverall, 0.0f, 1.0f);
        block.hasSignal = (block.overallNorm >= kSignalFloorNorm);
    }

    for (auto& block : blocks) {
        const float confidence = std::abs(block.overallNorm - 0.50f) * 1.35f
            + std::abs(block.lowBandNorm - 0.50f) * 0.65f;
        if (!block.hasSignal || block.overallNorm < 0.055f) {
            block.label = QStringLiteral("SilenceOrOutro");
        } else if (confidence < 0.42f) {
            block.label = QStringLiteral("Unknown");
        } else if (block.overallNorm > 0.68f && block.lowBandNorm > 0.52f) {
            block.label = QStringLiteral("HighEnergy");
        } else if (block.overallNorm < 0.28f || block.lowBandNorm < 0.22f) {
            block.label = QStringLiteral("LowEnergy");
        } else if (confidence >= 0.50f) {
            block.label = QStringLiteral("MediumEnergy");
        } else {
            block.label = QStringLiteral("Unknown");
        }
        block.colorHex = colorForLabel(block.label);
    }
}

std::vector<TrackSegment> PhraseAnalyzer::smoothAndMergeSegments(
    std::vector<PhraseBlock>& blocks, double durationSec) const
{
    // Rule 1: context-aware outlier filter on neutral energy states only.
    if (blocks.size() >= 3) {
        std::vector<QString> smoothedLabels;
        smoothedLabels.reserve(blocks.size());
        for (const auto& b : blocks)
            smoothedLabels.push_back(b.label);

        for (size_t i = 1; i + 1 < blocks.size(); ++i) {
            if (blocks[i - 1].label == blocks[i + 1].label && blocks[i].label != blocks[i - 1].label)
                smoothedLabels[i] = blocks[i - 1].label;
        }

        for (size_t i = 0; i < blocks.size(); ++i) {
            blocks[i].label = smoothedLabels[i];
            blocks[i].colorHex = colorForLabel(blocks[i].label);
        }
    }

    struct MergedSeg {
        QString label;
        QString colorHex;
        float startTime = 0.0f;
        float endTime = 0.0f;
        int blockCount = 0;
        float avgOverallRms = 0.0f;
    };

    std::vector<MergedSeg> merged;
    if (!blocks.empty()) {
        MergedSeg cur;
        cur.label = blocks.front().label;
        cur.colorHex = blocks.front().colorHex;
        cur.startTime = blocks.front().startTime;
        cur.endTime = blocks.front().endTime;
        cur.blockCount = 1;
        cur.avgOverallRms = blocks.front().overallRms;

        for (size_t i = 1; i < blocks.size(); ++i) {
            const auto& b = blocks[i];
            if (b.label == cur.label) {
                cur.endTime = b.endTime;
                cur.avgOverallRms =
                    (cur.avgOverallRms * static_cast<float>(cur.blockCount) + b.overallRms)
                    / static_cast<float>(cur.blockCount + 1);
                ++cur.blockCount;
            } else {
                merged.push_back(cur);
                cur.label = b.label;
                cur.colorHex = b.colorHex;
                cur.startTime = b.startTime;
                cur.endTime = b.endTime;
                cur.blockCount = 1;
                cur.avgOverallRms = b.overallRms;
            }
        }
        merged.push_back(cur);
    }

    // Rule 2: enforce minimum segment length of 32 beats (2 blocks).
    constexpr int minBlocks = 2;
    for (size_t i = 0; i < merged.size();) {
        if (merged[i].blockCount >= minBlocks) {
            ++i;
            continue;
        }

        const bool hasPrev = (i > 0);
        const bool hasNext = (i + 1 < merged.size());
        if (!hasPrev && !hasNext) {
            ++i;
            continue;
        }

        size_t target = i;
        if (hasPrev && hasNext) {
            const float dPrev = std::abs(merged[i].avgOverallRms - merged[i - 1].avgOverallRms);
            const float dNext = std::abs(merged[i].avgOverallRms - merged[i + 1].avgOverallRms);
            target = (dPrev <= dNext) ? (i - 1) : (i + 1);
        } else if (hasPrev) {
            target = i - 1;
        } else {
            target = i + 1;
        }

        if (target < i) {
            auto& dst = merged[target];
            const auto src = merged[i];
            const int totalBlocks = dst.blockCount + src.blockCount;
            dst.startTime = std::min(dst.startTime, src.startTime);
            dst.endTime = std::max(dst.endTime, src.endTime);
            dst.avgOverallRms =
                (dst.avgOverallRms * static_cast<float>(dst.blockCount)
                 + src.avgOverallRms * static_cast<float>(src.blockCount))
                / static_cast<float>(totalBlocks);
            dst.blockCount = totalBlocks;
            merged.erase(merged.begin() + static_cast<ptrdiff_t>(i));
            if (i > 0)
                --i;
            continue;
        }

        auto& dst = merged[target];
        const auto src = merged[i];
        const int totalBlocks = dst.blockCount + src.blockCount;
        dst.startTime = std::min(dst.startTime, src.startTime);
        dst.endTime = std::max(dst.endTime, src.endTime);
        dst.avgOverallRms =
            (dst.avgOverallRms * static_cast<float>(dst.blockCount)
             + src.avgOverallRms * static_cast<float>(src.blockCount))
            / static_cast<float>(totalBlocks);
        dst.blockCount = totalBlocks;
        merged.erase(merged.begin() + static_cast<ptrdiff_t>(i));
    }

    std::vector<TrackSegment> segments;
    if (merged.empty())
        return segments;

    segments.reserve(merged.size());
    for (const auto& run : merged) {
        if (run.endTime <= run.startTime + 0.01f)
            continue;
        if (run.label == QStringLiteral("Unknown"))
            continue;
        const float confidence =
            (run.label == QStringLiteral("HighEnergy") || run.label == QStringLiteral("LowEnergy"))
                ? 0.78f
                : (run.label == QStringLiteral("MediumEnergy") ? 0.58f : 0.62f);
        segments.push_back({
            run.label,
            run.startTime,
            std::min(run.endTime, static_cast<float>(durationSec)),
            run.colorHex,
            confidence
        });
    }

    return segments;
}
