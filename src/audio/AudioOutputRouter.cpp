#include "audio/AudioOutputRouter.h"

#include <algorithm>
#include <cmath>

void AudioOutputRouter::write(const juce::AudioBuffer<float>& masterTap,
                              const juce::AudioBuffer<float>& headphones,
                              const AudioParameters& parameters,
                              juce::AudioBuffer<float>& hardwareOutput,
                              int outputStart,
                              int samples) const noexcept
{
    // Booth intentionally follows the canonical post-limiter Master Tap and
    // therefore the software Master Gain. A separate booth level is hardware-only.
    writeStereo(masterTap, hardwareOutput, outputStart, samples,
                parameters.masterFirstChannel);
    writeStereo(masterTap, hardwareOutput, outputStart, samples,
                parameters.boothFirstChannel);
    writeStereo(headphones, hardwareOutput, outputStart, samples,
                parameters.headphonesFirstChannel);
}

void AudioOutputRouter::writeStereo(const juce::AudioBuffer<float>& source,
                                    juce::AudioBuffer<float>& destination,
                                    int destinationStart,
                                    int samples,
                                    int firstPhysicalChannel) noexcept
{
    if (firstPhysicalChannel < 1 || source.getNumChannels() < 1
        || source.getNumSamples() < samples)
        return;
    const int left = firstPhysicalChannel - 1;
    const int right = left + 1;
    if (right >= destination.getNumChannels() || destinationStart < 0
        || destinationStart + samples > destination.getNumSamples())
        return; // Missing/unavailable bus is silent. There is no fallback route.

    const float* sourceL = source.getReadPointer(0);
    const float* sourceR = source.getReadPointer(std::min(1, source.getNumChannels() - 1));
    float* destinationL = destination.getWritePointer(left, destinationStart);
    float* destinationR = destination.getWritePointer(right, destinationStart);
    for (int sample = 0; sample < samples; ++sample) {
        destinationL[sample] = std::isfinite(sourceL[sample]) ? sourceL[sample] : 0.0f;
        destinationR[sample] = std::isfinite(sourceR[sample]) ? sourceR[sample] : 0.0f;
    }
}
