#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

class IDeckAudioEndpoint : public juce::AudioSource {
public:
    ~IDeckAudioEndpoint() override = default;

    [[nodiscard]] virtual const juce::AudioBuffer<float>& preFaderBuffer() const noexcept = 0;
    [[nodiscard]] virtual bool cueEnabledForMix() const noexcept = 0;
    virtual void setCueEnabledForMix(bool enabled) noexcept = 0;
};

class IMasterBusAuxEndpoint {
public:
    virtual ~IMasterBusAuxEndpoint() = default;

    virtual void prepareAuxAudio(int maximumBlockSize, double sampleRate) = 0;
    virtual void releaseAuxAudio() = 0;
    virtual void mixAuxAudio(juce::AudioBuffer<float>& masterBuffer,
                             juce::AudioBuffer<float>& scratchBuffer,
                             int numberOfSamples) noexcept = 0;
};
