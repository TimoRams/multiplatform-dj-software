#include "DeckAudioGraph.h"

#include "audio/cache/AudioPageCache.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "engine/audio/MixerDspSource.h"
#include "engine/audio/ScratchDeckBridge.hpp"
#include "engine/audio/TimeStretchAudioSource.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>

struct DeckAudioGraph::Impl {
    explicit Impl(AudioPageCache& pageCache) : cache(pageCache)
    {
        scratch = std::make_unique<engine::audio::ScratchDeckBridge>(&transport, false);
        timeStretch = std::make_unique<TimeStretchAudioSource>(scratch.get());
        mixer = std::make_unique<MixerDspSource>(timeStretch.get());
    }
    AudioPageCache& cache;
    AudioCacheHandle handle;
    std::unique_ptr<CachedPlaybackAudioSource> playback;
    juce::AudioTransportSource transport;
    std::unique_ptr<engine::audio::ScratchDeckBridge> scratch;
    std::unique_ptr<TimeStretchAudioSource> timeStretch;
    std::unique_ptr<MixerDspSource> mixer;
    std::uint64_t trackGeneration = 0;
    std::uint64_t retiredDiskReadsFromAudioThread = 0;
    std::uint64_t retiredDecoderCallsFromAudioThread = 0;
};

DeckAudioGraph::DeckAudioGraph(AudioPageCache& cache)
    : m_impl(std::make_unique<Impl>(cache))
{
}

DeckAudioGraph::~DeckAudioGraph()
{
    clearTrack();
}

void DeckAudioGraph::prepareToPlay(int block, double rate)
{
    m_impl->mixer->prepareToPlay(block, rate);
}

void DeckAudioGraph::releaseResources()
{
    m_impl->mixer->releaseResources();
}

void DeckAudioGraph::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept
{
    m_impl->mixer->getNextAudioBlock(info);
}

void DeckAudioGraph::setAudioPlayheadSink(std::atomic<double>* sink) noexcept
{
    m_impl->scratch->setAudioPlayheadSink(sink);
}

void DeckAudioGraph::installPreparedTrack(PreparedTrack track)
{
    if (track.trackGeneration <= m_impl->trackGeneration || track.sourceSampleRate <= 0.0) {
        m_impl->cache.releaseTrack(track.cacheHandle);
        return;
    }

    m_impl->scratch->beginTransportSwap();
    m_impl->transport.stop();
    m_impl->transport.setSource(nullptr);
    if (m_impl->playback) {
        const auto stats = m_impl->playback->cacheStats();
        m_impl->retiredDiskReadsFromAudioThread += stats.diskReadsFromAudioThread;
        m_impl->retiredDecoderCallsFromAudioThread += stats.decoderCallsFromAudioThread;
    }
    m_impl->playback.reset();
    m_impl->cache.releaseTrack(m_impl->handle);

    m_impl->handle = track.cacheHandle;
    m_impl->playback = std::make_unique<CachedPlaybackAudioSource>(m_impl->cache, m_impl->handle);
    m_impl->trackGeneration = track.trackGeneration;
    m_impl->transport.setSource(m_impl->playback.get(), 0, nullptr, track.sourceSampleRate);
    m_impl->timeStretch->setTrackGeneration(track.trackGeneration);
    m_impl->scratch->setTrackCacheSource(&m_impl->cache, m_impl->handle);
    m_impl->transport.setPosition(0.0);
    m_impl->scratch->endTransportSwap();
}

void DeckAudioGraph::clearTrack(std::uint64_t invalidThroughGeneration)
{
    if (!m_impl)
        return;

    m_impl->trackGeneration = std::max(m_impl->trackGeneration, invalidThroughGeneration);
    m_impl->scratch->beginTransportSwap();
    m_impl->transport.stop();
    m_impl->transport.setSource(nullptr);
    m_impl->scratch->setTrackCacheSource(nullptr, {});
    if (m_impl->playback) {
        const auto stats = m_impl->playback->cacheStats();
        m_impl->retiredDiskReadsFromAudioThread += stats.diskReadsFromAudioThread;
        m_impl->retiredDecoderCallsFromAudioThread += stats.decoderCallsFromAudioThread;
    }
    m_impl->playback.reset();
    m_impl->cache.releaseTrack(m_impl->handle);
    m_impl->handle = {};
    m_impl->scratch->endTransportSwap();
}

DeckAudioGraph::RealtimeStats DeckAudioGraph::realtimeStats() const noexcept
{
    RealtimeStats result;
    result.diskReadsFromAudioThread = m_impl->retiredDiskReadsFromAudioThread;
    result.decoderCallsFromAudioThread = m_impl->retiredDecoderCallsFromAudioThread;
    if (m_impl->playback) {
        const auto playback = m_impl->playback->cacheStats();
        result.diskReadsFromAudioThread += playback.diskReadsFromAudioThread;
        result.decoderCallsFromAudioThread += playback.decoderCallsFromAudioThread;
    }
    result.diskReadsFromAudioThread += m_impl->scratch->scratchCacheStats().diskReadsFromAudioThread;

    const auto stretch = m_impl->timeStretch->realtimeStats();
    result.prepareCallsFromAudioThread = stretch.prepareCallsFromAudioThread;
    result.resetCallsFromAudioThread = stretch.resetCallsFromAudioThread;
    result.prewarmCallsFromAudioThread = stretch.prewarmCallsFromAudioThread;
    result.bufferGrowthsFromAudioThread = stretch.bufferGrowthsFromAudioThread;
    result.blockingLockAttempts = stretch.blockingLockAttempts;

    const auto mixer = m_impl->mixer->realtimeStats();
    result.coefficientBuildsFromAudioThread = mixer.coefficientBuildsFromAudioThread;
    result.prepareCallsFromAudioThread += mixer.prepareCallsFromAudioThread;
    result.bufferGrowthsFromAudioThread += mixer.bufferGrowthsFromAudioThread;
    result.blockingLockAttempts += mixer.blockingLockAttempts;
    result.objectConstructionsFromAudioThread = mixer.objectConstructionsFromAudioThread;
    result.trackGeneration = m_impl->trackGeneration;
    return result;
}

juce::AudioTransportSource& DeckAudioGraph::transport() noexcept { return m_impl->transport; }
const juce::AudioTransportSource& DeckAudioGraph::transport() const noexcept { return m_impl->transport; }
CachedPlaybackAudioSource* DeckAudioGraph::playback() noexcept { return m_impl->playback.get(); }
const CachedPlaybackAudioSource* DeckAudioGraph::playback() const noexcept { return m_impl->playback.get(); }
engine::audio::ScratchDeckBridge& DeckAudioGraph::scratch() noexcept { return *m_impl->scratch; }
const engine::audio::ScratchDeckBridge& DeckAudioGraph::scratch() const noexcept { return *m_impl->scratch; }
engine::audio::ScratchDeckBridge* DeckAudioGraph::scratchPtr() noexcept { return m_impl->scratch.get(); }
const engine::audio::ScratchDeckBridge* DeckAudioGraph::scratchPtr() const noexcept { return m_impl->scratch.get(); }
TimeStretchAudioSource& DeckAudioGraph::timeStretch() noexcept { return *m_impl->timeStretch; }
const TimeStretchAudioSource& DeckAudioGraph::timeStretch() const noexcept { return *m_impl->timeStretch; }
TimeStretchAudioSource* DeckAudioGraph::timeStretchPtr() noexcept { return m_impl->timeStretch.get(); }
const TimeStretchAudioSource* DeckAudioGraph::timeStretchPtr() const noexcept { return m_impl->timeStretch.get(); }
MixerDspSource& DeckAudioGraph::mixer() noexcept { return *m_impl->mixer; }
const MixerDspSource& DeckAudioGraph::mixer() const noexcept { return *m_impl->mixer; }
MixerDspSource* DeckAudioGraph::mixerPtr() noexcept { return m_impl->mixer.get(); }
const MixerDspSource* DeckAudioGraph::mixerPtr() const noexcept { return m_impl->mixer.get(); }
