#include "audio/HeadphoneBus.h"

#include <algorithm>
#include <cmath>

void HeadphoneBus::prepare(double sampleRate, int maximumBlockSize)
{
    m_limiter.prepare(std::max(1.0, sampleRate), maximumBlockSize, 2);
    m_limiter.setThreshold(0.90f);
    m_limiter.setCeiling(1.0f);
    m_limiter.setSaturationAmount(0.0f);
    m_limiter.setEnabled(true);
    m_gain = 1.0f;
}

void HeadphoneBus::mix(
    const std::array<const juce::AudioBuffer<float>*, kDeckCount>& pfl,
    const juce::AudioBuffer<float>& masterTap,
    const AudioParameters& parameters,
    juce::AudioBuffer<float>& output,
    int samples) noexcept
{
    output.clear(0, 0, samples);
    output.clear(1, 0, samples);

    const float mix = std::clamp(parameters.headphoneMix, 0.0f, 1.0f);
    const float cueGain = std::cos(mix * juce::MathConstants<float>::halfPi);
    const float masterGain = parameters.masterCueEnabled
        ? std::sin(mix * juce::MathConstants<float>::halfPi) : 0.0f;

    for (std::size_t deck = 0; deck < pfl.size(); ++deck) {
        const auto* source = pfl[deck];
        if (!parameters.pflEnabled[deck] || !source || source->getNumChannels() < 1
            || source->getNumSamples() < samples)
            continue;
        output.addFrom(0, 0, *source, 0, 0, samples, cueGain);
        output.addFrom(1, 0, *source, std::min(1, source->getNumChannels() - 1),
                       0, samples, cueGain);
    }

    if (masterGain > 0.0f && masterTap.getNumChannels() >= 2
        && masterTap.getNumSamples() >= samples) {
        output.addFrom(0, 0, masterTap, 0, 0, samples, masterGain);
        output.addFrom(1, 0, masterTap, 1, 0, samples, masterGain);
    }

    const float targetGain = std::clamp(parameters.headphoneGain, 0.0f, 2.0f);
    output.applyGainRamp(0, 0, samples, m_gain, targetGain);
    output.applyGainRamp(1, 0, samples, m_gain, targetGain);
    m_gain = targetGain;

    float* channels[2] { output.getWritePointer(0), output.getWritePointer(1) };
    m_limiter.processBlock(channels, 2, 0, samples);
}
