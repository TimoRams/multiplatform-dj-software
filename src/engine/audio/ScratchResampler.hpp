#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

namespace engine::audio {

// RT-safe Hermite scratch reader. Supports negative speed and fractional positions.
// No heap allocation after prepareToPlay().
class ScratchResampler {
public:
    void prepare(int numChannels, int maxBlockSize, double outputSampleRate);
    void reset(double readPositionSamples) noexcept;
    void snapSmoothedRate(double rate) noexcept;
    void invalidatePrefetch() noexcept { m_sourceSize = 0; }

    void setTrackLengthSamples(double lengthSamples) noexcept {
        m_trackLengthSamples = std::max(0.0, lengthSamples);
    }

    void setLoopRange(double loopInSample, double loopOutSample, bool active) noexcept
    {
        m_loopInSample = loopInSample;
        m_loopOutSample = loopOutSample;
        m_loopActive = active;
    }

    // rate: source samples advanced per output sample (negative = reverse)
    void processBlock(juce::AudioSource& input,
                      double rate,
                      const juce::AudioSourceChannelInfo& output) noexcept;

    [[nodiscard]] double readPosition() const noexcept { return m_readPos; }

private:
    void ensurePrefetch(juce::AudioSource& input, int minAhead) noexcept;
    float readHermite(int channel, double position) const noexcept;
    double wrapPosition(double pos) const noexcept;
    void applyDirectionFade(float* ch0, float* ch1, int numSamples) noexcept;

    juce::AudioBuffer<float> m_sourceBuffer;
    int m_channels = 2;
    int m_blockSize = 512;
    double m_outputSampleRate = 44100.0;
    double m_trackLengthSamples = 0.0;

    int m_sourceSize = 0;
    double m_readPos = 0.0;
    double m_lastRate = 0.0;
    double m_smoothedRate = 0.0;

    bool m_loopActive = false;
    double m_loopInSample = 0.0;
    double m_loopOutSample = 0.0;

    int m_directionFadeRemaining = 0;
    static constexpr int kDirectionFadeSamples = 64;
};

} // namespace engine::audio
