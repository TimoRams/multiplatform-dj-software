#pragma once

#include <cstdint>
#include <juce_audio_basics/juce_audio_basics.h>

class TimeStretchReadAheadSource {
public:
    virtual ~TimeStretchReadAheadSource() = default;
    virtual bool readAhead(int offset, juce::AudioBuffer<float>& destination,
                           int count) noexcept = 0;
    virtual double readAheadTempo(int) noexcept { return 0.0; }
    virtual double readAheadPosition() noexcept { return 0.0; }
    virtual std::uint64_t readAheadTransportGeneration() const noexcept { return 0; }
    virtual bool readAheadPlaybackActive() const noexcept { return true; }
    virtual bool readAheadScratchActive() const noexcept { return false; }
};
