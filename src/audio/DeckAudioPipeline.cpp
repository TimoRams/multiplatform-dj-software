#include "DeckAudioPipeline.h"

#include "audio/cache/AudioPageCache.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "audio/DeckChannelProcessor.h"
#include "audio/RenderModeRouter.h"
#include "audio/TimeStretchProcessor.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>
#include <thread>

struct DeckAudioPipeline::Impl {
    explicit Impl(AudioPageCache& pageCache) : cache(pageCache)
    {
        renderModeRouter = std::make_unique<engine::audio::RenderModeRouter>(&transport, false);
        timeStretch = std::make_unique<TimeStretchProcessor>(renderModeRouter.get());
        mixer = std::make_unique<DeckChannelProcessor>(timeStretch.get());
    }
    AudioPageCache& cache;
    AudioCacheHandle handle;
    std::unique_ptr<CachedPlaybackAudioSource> playback;
    juce::AudioTransportSource transport;
    std::unique_ptr<engine::audio::RenderModeRouter> renderModeRouter;
    std::unique_ptr<TimeStretchProcessor> timeStretch;
    std::unique_ptr<DeckChannelProcessor> mixer;
    std::uint64_t trackGeneration = 0;
    std::uint64_t retiredDiskReadsFromAudioThread = 0;
    std::uint64_t retiredDecoderCallsFromAudioThread = 0;
    std::atomic<bool> transportRequestedRunning { false };
    std::atomic<double> commandedPositionSeconds { 0.0 };
    std::atomic<std::uint64_t> seekGeneration { 0 };
    std::atomic<std::uint64_t> appliedSeekGeneration { 0 };
    // This gate owns the lifetime boundary for the complete deck callback,
    // including command consumption.  Keeping the gate in RenderModeRouter
    // left a window where consumeCommands() could still dereference playback
    // while the control thread retired it during a track handover.
    std::atomic<bool> trackSwapInProgress { false };
    std::atomic<unsigned int> audioCallbacksActive { 0 };
    double basePlaybackRate = 1.0;
    double jogNudgeRatio = 1.0;
    AudioCommandQueue<64> commands;

    void beginTrackSwap() noexcept
    {
        // Sequential consistency gives the callback and control thread one
        // total order: either an existing callback is observed and drained, or
        // a newly entering callback observes the gate and emits silence.
        trackSwapInProgress.store(true, std::memory_order_seq_cst);
        while (audioCallbacksActive.load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
    }

    void endTrackSwap() noexcept
    {
        trackSwapInProgress.store(false, std::memory_order_seq_cst);
    }

    struct ScopedTrackSwap final {
        explicit ScopedTrackSwap(Impl& pipeline) noexcept : owner(pipeline)
        {
            owner.beginTrackSwap();
        }

        ~ScopedTrackSwap()
        {
            owner.endTrackSwap();
        }

        Impl& owner;
    };

    void consumeCommands() noexcept
    {
        AudioCommand command;
        while (commands.pop(command)) {
            if (command.generation != 0 && command.generation != trackGeneration)
                continue;
            switch (command.type) {
            case AudioCommandType::Play:
                // Both flags must flip on the same audio callback: TimeStretchProcessor
                // pulls its input from RenderModeRouter, so if setInputPlaybackActive
                // won races ahead of setNormalPlaybackEnabled (e.g. called synchronously
                // from the control thread), the stretcher starts its keylock seed/bridge
                // handshake on a block that RenderModeRouter is still rendering as
                // pause-silence, and seeds the phase vocoder from silence.
                renderModeRouter->setNormalPlaybackEnabled(true);
                timeStretch->setInputPlaybackActive(playback != nullptr);
                if (!transport.isPlaying())
                    transport.start();
                break;
            case AudioCommandType::Pause:
                renderModeRouter->setNormalPlaybackEnabled(false);
                timeStretch->setInputPlaybackActive(false);
                break;
            case AudioCommandType::Seek: {
                // Seeks are coalesced below so the latest cursor always wins,
                // even when a controller floods the bounded command queue.
                break;
            }
            case AudioCommandType::SetLoop: {
                const double sampleRate = handle.sampleRate();
                if (playback && sampleRate > 0.0) {
                    playback->setLoopRangeSamples(
                        static_cast<std::int64_t>(std::llround(command.valueA * sampleRate)),
                        static_cast<std::int64_t>(std::llround(command.valueB * sampleRate)),
                        sampleRate);
                }
                renderModeRouter->setLoopRangeSeconds(command.valueA, command.valueB,
                                                      true, sampleRate);
                break;
            }
            case AudioCommandType::ClearLoop:
                if (playback)
                    playback->clearLoopRangeSamples();
                renderModeRouter->setLoopRangeSeconds(0.0, 0.0, false,
                                                      handle.sampleRate());
                break;
            case AudioCommandType::BeginScratch:
            case AudioCommandType::EndScratch:
            case AudioCommandType::ResetDeck:
                // Scratch uses its richer generation-tagged snapshot protocol;
                // structural resets remain control-thread lifecycle operations.
                break;
            }
        }

        const auto pendingSeek = seekGeneration.load(std::memory_order_acquire);
        if (pendingSeek != appliedSeekGeneration.load(std::memory_order_relaxed)) {
            const double seconds = commandedPositionSeconds.load(std::memory_order_acquire);
            if (playback) {
                playback->setCommandedReadPosition(static_cast<std::int64_t>(
                    std::llround(seconds * handle.sampleRate())));
            }
            // Harmless to arm while paused: the router clears this pending
            // flag on its silence path instead of consuming it, so only a
            // seek applied while actually rendering normal playback (a hot
            // cue or loop jump mid-track) triggers the declick blend.
            renderModeRouter->armNormalSeekDeclick();
            transport.setPosition(seconds);
            appliedSeekGeneration.store(pendingSeek, std::memory_order_release);
        }
    }
};

DeckAudioPipeline::DeckAudioPipeline(AudioPageCache& cache)
    : m_impl(std::make_unique<Impl>(cache))
{
}

DeckAudioPipeline::~DeckAudioPipeline()
{
    clearTrack();
}

void DeckAudioPipeline::prepareToPlay(int block, double rate)
{
    m_impl->mixer->prepareToPlay(block, rate);
}

void DeckAudioPipeline::releaseResources()
{
    m_impl->mixer->releaseResources();
}

void DeckAudioPipeline::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept
{
    struct CallbackActivity final {
        explicit CallbackActivity(std::atomic<unsigned int>& count) noexcept
            : active(count)
        {
            active.fetch_add(1, std::memory_order_seq_cst);
        }

        ~CallbackActivity()
        {
            active.fetch_sub(1, std::memory_order_seq_cst);
        }

        std::atomic<unsigned int>& active;
    } callbackActivity { m_impl->audioCallbacksActive };

    if (m_impl->trackSwapInProgress.load(std::memory_order_seq_cst)) {
        if (info.buffer != nullptr)
            info.clearActiveBufferRegion();
        return;
    }

    m_impl->consumeCommands();
    m_impl->mixer->getNextAudioBlock(info);
}

const juce::AudioBuffer<float>& DeckAudioPipeline::preFaderBuffer() const noexcept
{
    return m_impl->mixer->getPflBuffer();
}

const juce::AudioBuffer<float>& DeckAudioPipeline::postFaderTailBuffer() const noexcept
{
    return m_impl->mixer->getPostFaderTailBuffer();
}

void DeckAudioPipeline::setAudioPlayheadSink(std::atomic<double>* sink) noexcept
{
    m_impl->renderModeRouter->setAudioPlayheadSink(sink);
}

void DeckAudioPipeline::setTransportRunning(bool running) noexcept
{
    m_impl->transportRequestedRunning.store(running, std::memory_order_release);
    // setInputPlaybackActive() is applied in consumeCommands(), on the same audio
    // callback as setNormalPlaybackEnabled(), so TimeStretchProcessor and
    // RenderModeRouter never disagree about play state for even one block.
    const AudioCommand command {
        running ? AudioCommandType::Play : AudioCommandType::Pause,
        0.0, 0.0, m_impl->trackGeneration
    };
    (void) m_impl->commands.push(command);
}

void DeckAudioPipeline::seekToSeconds(double seconds) noexcept
{
    const double commanded = std::max(0.0, seconds);
    if (m_impl->playback && m_impl->handle.sampleRate() > 0.0) {
        m_impl->playback->prefetchForSeek(static_cast<juce::int64>(
            std::llround(commanded * m_impl->handle.sampleRate())));
    }
    m_impl->commandedPositionSeconds.store(commanded, std::memory_order_release);
    m_impl->seekGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void DeckAudioPipeline::setReverse(bool enabled) noexcept
{
    if (m_impl->playback)
        m_impl->playback->setReverse(enabled);
    m_impl->renderModeRouter->setReverse(enabled);
}

void DeckAudioPipeline::setPlaybackRate(double rate) noexcept
{
    m_impl->basePlaybackRate = std::clamp(rate, 0.01, 8.0);
    m_impl->renderModeRouter->setDeckTempoRatio(m_impl->basePlaybackRate);
    m_impl->timeStretch->setTempoRatio(m_impl->basePlaybackRate * m_impl->jogNudgeRatio);
}

void DeckAudioPipeline::setJogNudgeRatio(double ratio) noexcept
{
    m_impl->jogNudgeRatio = std::clamp(ratio, 0.94, 1.06);
    m_impl->renderModeRouter->setJogNudgeRatio(m_impl->jogNudgeRatio);
    m_impl->timeStretch->setTempoRatio(m_impl->basePlaybackRate * m_impl->jogNudgeRatio);
}

void DeckAudioPipeline::setKeylockEnabled(bool enabled) noexcept
{
    m_impl->renderModeRouter->setKeylockEnabled(enabled);
    m_impl->timeStretch->setPitchLockEnabled(enabled);
}

void DeckAudioPipeline::setKeySemitoneOffset(double semitones) noexcept
{
    m_impl->timeStretch->setKeySemitoneOffset(semitones);
}

void DeckAudioPipeline::setTimeStretchBackend(TimeStretchBackend backend) noexcept
{
    m_impl->timeStretch->setBackend(backend);
}

void DeckAudioPipeline::setLoopRangeSeconds(double startSeconds, double endSeconds, bool active,
                                         double sourceSampleRate) noexcept
{
    (void) sourceSampleRate;
    (void) m_impl->commands.push({active ? AudioCommandType::SetLoop
                                        : AudioCommandType::ClearLoop,
                                  startSeconds, endSeconds, m_impl->trackGeneration});
}

void DeckAudioPipeline::setPlaybackReadPositionSamples(std::int64_t position) noexcept
{
    if (m_impl->playback)
        m_impl->playback->setCommandedReadPosition(std::max<std::int64_t>(0, position));
}

int DeckAudioPipeline::keylockLatencySamples() const noexcept
{
    return m_impl->timeStretch ? std::max(0, m_impl->timeStretch->getLatencySamples()) : 0;
}

DeckAudioPipeline::TransportSnapshot DeckAudioPipeline::transportSnapshot() const noexcept
{
    return {
        m_impl->playback != nullptr,
        m_impl->playback != nullptr
            && m_impl->transportRequestedRunning.load(std::memory_order_acquire),
        m_impl->seekGeneration.load(std::memory_order_acquire)
                != m_impl->appliedSeekGeneration.load(std::memory_order_acquire)
            ? m_impl->commandedPositionSeconds.load(std::memory_order_acquire)
            : m_impl->transport.getCurrentPosition(),
        m_impl->transport.getLengthInSeconds(),
        m_impl->trackGeneration
    };
}

void DeckAudioPipeline::installPreparedTrack(PreparedTrack track)
{
    if (track.trackGeneration <= m_impl->trackGeneration
        || !std::isfinite(track.sourceSampleRate)
        || track.sourceSampleRate <= 0.0) {
        m_impl->cache.releaseTrack(track.cacheHandle);
        return;
    }

    Impl::ScopedTrackSwap trackSwap { *m_impl };
    m_impl->timeStretch->setInputPlaybackActive(false);
    m_impl->renderModeRouter->setNormalPlaybackEnabled(false);
    m_impl->transport.setSource(nullptr);
    m_impl->renderModeRouter->setPlaybackSource(nullptr);
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
    m_impl->renderModeRouter->setPlaybackSource(m_impl->playback.get());
    m_impl->timeStretch->setTrackGeneration(track.trackGeneration);
    m_impl->renderModeRouter->setTrackCacheSource(&m_impl->cache, m_impl->handle);
    m_impl->transport.setPosition(0.0);
    m_impl->commandedPositionSeconds.store(0.0, std::memory_order_release);
    const auto installedSeekGeneration = m_impl->seekGeneration.load(std::memory_order_acquire);
    m_impl->appliedSeekGeneration.store(installedSeekGeneration, std::memory_order_release);
    const bool shouldRun =
        m_impl->transportRequestedRunning.load(std::memory_order_acquire);
    m_impl->timeStretch->setInputPlaybackActive(shouldRun);
    m_impl->renderModeRouter->setNormalPlaybackEnabled(shouldRun);
    if (shouldRun)
        m_impl->transport.start();
}

void DeckAudioPipeline::clearTrack(std::uint64_t invalidThroughGeneration,
                                   AudioCacheReleaseMode releaseMode)
{
    if (!m_impl)
        return;

    Impl::ScopedTrackSwap trackSwap { *m_impl };
    m_impl->trackGeneration = std::max(m_impl->trackGeneration, invalidThroughGeneration);
    m_impl->transportRequestedRunning.store(false, std::memory_order_release);
    m_impl->commandedPositionSeconds.store(0.0, std::memory_order_release);
    const auto clearedSeekGeneration = m_impl->seekGeneration.load(std::memory_order_acquire);
    m_impl->appliedSeekGeneration.store(clearedSeekGeneration, std::memory_order_release);
    m_impl->timeStretch->setInputPlaybackActive(false);
    m_impl->renderModeRouter->setNormalPlaybackEnabled(false);
    m_impl->transport.setSource(nullptr);
    m_impl->renderModeRouter->setPlaybackSource(nullptr);
    m_impl->renderModeRouter->setTrackCacheSource(nullptr, {});
    if (m_impl->playback) {
        const auto stats = m_impl->playback->cacheStats();
        m_impl->retiredDiskReadsFromAudioThread += stats.diskReadsFromAudioThread;
        m_impl->retiredDecoderCallsFromAudioThread += stats.decoderCallsFromAudioThread;
    }
    m_impl->playback.reset();
    m_impl->cache.releaseTrack(m_impl->handle, releaseMode);
    m_impl->handle = {};
}

DeckAudioPipeline::RealtimeStats DeckAudioPipeline::realtimeStats() const noexcept
{
    RealtimeStats result;
    result.diskReadsFromAudioThread = m_impl->retiredDiskReadsFromAudioThread;
    result.decoderCallsFromAudioThread = m_impl->retiredDecoderCallsFromAudioThread;
    if (m_impl->playback) {
        const auto playback = m_impl->playback->cacheStats();
        result.diskReadsFromAudioThread += playback.diskReadsFromAudioThread;
        result.decoderCallsFromAudioThread += playback.decoderCallsFromAudioThread;
        result.playbackPageMisses = playback.pageMisses;
        result.playbackStarvationBlocks = playback.starvationBlocks;
        result.playbackDroppedRequests = playback.droppedRequests;
    }
    const auto scratch = m_impl->renderModeRouter->scratchCacheStats();
    result.diskReadsFromAudioThread += scratch.diskReadsFromAudioThread;
    result.scratchPageMisses = scratch.pageMisses;
    result.scratchStarvationBlocks = scratch.starvationBlocks;
    result.scratchDroppedRequests = scratch.droppedRequests;
    result.scratchRecoveryEvents = scratch.recoveryEvents;
    result.scratchGenerationMismatches = scratch.generationMismatches;

    const auto stretch = m_impl->timeStretch->realtimeStats();
    result.prepareCallsFromAudioThread = stretch.prepareCallsFromAudioThread;
    result.resetCallsFromAudioThread = stretch.resetCallsFromAudioThread;
    result.prewarmCallsFromAudioThread = stretch.prewarmCallsFromAudioThread;
    result.bufferGrowthsFromAudioThread = stretch.bufferGrowthsFromAudioThread;
    result.blockingLockAttempts = stretch.blockingLockAttempts;
    result.keylockSeeds = stretch.keylockSeeds;
    result.keylockSeedsOnAudioThread = stretch.keylockSeedsOnAudioThread;
    result.keylockSeedBridgeBlocks = stretch.keylockSeedBridgeBlocks;
    result.worstKeylockSeedMicros = stretch.worstKeylockSeedMicros;

    const auto mixer = m_impl->mixer->realtimeStats();
    result.coefficientBuildsFromAudioThread = mixer.coefficientBuildsFromAudioThread;
    result.prepareCallsFromAudioThread += mixer.prepareCallsFromAudioThread;
    result.bufferGrowthsFromAudioThread += mixer.bufferGrowthsFromAudioThread;
    result.blockingLockAttempts += mixer.blockingLockAttempts;
    result.objectConstructionsFromAudioThread = mixer.objectConstructionsFromAudioThread;
    result.droppedCommands = m_impl->commands.droppedCommands();
    result.trackGeneration = m_impl->trackGeneration;
    return result;
}

juce::AudioTransportSource& DeckAudioPipeline::transport() noexcept { return m_impl->transport; }
const juce::AudioTransportSource& DeckAudioPipeline::transport() const noexcept { return m_impl->transport; }
CachedPlaybackAudioSource* DeckAudioPipeline::playback() noexcept { return m_impl->playback.get(); }
const CachedPlaybackAudioSource* DeckAudioPipeline::playback() const noexcept { return m_impl->playback.get(); }
engine::audio::RenderModeRouter& DeckAudioPipeline::renderModeRouter() noexcept { return *m_impl->renderModeRouter; }
const engine::audio::RenderModeRouter& DeckAudioPipeline::renderModeRouter() const noexcept { return *m_impl->renderModeRouter; }
engine::audio::RenderModeRouter* DeckAudioPipeline::renderModeRouterPtr() noexcept { return m_impl->renderModeRouter.get(); }
const engine::audio::RenderModeRouter* DeckAudioPipeline::renderModeRouterPtr() const noexcept { return m_impl->renderModeRouter.get(); }
TimeStretchProcessor& DeckAudioPipeline::timeStretch() noexcept { return *m_impl->timeStretch; }
const TimeStretchProcessor& DeckAudioPipeline::timeStretch() const noexcept { return *m_impl->timeStretch; }
TimeStretchProcessor* DeckAudioPipeline::timeStretchPtr() noexcept { return m_impl->timeStretch.get(); }
const TimeStretchProcessor* DeckAudioPipeline::timeStretchPtr() const noexcept { return m_impl->timeStretch.get(); }
DeckChannelProcessor& DeckAudioPipeline::mixer() noexcept { return *m_impl->mixer; }
const DeckChannelProcessor& DeckAudioPipeline::mixer() const noexcept { return *m_impl->mixer; }
DeckChannelProcessor* DeckAudioPipeline::mixerPtr() noexcept { return m_impl->mixer.get(); }
const DeckChannelProcessor* DeckAudioPipeline::mixerPtr() const noexcept { return m_impl->mixer.get(); }
