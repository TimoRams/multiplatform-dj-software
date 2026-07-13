#pragma once

#include "audio/cache/AudioCacheHandle.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

class AudioPageCache;
class CachedPlaybackAudioSource;
class MixerDspSource;
class TimeStretchAudioSource;
namespace juce { class AudioTransportSource; }
namespace engine::audio { class ScratchDeckBridge; }

class DeckAudioGraph final : public juce::AudioSource {
public:
    struct RealtimeStats {
        std::uint64_t diskReadsFromAudioThread = 0;
        std::uint64_t decoderCallsFromAudioThread = 0;
        std::uint64_t prepareCallsFromAudioThread = 0;
        std::uint64_t resetCallsFromAudioThread = 0;
        std::uint64_t prewarmCallsFromAudioThread = 0;
        std::uint64_t coefficientBuildsFromAudioThread = 0;
        std::uint64_t bufferGrowthsFromAudioThread = 0;
        std::uint64_t blockingLockAttempts = 0;
        std::uint64_t objectConstructionsFromAudioThread = 0;
        std::uint64_t trackGeneration = 0;
    };

    struct PreparedTrack {
        AudioCacheHandle cacheHandle;
        double sourceSampleRate=0.0;
        std::uint64_t trackGeneration=0;
    };

    explicit DeckAudioGraph(AudioPageCache& cache);
    ~DeckAudioGraph() override;
    DeckAudioGraph(const DeckAudioGraph&)=delete;
    DeckAudioGraph& operator=(const DeckAudioGraph&)=delete;

    void prepareToPlay(int maximumBlockSize,double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept override;

    void installPreparedTrack(PreparedTrack track);
    void clearTrack(std::uint64_t invalidThroughGeneration=0);
    void setAudioPlayheadSink(std::atomic<double>* sink) noexcept;
    [[nodiscard]] RealtimeStats realtimeStats() const noexcept;

    [[nodiscard]] juce::AudioTransportSource& transport() noexcept;
    [[nodiscard]] const juce::AudioTransportSource& transport() const noexcept;
    [[nodiscard]] CachedPlaybackAudioSource* playback() noexcept;
    [[nodiscard]] const CachedPlaybackAudioSource* playback() const noexcept;
    [[nodiscard]] engine::audio::ScratchDeckBridge& scratch() noexcept;
    [[nodiscard]] const engine::audio::ScratchDeckBridge& scratch() const noexcept;
    [[nodiscard]] engine::audio::ScratchDeckBridge* scratchPtr() noexcept;
    [[nodiscard]] const engine::audio::ScratchDeckBridge* scratchPtr() const noexcept;
    [[nodiscard]] TimeStretchAudioSource& timeStretch() noexcept;
    [[nodiscard]] const TimeStretchAudioSource& timeStretch() const noexcept;
    [[nodiscard]] TimeStretchAudioSource* timeStretchPtr() noexcept;
    [[nodiscard]] const TimeStretchAudioSource* timeStretchPtr() const noexcept;
    [[nodiscard]] MixerDspSource& mixer() noexcept;
    [[nodiscard]] const MixerDspSource& mixer() const noexcept;
    [[nodiscard]] MixerDspSource* mixerPtr() noexcept;
    [[nodiscard]] const MixerDspSource* mixerPtr() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
