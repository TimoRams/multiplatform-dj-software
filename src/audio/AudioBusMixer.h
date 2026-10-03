#pragma once

#include "audio/AudioParameters.h"
#include "audio/internal/BrickwallLimiter.h"
#include "fx/FxProcessor.h"

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>

struct MasterMeterSnapshot {
    float preLimiterPeakL = 0.0f;
    float preLimiterPeakR = 0.0f;
    float finalPeakL = 0.0f;
    float finalPeakR = 0.0f;
    float minimumGainReduction = 1.0f;
};

class MasterMixer final {
public:
    static constexpr std::size_t kDeckCount = 4;

    void prepare(double sampleRate, int maximumBlockSize);
    void reset() noexcept;

    void mixPrograms(const std::array<const juce::AudioBuffer<float>*, kDeckCount>& programs,
                     const std::array<const juce::AudioBuffer<float>*, kDeckCount>& tailReturns,
                     const AudioParameters& parameters,
                     juce::AudioBuffer<float>& master,
                     int samples) noexcept;
    void finalize(const AudioParameters& parameters,
                  juce::AudioBuffer<float>& master,
                  int samples,
                  juce::AudioBuffer<float>* preMasterGainTap = nullptr) noexcept;

    [[nodiscard]] const MasterMeterSnapshot& meter() const noexcept { return m_meter; }
    [[nodiscard]] float crossfaderGain(std::size_t deck) const noexcept
    {
        return deck < kDeckCount
            ? m_publishedCrossfaderGain[deck].load(std::memory_order_acquire) : 0.0f;
    }
    [[nodiscard]] int limiterLatencySamples() const noexcept
    { return m_limiter.getLookaheadSamples(); }

private:
    static float crossfaderTarget(float position,
                                  CrossfaderAssignment assignment,
                                  CrossfaderCurve curve) noexcept;

    BrickwallLimiter m_limiter;
    FxProcessor m_masterFx;
    std::array<float, kDeckCount> m_crossfaderGain { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<std::atomic<float>, kDeckCount> m_publishedCrossfaderGain {};
    float m_masterGain = 1.0f;
    double m_sampleRate = 48000.0;
    MasterMeterSnapshot m_meter {};
};

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
    // at the block boundary, which is audible as a click.
    std::array<float, kDeckCount> m_deckGain {};
    float m_masterTapGain = 0.0f;
};

class AudioOutputRouter final {
public:
    void write(const juce::AudioBuffer<float>& masterTap,
               const juce::AudioBuffer<float>& headphones,
               const AudioParameters& parameters,
               juce::AudioBuffer<float>& hardwareOutput,
               int outputStart,
               int samples) const noexcept;

private:
    static void writeStereo(const juce::AudioBuffer<float>& source,
                            juce::AudioBuffer<float>& destination,
                            int destinationStart,
                            int samples,
                            int firstPhysicalChannel) noexcept;
};
