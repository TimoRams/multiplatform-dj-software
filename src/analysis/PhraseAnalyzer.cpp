#include "PhraseAnalyzer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

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

std::vector<PhraseBlock> PhraseAnalyzer::buildBlocks(const std::vector<double>& beatTimestamps,
                                                     double durationSec) const
{
    std::vector<PhraseBlock> blocks;
    if (beatTimestamps.size() <= static_cast<size_t>(kPhraseBeats) || durationSec <= 0.0)
        return blocks;

    const size_t maxStart = beatTimestamps.size() - static_cast<size_t>(kPhraseBeats);
    blocks.reserve(maxStart / static_cast<size_t>(kPhraseBeats) + 1);

    for (size_t i = 0; i <= maxStart; i += static_cast<size_t>(kPhraseBeats)) {
        const float start = static_cast<float>(beatTimestamps[i]);
        const float end = static_cast<float>(beatTimestamps[i + static_cast<size_t>(kPhraseBeats)]);
        if (end <= start + 0.01f)
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

std::vector<TrackSegment> PhraseAnalyzer::smoothAndMergeSegments(std::vector<PhraseBlock>& blocks,
                                                                 double durationSec) const
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
