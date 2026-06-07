#include "ScratchResampler.hpp"

#include "HermiteKernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::audio {

void ScratchResampler::prepare(int numChannels, int maxBlockSize, double outputSampleRate)
{
    m_channels = std::max(1, numChannels);
    m_blockSize = std::max(64, maxBlockSize);
    m_outputSampleRate = std::max(1.0, outputSampleRate);
    m_sourceBuffer.setSize(m_channels, m_blockSize * 8, false, true, true);
    m_sourceBuffer.clear();
    m_sourceSize = 0;
    m_readPos = 0.0;
    m_lastRate = 0.0;
    m_smoothedRate = 0.0;
    m_directionFadeRemaining = 0;
}

void ScratchResampler::reset(double readPositionSamples) noexcept
{
    m_readPos = readPositionSamples;
    m_sourceSize = 0;
    m_lastRate = 0.0;
    m_smoothedRate = 0.0;
    m_directionFadeRemaining = 0;
}

void ScratchResampler::snapSmoothedRate(double rate) noexcept
{
    m_smoothedRate = rate;
    m_lastRate = rate;
}

double ScratchResampler::wrapPosition(double pos) const noexcept
{
    if (m_loopActive && m_loopOutSample > m_loopInSample + 1.0) {
        const double len = m_loopOutSample - m_loopInSample;
        double o = pos - m_loopInSample;
        o = std::fmod(o, len);
        if (o < 0.0)
            o += len;
        return m_loopInSample + o;
    }

    if (m_trackLengthSamples <= 1.0)
        return std::max(0.0, pos);

    return std::clamp(pos, 0.0, m_trackLengthSamples - 1.0);
}

void ScratchResampler::ensurePrefetch(juce::AudioSource& input, int minAhead) noexcept
{
    const int capacity = m_sourceBuffer.getNumSamples();
    if (capacity < 8)
        return;

    if (!std::isfinite(m_readPos))
        m_readPos = 0.0;
    m_readPos = std::clamp(m_readPos, 0.0, static_cast<double>(capacity - 1));

    const int needed = std::min(capacity - 1,
                                static_cast<int>(std::ceil(m_readPos)) + minAhead + 4);

    while (m_sourceSize < needed && m_sourceSize < capacity - m_blockSize) {
        const int pullStart = m_sourceSize;
        const int pullCount = std::min(m_blockSize, capacity - pullStart);
        if (pullCount <= 0)
            break;

        juce::AudioSourceChannelInfo pullInfo(&m_sourceBuffer, pullStart, pullCount);
        input.getNextAudioBlock(pullInfo);
        m_sourceSize += pullCount;
    }

    const int consume = std::clamp(static_cast<int>(std::floor(m_readPos)) - 2,
                                   0,
                                   std::max(0, m_sourceSize - 7));
    const int keep = m_sourceSize - consume;
    if (consume > 0 && keep > 6) {
        for (int ch = 0; ch < m_channels; ++ch) {
            float* dst = m_sourceBuffer.getWritePointer(ch);
            const float* src = m_sourceBuffer.getReadPointer(ch);
            std::memmove(dst, src + consume, static_cast<size_t>(keep) * sizeof(float));
        }
        m_sourceSize = keep;
        m_readPos -= static_cast<double>(consume);
    }
}

float ScratchResampler::readHermite(int channel, double position) const noexcept
{
    const int i = static_cast<int>(std::floor(position));
    const float frac = static_cast<float>(position - static_cast<double>(i));
    const int ch = std::min(channel, m_sourceBuffer.getNumChannels() - 1);

    const auto at = [&](int idx) -> float {
        const int clamped = std::clamp(idx, 0, m_sourceSize - 1);
        return m_sourceBuffer.getSample(ch, clamped);
    };

    return engine::audio::cubicHermite(at(i - 1), at(i), at(i + 1), at(i + 2), frac);
}

void ScratchResampler::applyDirectionFade(float* ch0, float* ch1, int numSamples) noexcept
{
    if (m_directionFadeRemaining <= 0)
        return;

    for (int i = 0; i < numSamples; ++i) {
        const float t = static_cast<float>(m_directionFadeRemaining) / static_cast<float>(kDirectionFadeSamples);
        const float gain = 1.0f - std::min(1.0f, t);
        if (ch0) ch0[i] *= gain;
        if (ch1) ch1[i] *= gain;
        m_directionFadeRemaining = std::max(0, m_directionFadeRemaining - 1);
    }
}

void ScratchResampler::processBlock(juce::AudioSource& input,
                                    double rate,
                                    const juce::AudioSourceChannelInfo& output) noexcept
{
    if (!output.buffer || output.numSamples <= 0) {
        if (output.buffer)
            output.clearActiveBufferRegion();
        return;
    }

    const int start = output.startSample;
    const int numSamples = output.numSamples;
    if (start < 0 || start + numSamples > output.buffer->getNumSamples()) {
        output.clearActiveBufferRegion();
        return;
    }

    if (auto* positionable = dynamic_cast<juce::PositionableAudioSource*>(&input)) {
        const auto targetPos = static_cast<juce::int64>(std::llround(std::max(0.0, m_readPos)));
        const auto sourcePos = positionable->getNextReadPosition();
        if (std::abs(static_cast<double>(sourcePos - targetPos)) > 4.0) {
            positionable->setNextReadPosition(targetPos);
            m_sourceSize = 0;
            m_readPos = static_cast<double>(targetPos);
        }
    }

    if (std::abs(rate) < 1e-7) {
        m_smoothedRate = 0.0;
        m_lastRate = 0.0;
    } else {
        if (rate * m_lastRate < 0.0 && std::abs(rate) > 1e-5 && std::abs(m_lastRate) > 1e-5)
            m_directionFadeRemaining = kDirectionFadeSamples;
        m_lastRate = rate;
    }

    const int outChannels = std::min(output.buffer->getNumChannels(), m_channels);

    float* out0 = outChannels > 0 ? output.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? output.buffer->getWritePointer(1, start) : nullptr;

    const int blockAhead = static_cast<int>(std::ceil(std::abs(rate) * static_cast<double>(numSamples))) + 12;
    ensurePrefetch(input, blockAhead);

    for (int i = 0; i < numSamples; ++i) {
        const double delta = rate - m_smoothedRate;
        const double slew = std::max(0.002,
            std::max(24.0, std::abs(delta) * 0.90 * m_outputSampleRate) / m_outputSampleRate);
        m_smoothedRate += std::clamp(delta, -slew, slew);

        if (m_sourceSize < static_cast<int>(std::ceil(m_readPos)) + 8)
            ensurePrefetch(input, 8);

        if (m_sourceSize < 6) {
            if (out0) out0[i] = 0.0f;
            if (out1) out1[i] = 0.0f;
            continue;
        }

        if (std::abs(m_smoothedRate) < 1e-7) {
            if (out0) out0[i] = 0.0f;
            if (out1) out1[i] = 0.0f;
            continue;
        }

        m_readPos = wrapPosition(m_readPos);

        if (out0) out0[i] = readHermite(0, m_readPos);
        if (out1) out1[i] = readHermite(1, m_readPos);

        m_readPos += m_smoothedRate;
    }

    applyDirectionFade(out0, out1, numSamples);
}

} // namespace engine::audio
