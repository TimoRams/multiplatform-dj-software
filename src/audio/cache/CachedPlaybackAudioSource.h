#pragma once

#include "audio/AudioRouting.h"
#include "AudioPageCache.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

struct PlaybackCacheStats {
    std::uint64_t pageHits = 0, pageMisses = 0, starvationBlocks = 0;
    std::uint64_t recoveryEvents = 0, droppedRequests = 0;
    std::uint64_t generationMismatches = 0;
    std::uint64_t diskReadsFromAudioThread = 0, decoderCallsFromAudioThread = 0;
};

class CachedPlaybackAudioSource final : public juce::PositionableAudioSource
{
public:
    CachedPlaybackAudioSource(AudioPageCache& cache, AudioCacheHandle handle) noexcept;
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept override;
    void setNextReadPosition(juce::int64 position) override;
    // Explicit control-thread seek.  During reverse the enclosing JUCE transport
    // still writes its forward bookkeeping position; that must not overwrite the
    // reader's reverse-progressing cursor.
    void setCommandedReadPosition(juce::int64 position) noexcept;
    // Control-thread cue/seek preparation. Requests the destination before the
    // next audio callback so a cold mid-track jump does not begin with silence.
    void prefetchForSeek(juce::int64 position) noexcept;
    [[nodiscard]] juce::int64 getNextReadPosition() const override;
    [[nodiscard]] juce::int64 getTotalLength() const override;
    [[nodiscard]] bool isLooping() const override;
    void setLooping(bool enabled) override;

    void setReverse(bool reverse) noexcept { m_reverse.store(reverse, std::memory_order_release); }
    void setLoopRangeSamples(juce::int64 in, juce::int64 out, double) noexcept;
    void clearLoopRangeSamples() noexcept;
    [[nodiscard]] PlaybackCacheStats cacheStats() const noexcept;

private:
    void requestReadAhead(juce::int64 position, int blockSize, bool reverse) noexcept;
    float nextGain(bool ready) noexcept;

    AudioPageCache& m_cache;
    AudioCacheHandle m_handle;
    std::atomic<juce::int64> m_position{0};
    std::atomic<bool> m_reverse{false}, m_loopEnabled{false};
    std::atomic<juce::int64> m_loopIn{0}, m_loopOut{0};
    float m_starvationGain = 0.0f;
    float m_lastL = 0.0f, m_lastR = 0.0f;
    static constexpr int kFadeSamples =
        AudioRoutingConstants::kCacheMissResumeCrossfadeMaxSamples;
    std::atomic<std::uint64_t> m_hits{0}, m_misses{0}, m_starvation{0}, m_recovery{0};
    std::atomic<std::uint64_t> m_dropped{0}, m_generationMismatch{0};
};
