#pragma once

#include "audio/AudioParameters.h"
#include "audio/AudioRouting.h"

#include <juce_audio_basics/juce_audio_basics.h>

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
