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
    if (label == QStringLiteral("Drop"))
        return QStringLiteral("#FF3B30");
    if (label == QStringLiteral("Breakdown"))
        return QStringLiteral("#2A7FFF");
    if (label == QStringLiteral("Build-up"))
        return QStringLiteral("#FFD21F");
    if (label == QStringLiteral("Intro"))
        return QStringLiteral("#47C2FF");
    if (label == QStringLiteral("Outro"))
        return QStringLiteral("#9A9A9A");
    return QStringLiteral("#32CD32");
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
        block.label = QStringLiteral("Phrase");
        block.colorHex = colorForLabel(block.label);
    }

    for (auto& block : blocks) {
        if (block.lowBandNorm > 0.75f && block.overallNorm > 0.65f) {
            block.label = QStringLiteral("Drop");
            block.colorHex = colorForLabel(block.label);
        }
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        auto& block = blocks[i];
        if (block.lowBandNorm >= 0.2f)
            continue;

        const float prevOverall = (i > 0) ? blocks[i - 1].overallNorm : block.overallNorm;
        const float nextOverall = (i + 1 < blocks.size()) ? blocks[i + 1].overallNorm : block.overallNorm;
        const bool hasContrast = (prevOverall - block.overallNorm > 0.22f)
            || (nextOverall - block.overallNorm > 0.22f)
            || (std::max(prevOverall, nextOverall) > 0.6f && block.overallNorm < 0.45f);

        if (hasContrast && block.label != QStringLiteral("Drop")) {
            block.label = QStringLiteral("Breakdown");
            block.colorHex = colorForLabel(block.label);
        }
    }

    for (size_t i = 1; i + 1 < blocks.size(); ++i) {
        if (blocks[i + 1].label != QStringLiteral("Drop"))
            continue;

        bool rising2 = blocks[i - 1].overallNorm < blocks[i].overallNorm;
        bool rising3 = false;
        if (i >= 2) {
            rising3 = blocks[i - 2].overallNorm < blocks[i - 1].overallNorm
                && blocks[i - 1].overallNorm < blocks[i].overallNorm;
        }

        if ((rising2 || rising3) && blocks[i].label != QStringLiteral("Drop")) {
            blocks[i].label = QStringLiteral("Build-up");
            blocks[i].colorHex = colorForLabel(blocks[i].label);

            if (rising3 && blocks[i - 1].label != QStringLiteral("Drop")) {
                blocks[i - 1].label = QStringLiteral("Build-up");
                blocks[i - 1].colorHex = colorForLabel(blocks[i - 1].label);
            }
        }
    }

    int firstSignal = -1;
    int lastSignal = -1;
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
        if (!blocks[static_cast<size_t>(i)].hasSignal)
            continue;
        if (firstSignal < 0)
            firstSignal = i;
        lastSignal = i;
    }

    if (firstSignal >= 0) {
        blocks[static_cast<size_t>(firstSignal)].label = QStringLiteral("Intro");
        blocks[static_cast<size_t>(firstSignal)].colorHex = colorForLabel(QStringLiteral("Intro"));
    }
    if (lastSignal >= 0 && lastSignal != firstSignal) {
        blocks[static_cast<size_t>(lastSignal)].label = QStringLiteral("Outro");
        blocks[static_cast<size_t>(lastSignal)].colorHex = colorForLabel(QStringLiteral("Outro"));
    }
}

std::vector<TrackSegment> PhraseAnalyzer::smoothAndMergeSegments(std::vector<PhraseBlock>& blocks,
                                                                 double durationSec) const
{
    // Rule 1: dynamic thresholds from track-wide average RMS values.
    double sumOverall = 0.0;
    double sumLow = 0.0;
    int signalCount = 0;
    for (const auto& block : blocks) {
        if (!block.hasSignal)
            continue;
        sumOverall += block.overallRms;
        sumLow += block.lowBandRms;
        ++signalCount;
    }

    const float avgOverall = (signalCount > 0) ? static_cast<float>(sumOverall / signalCount) : 0.0f;
    const float avgLow = (signalCount > 0) ? static_cast<float>(sumLow / signalCount) : 0.0f;
    const float dropLowThreshold = avgLow * 1.20f;
    const float dropOverallThreshold = avgOverall * 1.10f;

    for (auto& block : blocks) {
        if (!block.hasSignal)
            continue;

        const bool isDynamicDrop =
            (block.lowBandRms > dropLowThreshold) &&
            (block.overallRms > dropOverallThreshold);

        if (isDynamicDrop) {
            block.label = QStringLiteral("Drop");
            block.colorHex = colorForLabel(block.label);
            continue;
        }

        if (block.label == QStringLiteral("Drop")) {
            block.label = QStringLiteral("Phrase");
            block.colorHex = colorForLabel(block.label);
        }
    }

    // Rule 2: context-aware outlier filter.
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

    // Rule 3: enforce minimum segment length of 32 beats (2 blocks), except Outro.
    constexpr int minBlocks = 2;
    for (size_t i = 0; i < merged.size();) {
        if (merged[i].label == QStringLiteral("Outro") || merged[i].blockCount >= minBlocks) {
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
        segments.push_back({
            run.label,
            run.startTime,
            std::min(run.endTime, static_cast<float>(durationSec)),
            run.colorHex
        });
    }

    return segments;
}
