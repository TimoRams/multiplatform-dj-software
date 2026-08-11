#pragma once

#include "audio/AudioParameters.h"
#include "audio/cache/AudioCacheTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

class AudioPageCache;
class CachedPlaybackAudioSource;
class DeckChannelProcessor;
class TimeStretchProcessor;
enum class TimeStretchBackend : std::uint8_t;
namespace juce { class AudioTransportSource; }
namespace engine::audio { class RenderModeRouter; }

class DeckAudioPipeline final : public juce::AudioSource {
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
        std::uint64_t droppedCommands = 0;
        std::uint64_t trackGeneration = 0;
    };

    struct PreparedTrack {
        AudioCacheHandle cacheHandle;
        double sourceSampleRate=0.0;
        std::uint64_t trackGeneration=0;
    };

    struct TransportSnapshot {
        bool hasTrack = false;
        bool running = false;
        double positionSeconds = 0.0;
        double lengthSeconds = 0.0;
        std::uint64_t trackGeneration = 0;
    };

    explicit DeckAudioPipeline(AudioPageCache& cache);
    ~DeckAudioPipeline() override;
    DeckAudioPipeline(const DeckAudioPipeline&)=delete;
    DeckAudioPipeline& operator=(const DeckAudioPipeline&)=delete;

    void prepareToPlay(int maximumBlockSize,double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept override;
    [[nodiscard]] const juce::AudioBuffer<float>& preFaderBuffer() const noexcept;
    [[nodiscard]] const juce::AudioBuffer<float>& postFaderTailBuffer() const noexcept;

    void installPreparedTrack(PreparedTrack track);
    void clearTrack(std::uint64_t invalidThroughGeneration=0);
    void setAudioPlayheadSink(std::atomic<double>* sink) noexcept;
    void setTransportRunning(bool running) noexcept;
    void seekToSeconds(double seconds) noexcept;
    void setReverse(bool enabled) noexcept;
    void setPlaybackRate(double rate) noexcept;
    void setJogNudgeRatio(double ratio) noexcept;
    void setKeylockEnabled(bool enabled) noexcept;
    void setTimeStretchBackend(TimeStretchBackend backend) noexcept;
    void setLoopRangeSeconds(double startSeconds, double endSeconds, bool active,
                             double sourceSampleRate) noexcept;
    void setPlaybackReadPositionSamples(std::int64_t position) noexcept;
    [[nodiscard]] int keylockLatencySamples() const noexcept;
    [[nodiscard]] TransportSnapshot transportSnapshot() const noexcept;
    [[nodiscard]] RealtimeStats realtimeStats() const noexcept;

    [[nodiscard]] juce::AudioTransportSource& transport() noexcept;
    [[nodiscard]] const juce::AudioTransportSource& transport() const noexcept;
    [[nodiscard]] CachedPlaybackAudioSource* playback() noexcept;
    [[nodiscard]] const CachedPlaybackAudioSource* playback() const noexcept;
    [[nodiscard]] engine::audio::RenderModeRouter& renderModeRouter() noexcept;
    [[nodiscard]] const engine::audio::RenderModeRouter& renderModeRouter() const noexcept;
    [[nodiscard]] engine::audio::RenderModeRouter* renderModeRouterPtr() noexcept;
    [[nodiscard]] const engine::audio::RenderModeRouter* renderModeRouterPtr() const noexcept;
    [[nodiscard]] TimeStretchProcessor& timeStretch() noexcept;
    [[nodiscard]] const TimeStretchProcessor& timeStretch() const noexcept;
    [[nodiscard]] TimeStretchProcessor* timeStretchPtr() noexcept;
    [[nodiscard]] const TimeStretchProcessor* timeStretchPtr() const noexcept;
    [[nodiscard]] DeckChannelProcessor& mixer() noexcept;
    [[nodiscard]] const DeckChannelProcessor& mixer() const noexcept;
    [[nodiscard]] DeckChannelProcessor* mixerPtr() noexcept;
    [[nodiscard]] const DeckChannelProcessor* mixerPtr() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
