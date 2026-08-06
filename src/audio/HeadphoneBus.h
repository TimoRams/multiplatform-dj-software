#pragma once

#include "audio/AudioParameters.h"
#include "audio/AudioRouting.h"
#include "fx/BrickwallLimiter.h"

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>

class HeadphoneBus final {
public:
    static constexpr std::size_t kDeckCount = 4;

    void prepare(double sampleRate, int maximumBlockSize);
    void mix(const std::array<const juce::AudioBuffer<float>*, kDeckCount>& pfl,
             const juce::AudioBuffer<float>& masterTap,
             const AudioParameters& parameters,
             juce::AudioBuffer<float>& output,
             int samples) noexcept;

private:
    BrickwallLimiter m_limiter;
    float m_gain = 1.0f;
};
