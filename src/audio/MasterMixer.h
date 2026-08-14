#pragma once

#include "audio/AudioParameters.h"
#include "audio/AudioRouting.h"
#include "audio/internal/BrickwallLimiter.h"
#include "fx/FxProcessor.h"

#include <array>
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
    [[nodiscard]] int limiterLatencySamples() const noexcept
    { return m_limiter.getLookaheadSamples(); }

private:
    static float crossfaderTarget(float position,
                                  CrossfaderAssignment assignment,
                                  CrossfaderCurve curve) noexcept;

    BrickwallLimiter m_limiter;
    FxProcessor m_masterFx;
    std::array<float, kDeckCount> m_crossfaderGain { 1.0f, 1.0f, 1.0f, 1.0f };
    float m_masterGain = 1.0f;
    double m_sampleRate = 48000.0;
    MasterMeterSnapshot m_meter {};
};
