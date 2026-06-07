#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

// High-quality scratch resampler: cubic Hermite interpolation, per-sample ratio,
// micro-fade on playback-direction reversals.
class HermiteResamplingAudioSource : public juce::AudioSource
{
public:
    explicit HermiteResamplingAudioSource(juce::AudioSource* input,
                                          bool deleteInputWhenDeleted = false,
                                          int numChannels = 2);

    void setResamplingRatio(double samplesInPerOutputSample);
    double getResamplingRatio() const;
    void snapSmoothedRatio();
    void armDirectionCrossfade();
    void resetStream();

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

private:
    void ensureSourceBuffer(int minSamples);
    float readHermite(int channel, double position) const;
    void handleDirectionFade(float* ch0, float* ch1, int numSamples);

    juce::OptionalScopedPointer<juce::AudioSource> input;
    const int channels;

    juce::AudioBuffer<float> sourceBuffer;
    int sourceBufferSize = 0;
    double sourceBufferPos = 0.0;

    std::atomic<double> ratio { 1.0 };
    double lastRatio = 1.0;
    double smoothedRatio = 1.0;
    int directionFadeRemaining = 0;
    static constexpr int kDirectionFadeSamples = 64;

    double outputSampleRate = 44100.0;
    int blockSize = 512;
};
