#include "HermiteResamplingAudioSource.h"

#include "HermiteKernel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

HermiteResamplingAudioSource::HermiteResamplingAudioSource(juce::AudioSource* inputSource,
                                                           bool deleteInputWhenDeleted,
                                                           int numChannels)
    : input(inputSource, deleteInputWhenDeleted),
      channels(std::max(1, numChannels))
{
}

void HermiteResamplingAudioSource::setResamplingRatio(double samplesInPerOutputSample)
{
    ratio.store(std::clamp(samplesInPerOutputSample, 0.0001, 64.0), std::memory_order_relaxed);
}

double HermiteResamplingAudioSource::getResamplingRatio() const
{
    return ratio.load(std::memory_order_relaxed);
}

void HermiteResamplingAudioSource::snapSmoothedRatio()
{
    smoothedRatio = ratio.load(std::memory_order_relaxed);
}

void HermiteResamplingAudioSource::armDirectionCrossfade()
{
    directionFadeRemaining = kDirectionFadeSamples;
}

void HermiteResamplingAudioSource::resetStream()
{
    sourceBuffer.clear();
    sourceBufferSize = 0;
    sourceBufferPos = 2.0;
    directionFadeRemaining = 0;
    const double r = ratio.load(std::memory_order_relaxed);
    lastRatio = r;
    smoothedRatio = r;
}

void HermiteResamplingAudioSource::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    outputSampleRate = std::max(1.0, sampleRate);
    blockSize = std::max(64, samplesPerBlockExpected);
    sourceBuffer.setSize(channels, blockSize * 8, false, true, true);
    sourceBuffer.clear();
    sourceBufferSize = 0;
    sourceBufferPos = 2.0;
    const double r = ratio.load(std::memory_order_relaxed);
    lastRatio = r;
    smoothedRatio = r;
    directionFadeRemaining = 0;

    if (input)
        input->prepareToPlay(samplesPerBlockExpected, sampleRate);
}

void HermiteResamplingAudioSource::releaseResources()
{
    sourceBuffer.setSize(0, 0);
    sourceBufferSize = 0;
    if (input)
        input->releaseResources();
}

void HermiteResamplingAudioSource::ensureSourceBuffer(int minAheadSamples)
{
    if (!input)
        return;

    const int capacity = sourceBuffer.getNumSamples();
    if (capacity < 8)
        return;

    if (!std::isfinite(sourceBufferPos))
        sourceBufferPos = 2.0;
    sourceBufferPos = std::clamp(sourceBufferPos, 0.0, static_cast<double>(capacity - 1));

    const int needed = std::min(capacity - 1,
                                static_cast<int>(std::ceil(sourceBufferPos)) + minAheadSamples);

    while (sourceBufferSize < needed && sourceBufferSize < capacity - blockSize) {
        const int pullStart = sourceBufferSize;
        const int pullCount = std::min(blockSize, capacity - pullStart);
        if (pullCount <= 0)
            break;
        juce::AudioSourceChannelInfo pullInfo(&sourceBuffer, pullStart, pullCount);
        input->getNextAudioBlock(pullInfo);
        sourceBufferSize += pullCount;
    }

    const int consume = std::clamp(static_cast<int>(std::floor(sourceBufferPos)) - 2,
                                   0,
                                   std::max(0, sourceBufferSize - 7));
    const int keep = sourceBufferSize - consume;
    if (consume > 0 && keep > 6) {
        for (int ch = 0; ch < channels; ++ch) {
            float* dst = sourceBuffer.getWritePointer(ch);
            const float* src = sourceBuffer.getReadPointer(ch);
            std::memmove(dst, src + consume, static_cast<size_t>(keep) * sizeof(float));
        }
        sourceBufferSize = keep;
        sourceBufferPos -= static_cast<double>(consume);
    }
}

float HermiteResamplingAudioSource::readHermite(int channel, double position) const
{
    const int i = static_cast<int>(std::floor(position));
    const float frac = static_cast<float>(position - static_cast<double>(i));
    const int ch = std::min(channel, sourceBuffer.getNumChannels() - 1);

    const auto sampleAt = [&](int idx) -> float
    {
        const int clamped = std::clamp(idx, 0, sourceBufferSize - 1);
        return sourceBuffer.getSample(ch, clamped);
    };

    return engine::audio::cubicHermite(sampleAt(i - 1), sampleAt(i), sampleAt(i + 1), sampleAt(i + 2), frac);
}

void HermiteResamplingAudioSource::handleDirectionFade(float* ch0, float* ch1, int numSamples)
{
    if (directionFadeRemaining <= 0)
        return;

    for (int i = 0; i < numSamples; ++i) {
        const float t = static_cast<float>(directionFadeRemaining) / static_cast<float>(kDirectionFadeSamples);
        const float gain = std::min(1.0f, t);
        if (ch0) ch0[i] *= gain;
        if (ch1) ch1[i] *= gain;
        directionFadeRemaining = std::max(0, directionFadeRemaining - 1);
    }
}

void HermiteResamplingAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (!input || !bufferToFill.buffer) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const double currentRatio = ratio.load(std::memory_order_relaxed);
    if (currentRatio * lastRatio < 0.0
            && std::abs(currentRatio) > 0.002
            && std::abs(lastRatio) > 0.002) {
        directionFadeRemaining = kDirectionFadeSamples;
    }
    lastRatio = currentRatio;

    const int outChannels = std::min(bufferToFill.buffer->getNumChannels(), channels);
    const int start = bufferToFill.startSample;
    const int numSamples = bufferToFill.numSamples;

    float* out0 = outChannels > 0 ? bufferToFill.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? bufferToFill.buffer->getWritePointer(1, start) : nullptr;

    const int blockAhead = static_cast<int>(std::ceil(std::abs(currentRatio) * static_cast<double>(numSamples))) + 12;
    ensureSourceBuffer(blockAhead);

    for (int i = 0; i < numSamples; ++i) {
        if (sourceBufferSize < static_cast<int>(std::ceil(sourceBufferPos)) + 8)
            ensureSourceBuffer(8);

        if (sourceBufferSize < 6) {
            if (out0) out0[i] = 0.0f;
            if (out1) out1[i] = 0.0f;
            continue;
        }

        const double delta = currentRatio - smoothedRatio;
        const double ratioSlew = std::max(0.0008,
            std::max(48.0, std::abs(delta) * 0.65 * outputSampleRate) / outputSampleRate);
        smoothedRatio += std::clamp(delta, -ratioSlew, ratioSlew);

        if (out0) out0[i] = readHermite(0, sourceBufferPos);
        if (out1) out1[i] = readHermite(1, sourceBufferPos);

        sourceBufferPos += smoothedRatio;
    }

    handleDirectionFade(out0, out1, numSamples);
}
