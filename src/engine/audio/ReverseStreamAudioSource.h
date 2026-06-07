#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cmath>
#include <limits>

// Positionable reader with reverse playback, loop wrapping, and direction crossfades.
class ReverseStreamAudioSource : public juce::PositionableAudioSource {
public:
    ReverseStreamAudioSource(juce::PositionableAudioSource* forwardSource,
                             juce::PositionableAudioSource* directSource);

    void setLoopRangeSamples(juce::int64 loopInSample, juce::int64 loopOutSample, double sampleRate);
    void clearLoopRangeSamples();
    void setReverse(bool rev);

    void setNextReadPosition(juce::int64 newPosition) override;
    juce::int64 getNextReadPosition() const override;
    juce::int64 getTotalLength() const override;

    bool isLooping() const override;
    void setLooping(bool shouldLoop) override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

private:
    juce::PositionableAudioSource* forwardPlaybackSource() const;
    juce::PositionableAudioSource* randomAccessSource() const;

    void applyFadeInToRange(juce::AudioBuffer<float>* buffer,
                            int startSample,
                            int count,
                            int numChannels);
    void applyFadeOutToTail(juce::AudioBuffer<float>* buffer,
                            int chunkStart,
                            int chunkLen,
                            int numChannels);
    void applyDirectionFadeInToRange(juce::AudioBuffer<float>* buffer,
                                     int startSample,
                                     int count,
                                     int numChannels);
    void getLoopedForwardAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill);
    juce::int64 findNearestZeroCrossingUnsafe(juce::int64 approx,
                                              int radius,
                                              juce::int64 totalLength);

    juce::PositionableAudioSource* m_forwardSource;
    juce::PositionableAudioSource* m_directSource;
    std::atomic<bool> m_reverse { false };
    juce::int64 m_logicalPos { 0 };
    juce::SpinLock m_stateLock;
    bool m_loopEnabled { false };
    juce::int64 m_loopInSample { 0 };
    juce::int64 m_loopOutSample { 0 };
    double m_sampleRate { 44100.0 };
    int m_windowSamples { 64 };
    int m_pendingFadeInSamples { 0 };
    int m_pendingDirectionFadeInSamples { 0 };
};
