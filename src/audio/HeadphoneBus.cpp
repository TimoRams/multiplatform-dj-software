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
    m_sampleRate = std::max(1.0, sampleRate);
    m_gain = 1.0f;
    m_deckGain.fill(0.0f);
    m_masterTapGain = 0.0f;
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

    // Every contribution slews toward its target over ~5 ms instead of stepping
    // at the block boundary. Pressing a channel CUE button, switching MASTER CUE
    // or sweeping the CUE MIX knob otherwise puts a discontinuity straight into
    // the headphone feed, which is heard as a click.
    const float step = 1.0f / static_cast<float>(
        std::max(1, static_cast<int>(m_sampleRate * 0.005)));

    float* outputL = output.getWritePointer(0);
    float* outputR = output.getWritePointer(1);

    const auto addSlewed = [&](const float* sourceL, const float* sourceR,
                               float target, float& gainState) noexcept {
        float gain = gainState;
        for (int sample = 0; sample < samples; ++sample) {
            gain = std::clamp(target, gain - step, gain + step);
            outputL[sample] += sourceL[sample] * gain;
            outputR[sample] += sourceR[sample] * gain;
        }
        gainState = gain;
    };

    for (std::size_t deck = 0; deck < pfl.size(); ++deck) {
        const auto* source = pfl[deck];
        if (!source || source->getNumChannels() < 1 || source->getNumSamples() < samples) {
            // No usable audio this block — there is nothing to fade out through,
            // so the next block has to start from silence.
            m_deckGain[deck] = 0.0f;
            continue;
        }

        const float target = parameters.pflEnabled[deck] ? cueGain : 0.0f;
        if (target <= 0.0f && m_deckGain[deck] <= 0.0f)
            continue;

        const int right = std::min(1, source->getNumChannels() - 1);
        addSlewed(source->getReadPointer(0), source->getReadPointer(right),
                  target, m_deckGain[deck]);
    }

    if (masterTap.getNumChannels() >= 2 && masterTap.getNumSamples() >= samples) {
        if (masterGain > 0.0f || m_masterTapGain > 0.0f)
            addSlewed(masterTap.getReadPointer(0), masterTap.getReadPointer(1),
                      masterGain, m_masterTapGain);
    } else {
        m_masterTapGain = 0.0f;
    }

    const float targetGain = std::clamp(parameters.headphoneGain, 0.0f, 2.0f);
    output.applyGainRamp(0, 0, samples, m_gain, targetGain);
    output.applyGainRamp(1, 0, samples, m_gain, targetGain);
    m_gain = targetGain;

    float* channels[2] { output.getWritePointer(0), output.getWritePointer(1) };
    m_limiter.processBlock(channels, 2, 0, samples);
}
