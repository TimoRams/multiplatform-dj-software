#pragma once

#include "audio/AudioParameters.h"
#include "audio/AudioRouting.h"
#include "audio/internal/BrickwallLimiter.h"

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
    double m_sampleRate = 48'000.0;
    float m_gain = 1.0f;
    // Every contribution to the cue bus is ramped across the block. Switching a
    // channel's CUE button or moving the CUE MIX knob otherwise steps the gain
    // at the block boundary, which is audible as a click in the headphones.
    std::array<float, kDeckCount> m_deckGain {};
    float m_masterTapGain = 0.0f;
};
