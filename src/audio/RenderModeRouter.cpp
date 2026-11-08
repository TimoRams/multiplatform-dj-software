#include "RenderModeRouter.h"

#include "audio/cache/CachedPlaybackAudioSource.h"
#include "audio/internal/HermiteResamplingAudioSource.h"
#include "audio/internal/HermiteKernel.h"

#include <utility>

namespace engine::audio {

RenderModeRouter::RenderModeRouter(juce::AudioSource* inputSource, bool deleteInputWhenDeleted)
    : m_transport(inputSource, deleteInputWhenDeleted)
{
    if (m_transport) {
        m_positionableTransportSource = dynamic_cast<juce::PositionableAudioSource*>(m_transport.get());
        m_hermite = std::make_unique<HermiteResamplingAudioSource>(m_transport.get(), false);
    }
}

RenderModeRouter::~RenderModeRouter() = default;

void RenderModeRouter::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    m_outputSampleRate = std::max(1.0, sampleRate);
    m_readAheadStemSmoother.prepare(m_outputSampleRate);
    m_blockSize = std::max(64, samplesPerBlockExpected);
    m_timelineReady = false;
    if (m_hermite)
        m_hermite->prepareToPlay(samplesPerBlockExpected, sampleRate);
    else if (m_transport)
        m_transport->prepareToPlay(samplesPerBlockExpected, sampleRate);
    m_scratchResampler.prepare(2, m_blockSize, m_outputSampleRate);
    m_scratchExitResampler.prepare(2, m_blockSize, m_outputSampleRate);
    m_scratchExitAudio.setSize(2, kCrossfadeSamples);
    m_scratchExitAudio.clear();
    m_scratchExitRemaining = 0;
    m_deckLandingStarted = false;
    applyDeckTempoToHermite();
}

void RenderModeRouter::releaseResources()
{
    if (m_hermite)
        m_hermite->releaseResources();
    else if (m_transport)
        m_transport->releaseResources();
}

void RenderModeRouter::applyDeckTempoToHermite() noexcept
{
    if (!m_hermite)
        return;

    m_hermite->setResamplingRatio(effectiveDeckTempoRatio());
}

void RenderModeRouter::resetNormalPlaybackAfterSeek() noexcept
{
    m_timelineReady = false;
    if (m_hermite) {
        m_hermite->resetStream();
        m_hermite->snapSmoothedRatio();
    }
}

double RenderModeRouter::effectiveDeckTempoRatio() const noexcept
{
    // Direction is carried by CachedPlaybackAudioSource.  Hermite/JUCE
    // resampling remains a positive-rate boundary; a negative ratio here would
    // apply reverse a second time and leaves its forward-oriented pull path
    // starved on several block sizes.
    return std::abs(m_deckTempoRatio.load(std::memory_order_relaxed)
                    * m_jogNudgeRatio.load(std::memory_order_relaxed));
}

double RenderModeRouter::signedDeckTempoRatio() const noexcept
{
    const double speed = effectiveDeckTempoRatio();
    return m_reverse.load(std::memory_order_relaxed) ? -speed : speed;
}

bool RenderModeRouter::isScratchPathActive() const noexcept
{
    return m_useScratchScaler.load(std::memory_order_acquire)
        || m_controller.isActive()
        || m_controller.isInertiaActive();
}

void RenderModeRouter::beginScratch(double anchorSeconds,
                                     double trackSampleRate,
                                     double trackLengthSeconds,
                                     bool wasPlayingBeforeScratch,
                                     double normalPlaybackSpeed)
{
    m_readerHandoffGeneration.fetch_add(1, std::memory_order_acq_rel);
    // A re-grab supersedes any release that the callback has not consumed yet.
    m_cancelledHandoffGeneration.store(
        m_handoffCommandGeneration.load(std::memory_order_acquire),
        std::memory_order_release);
    m_cancelledReleaseGeneration.store(
        m_requestedReleaseGeneration.load(std::memory_order_acquire),
        std::memory_order_release);
    m_latestReleaseSpeed.store(0.0, std::memory_order_relaxed);
    m_trackSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_trackLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
    m_scratchDisplayFollowsAudio.store(false, std::memory_order_release);
    if (m_loopActive.load(std::memory_order_acquire))
        m_scratchDisplayFollowsAudio.store(true, std::memory_order_release);

    // Bind this scratch session to the physical touch that caused it. The
    // controller thread may already have accumulated several platter packets by
    // the time Qt handles TouchDown; keeping the generation lets the callback
    // consume that complete trajectory without accepting a later re-grab by
    // accident. Generic/on-screen scratch inputs simply leave the generation at
    // zero and continue through the existing UI-published path.
    std::uint64_t realtimeGeneration = 0;
    if (auto* input = m_realtimeScratchInput.load(std::memory_order_acquire)) {
        const auto snapshot = input->readForControlThread();
        if (snapshot.touching())
            realtimeGeneration = snapshot.generation;
    }
    m_startScratchInputGeneration.store(realtimeGeneration,
                                        std::memory_order_relaxed);

    const double audioAnchorSec = std::max(0.0, anchorSeconds);
    const double audioAnchorSamples = audioAnchorSec * trackSampleRate;
    const double targetSamples = audioAnchorSec * trackSampleRate;
    m_scratchResampler.prefetchAround(audioAnchorSamples);
    m_lastScratchPrefetchPage.store(
        static_cast<std::int64_t>(audioAnchorSamples)
            / AudioPage::kSamplesPerChannel,
        std::memory_order_relaxed);
    const double playbackSpeed = m_reverse.load(std::memory_order_relaxed)
        ? -std::abs(normalPlaybackSpeed)
        : std::abs(normalPlaybackSpeed);
    m_controller.setTrackSampleRate(trackSampleRate);
    m_platter.reset(targetSamples, trackSampleRate);
    m_pointerTrajectory.reset(m_pointerScratchClock(),
                              targetSamples);
    m_controller.startScratch(audioAnchorSamples, wasPlayingBeforeScratch, playbackSpeed);
    m_controller.setHandPositionSec(anchorSeconds);
    m_startPositionSeconds.store(audioAnchorSec, std::memory_order_relaxed);
    m_startSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_startLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
    m_startCommandGeneration.fetch_add(1, std::memory_order_release);
}

engine::scratch::ScratchReleaseDisposition RenderModeRouter::endScratch(bool allowInertia)
{
    if (allowInertia)
        return m_controller.releaseScratch();

    m_controller.stopScratch();
    return engine::scratch::ScratchReleaseDisposition::HandoffNow;
}

std::uint64_t RenderModeRouter::requestScratchRelease(double normalizedReleaseSpeed,
                                                       bool allowInertia,
                                                       bool playbackIntent) noexcept
{
    // Duplicate touch-up packets refer to the release already in flight.
    const auto current = m_requestedReleaseGeneration.load(std::memory_order_acquire);
    if (current > m_cancelledReleaseGeneration.load(std::memory_order_acquire)
        && m_completedReleaseGeneration.load(std::memory_order_acquire) < current) {
        return current;
    }
    if (m_controller.phase() != engine::scratch::ScratchPhase::TouchTracking)
        return current;

    const auto generation = m_releaseGenerationCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const double speed = std::clamp(normalizedReleaseSpeed, -8.0, 8.0);
    const double trackRate = std::max(1.0, m_trackSampleRate.load(std::memory_order_relaxed));

    // A single control-side writer publishes a coherent command. The callback
    // performs one read attempt and simply defers a block if it catches odd.
    const auto oddSequence = m_releaseCommandSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_releaseCommandGeneration.store(generation, std::memory_order_relaxed);
    m_releaseCommandSpeed.store(speed, std::memory_order_relaxed);
    m_releaseCommandDeckRate.store(signedDeckTempoRatio(), std::memory_order_relaxed);
    m_releaseCommandSampleRate.store(trackRate, std::memory_order_relaxed);
    m_releaseCommandPlaybackIntent.store(playbackIntent, std::memory_order_relaxed);
    m_releaseCommandAllowInertia.store(allowInertia, std::memory_order_relaxed);
    m_releaseCommandKeylock.store(m_keylockPassthrough.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    m_releaseCommandReverse.store(m_reverse.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    m_releaseCommandLoop.store(m_loopActive.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    m_latestReleaseSpeed.store(speed, std::memory_order_relaxed);

    // The callback changes TouchTracking to ReleasePending only after this
    // complete snapshot is readable, preserving any unrendered hand movement
    // before it decides whether to coast or hand off.
    m_requestedReleaseGeneration.store(generation, std::memory_order_release);
    m_releaseCommandSequence.store(oddSequence + 1, std::memory_order_release);
    return generation;
}

void RenderModeRouter::submitScratchReleaseSpeed(double normalizedReleaseSpeed) noexcept
{
    const auto boundGeneration =
        m_startScratchInputGeneration.load(std::memory_order_acquire);
    if (boundGeneration != 0) {
        if (auto* input = m_realtimeScratchInput.load(std::memory_order_acquire)) {
            const auto snapshot = input->readForControlThread();
            if (snapshot.generation == boundGeneration
                && snapshot.phase == engine::scratch::RealtimeScratchPhase::Released) {
                // The callback consumes this native rim packet by motionSequence.
                // Applying the later Qt copy as well would pull inertia toward
                // the same measurement twice and make release response uneven.
                return;
            }
        }
    }

    const double speed = std::clamp(normalizedReleaseSpeed, -8.0, 8.0);
    m_latestReleaseSpeed.store(speed, std::memory_order_release);
    m_controller.submitReleaseSpeed(speed);
}

void RenderModeRouter::engageScratchDuringInertia() noexcept
{
    if (!m_controller.isInertiaActive())
        return;

    const bool wasPlaying = m_controller.wasPlayingBeforeScratch();
    const double normalSpeed = signedDeckTempoRatio();
    // The callback remains the sole owner of the resampler state. The controller
    // preserves its inertia velocity, so re-grabbing stays continuous without a
    // control-thread read-head reset.
    m_pointerTrajectory.clear();
    m_controller.startScratch(m_controller.readPositionSamples(), wasPlaying, normalSpeed);
    m_useScratchScaler.store(true, std::memory_order_release);
}

void RenderModeRouter::addTargetDeltaSeconds(double deltaSeconds, double trackSampleRate) noexcept
{
    m_platter.addTimeDeltaSeconds(deltaSeconds);
    const double next = m_scratchDisplaySec.load(std::memory_order_relaxed) + deltaSeconds;
    m_scratchDisplaySec.store(next, std::memory_order_relaxed);
    juce::ignoreUnused(trackSampleRate);
}

void RenderModeRouter::prefetchScratchTarget() noexcept
{
    double samplePosition = m_platter.targetSamplePosition();
    if (!std::isfinite(samplePosition))
        return;
    if (m_loopActive.load(std::memory_order_acquire)) {
        const double loopIn = std::round(m_loopInSample.load(std::memory_order_relaxed));
        const double loopOut = std::round(m_loopOutSample.load(std::memory_order_relaxed));
        if (std::isfinite(loopIn) && std::isfinite(loopOut) && loopOut > loopIn + 1.0) {
            double offset = std::fmod(samplePosition - loopIn, loopOut - loopIn);
            if (offset < 0.0)
                offset += loopOut - loopIn;
            samplePosition = loopIn + offset;
        }
    }
    if (samplePosition < 0.0)
        return;
    const auto page = static_cast<std::int64_t>(samplePosition)
        / AudioPage::kSamplesPerChannel;
    auto previous = m_lastScratchPrefetchPage.load(std::memory_order_relaxed);
    if (previous >= 0 && std::abs(page - previous) <= 1)
        return;
    if (!m_lastScratchPrefetchPage.compare_exchange_strong(
            previous, page, std::memory_order_relaxed, std::memory_order_relaxed))
        return;
    m_scratchResampler.prefetchAround(samplePosition);
}

void RenderModeRouter::submitHandDeltaSeconds(double deltaSeconds,
                                              double dtSeconds,
                                              double measuredRate) noexcept
{
    m_platter.addTimeDeltaSeconds(deltaSeconds);
    m_controller.submitHandDelta(deltaSeconds, dtSeconds, measuredRate);
}

void RenderModeRouter::submitPointerDeltaSeconds(double deltaSeconds,
                                                 double dtSeconds) noexcept
{
    submitHandDeltaSeconds(deltaSeconds, dtSeconds);
    m_pointerTrajectory.push(m_pointerScratchClock(),
                             m_platter.targetSamplePosition());
}

void RenderModeRouter::submitReleaseDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept
{
    m_controller.submitReleaseDelta(deltaSeconds, dtSeconds);
}

void RenderModeRouter::syncScratchReadPosition(double displaySec, double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double audioSec = std::max(0.0, displaySec);
    const double audioSamples = audioSec * sr;

    m_readerSyncPositionSeconds.store(audioSec, std::memory_order_relaxed);
    m_readerSyncSampleRate.store(sr, std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
    m_controller.syncReadPositionSamples(audioSamples);
    m_controller.setHandPositionSec(displaySec);

    m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);
}

void RenderModeRouter::publishScratchDisplay(double displaySec) noexcept
{
    m_controller.setHandPositionSec(displaySec);
    // Once a loop has rebased the scratch timeline, only the audio cursor
    // represents the visible position, even if the loop is later cleared.
    if (!m_scratchDisplayFollowsAudio.load(std::memory_order_acquire)
        && !m_loopActive.load(std::memory_order_acquire)) {
        m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);
    }
}

void RenderModeRouter::configureTrack(double trackSampleRate, double trackLengthSeconds) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    m_trackSampleRate.store(sr, std::memory_order_relaxed);
    m_trackLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
    m_controller.setTrackSampleRate(sr);
    m_startLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
}

void RenderModeRouter::syncReadPositionSeconds(double positionSeconds, double trackSampleRate) noexcept
{
    m_readerSyncPositionSeconds.store(std::max(0.0, positionSeconds), std::memory_order_relaxed);
    m_readerSyncSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::prepareNormalPlaybackHandoff(double positionSeconds, double trackSampleRate) noexcept
{
    // The callback owns both the scratch reader and JUCE transport. Publishing a
    // generation here prevents a control-thread reset from racing a live block.
    m_handoffPositionSeconds.store(std::max(0.0, positionSeconds), std::memory_order_relaxed);
    m_handoffSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_handoffFromScratchCursor.store(false, std::memory_order_relaxed);
    m_handoffCommandGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::prepareNormalPlaybackHandoffFromScratchCursor(double trackSampleRate) noexcept
{
    // FLX10 touch-up can reach the UI thread before the audio callback has
    // rendered the last queued platter ticks. The callback therefore captures
    // its cursor only after producing one final scratch block.
    m_handoffPositionSeconds.store(
        readPositionSeconds(trackSampleRate), std::memory_order_relaxed);
    m_handoffSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_handoffFromScratchCursor.store(true, std::memory_order_relaxed);
    m_handoffCommandGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::exitScratchMode(double positionSeconds, double trackSampleRate) noexcept
{
    // Explicitly leaving scratch (pause, eject, track replacement) supersedes
    // every callback-owned release that may still be coasting. The following
    // handoff generation moves both readers to the authoritative cursor; the
    // cancelled release cannot later re-enable its old scratch path.
    m_cancelledReleaseGeneration.store(
        m_requestedReleaseGeneration.load(std::memory_order_acquire),
        std::memory_order_release);
    m_latestReleaseSpeed.store(0.0, std::memory_order_release);
    prepareNormalPlaybackHandoff(positionSeconds, trackSampleRate);
}

void RenderModeRouter::setDeckTempoRatio(double ratio) noexcept
{
    const double clamped = std::clamp(ratio, 0.01, 8.0);
    m_deckTempoRatio.store(clamped, std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(signedDeckTempoRatio());
    applyDeckTempoToHermite();
    m_transportIntentGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::setJogNudgeRatio(double ratio) noexcept
{
    m_jogNudgeRatio.store(std::clamp(ratio, 0.94, 1.06), std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(signedDeckTempoRatio());
    applyDeckTempoToHermite();
    m_transportIntentGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::setReverse(bool reverse) noexcept
{
    m_reverse.store(reverse, std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(signedDeckTempoRatio());
    applyDeckTempoToHermite();
    m_transportIntentGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::setNormalPlaybackEnabled(bool enabled) noexcept
{
    m_normalPlaybackEnabled.store(enabled, std::memory_order_release);
    m_transportIntentGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                                          double trackSampleRate) noexcept
{
    m_loopInSample.store(loopInSec * trackSampleRate, std::memory_order_relaxed);
    m_loopOutSample.store(loopOutSec * trackSampleRate, std::memory_order_relaxed);
    if (active)
        m_scratchDisplayFollowsAudio.store(true, std::memory_order_release);
    m_loopActive.store(active, std::memory_order_release);
    m_loopCommandGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle)
{
    m_readAheadCache = cache;
    m_readAheadHandle = handle;
    m_timelineReady = false;
    m_readAheadStemSmoother = {};
    m_readAheadStemSmoother.prepare(m_outputSampleRate);
    m_scratchResampler.setTrackCacheSource(cache, handle);
    m_scratchExitResampler.setTrackCacheSource(cache, handle);
}

void RenderModeRouter::setStemMixState(brockdj::stems::StemMixState* state) noexcept
{
    m_readAheadStems = state;
    m_scratchResampler.setStemMixState(state);
    m_scratchExitResampler.setStemMixState(state);
}

void RenderModeRouter::setReadAheadPosition(double seconds, double rate, int frames, int horizon) noexcept
{
    if (!m_timelineReady || std::signbit(rate) != std::signbit(m_readAheadDirection)) {
        m_tempoTimeline.reset(seconds * m_outputSampleRate, rate);
        m_timelineReady = true;
    }
    m_readAheadDirection = rate;
    m_tempoTimeline.setTarget(rate, std::max(frames, static_cast<int>(m_outputSampleRate * 0.01)));
    m_tempoTimeline.ensure(frames + horizon);
}

double RenderModeRouter::readAheadTempo(int offset) noexcept
{
    return std::abs(m_tempoTimeline.rate(offset));
}

double RenderModeRouter::readAheadPosition() noexcept
{
    return normalPlaybackPositionSeconds();
}

double RenderModeRouter::normalPlaybackPositionSeconds() noexcept
{
    double position = m_tempoTimeline.position(0) / m_outputSampleRate;
    const double sampleRate = m_readAheadHandle.sampleRate();
    if (sampleRate > 0.0 && m_loopActive.load(std::memory_order_acquire)) {
        const double start = m_loopInSample.load(std::memory_order_relaxed) / sampleRate;
        const double end = m_loopOutSample.load(std::memory_order_relaxed) / sampleRate;
        if (end > start && ((m_readAheadDirection >= 0.0 && position >= end)
                           || (m_readAheadDirection < 0.0 && position < start)))
            position = start + std::fmod(std::fmod(position - start, end - start) + end - start, end - start);
    }
    return std::clamp(position, 0.0, m_readAheadHandle.lengthInSamples() / std::max(1.0, sampleRate));
}

double RenderModeRouter::normalPlaybackTempo() noexcept
{
    return m_tempoTimeline.rate(0);
}

bool RenderModeRouter::readAhead(int offset, juce::AudioBuffer<float>& destination,
                                int count) noexcept
{
    if (!m_readAheadCache || !m_readAheadHandle.isValid() || count < 0
        || count > destination.getNumSamples() || destination.getNumChannels() < 2)
        return false;
    const bool loopEnabled = m_loopActive.load(std::memory_order_acquire);
    const auto loopStart = static_cast<std::int64_t>(m_loopInSample.load(std::memory_order_relaxed));
    const auto loopEnd = static_cast<std::int64_t>(m_loopOutSample.load(std::memory_order_relaxed));
    AudioPageReadGuard page;
    std::int64_t pageIndex = -1;
    bool available = true;
    brockdj::stems::StemReadScope stems(m_readAheadStems, m_readAheadStemSmoother, m_readAheadHandle.id());
    const auto wrapSample = [&](std::int64_t sample) {
        if (loopEnabled && loopEnd > loopStart
            && ((m_readAheadDirection >= 0.0 && sample >= loopEnd)
                || (m_readAheadDirection < 0.0 && sample < loopStart))) {
            const auto length = loopEnd - loopStart;
            sample = loopStart + ((sample - loopStart) % length + length) % length;
        }
        return sample;
    };
    const auto readSample = [&](std::int64_t sample, int channel) {
        sample = wrapSample(sample);
        if (sample < 0 || sample >= m_readAheadHandle.lengthInSamples())
            return 0.0f;
        const auto wantedPage = AudioPage::pageIndexForSample(sample);
        if (wantedPage != pageIndex) {
            page = m_readAheadCache->tryGetPage(m_readAheadHandle, wantedPage);
            pageIndex = wantedPage;
            if (!page) {
                available = false;
                (void) m_readAheadCache->requestPage(m_readAheadHandle, wantedPage,
                                                     AudioCachePriority::PlaybackReadAhead);
                return 0.0f;
            }
        }
        if (!page || sample - page->firstSample >= page->validSampleCount) {
            available = false;
            return 0.0f;
        }
        const auto sourceChannel = std::min(channel, static_cast<int>(page->channelCount) - 1);
        const float mix = page->channelData(static_cast<unsigned>(sourceChannel))[sample - page->firstSample];
        return stems.active() ? stems.apply(sample, channel, mix) : mix;
    };
    for (int sample = 0; sample < count; ++sample) {
        const double position = m_tempoTimeline.position(offset + sample)
            * m_readAheadHandle.sampleRate() / m_outputSampleRate;
        const auto base = static_cast<std::int64_t>(std::floor(position));
        const float fraction = static_cast<float>(position - base);
        if (stems.active())
            stems.advance(wrapSample(base));
        for (int channel = 0; channel < 2; ++channel) {
            const float value = cubicHermite(readSample(base - 1, channel),
                                             readSample(base, channel),
                                             readSample(base + 1, channel),
                                             readSample(base + 2, channel), fraction);
            destination.setSample(channel, sample, value);
        }
        if (!available)
            return false;
    }
    return true;
}

void RenderModeRouter::setRealtimeScratchInput(
    std::shared_ptr<engine::scratch::RealtimeScratchInput> input) noexcept
{
    // The manager owns the ingress for its whole lifetime as well. Publish the
    // raw pointer only after installing our lifetime-holding reference; audio
    // reads the pointer but never touches shared_ptr reference counts.
    m_realtimeScratchInputOwner = std::move(input);
    m_realtimeScratchInput.store(m_realtimeScratchInputOwner.get(),
                                 std::memory_order_release);
}

bool RenderModeRouter::isScratching() const noexcept
{
    return m_controller.isScratching();
}

bool RenderModeRouter::isInertiaActive() const noexcept
{
    return m_controller.isInertiaActive();
}

double RenderModeRouter::scratchRate() const noexcept
{
    return m_controller.normalizedRate();
}

double RenderModeRouter::readPositionSeconds(double trackSampleRate) const noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    return m_audioScratchReadPositionSamples.load(std::memory_order_acquire) / sr;
}

double RenderModeRouter::displayPositionSeconds() const noexcept
{
    return m_scratchDisplaySec.load(std::memory_order_relaxed);
}

bool RenderModeRouter::normalPlaybackHandoffPending() const noexcept
{
    const bool legacyPending = m_completedHandoffCommandGeneration.load(std::memory_order_acquire)
        != m_handoffCommandGeneration.load(std::memory_order_acquire);
    const bool releasePending = m_completedReleaseGeneration.load(std::memory_order_acquire)
        < m_requestedReleaseGeneration.load(std::memory_order_acquire);
    return legacyPending || releasePending;
}

ScratchReleaseSnapshot RenderModeRouter::scratchReleaseSnapshot() const noexcept
{
    ScratchReleaseSnapshot snapshot;
    const auto before = m_releaseAckSequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0U)
        return snapshot;

    snapshot.generation = m_releaseAckGeneration.load(std::memory_order_relaxed);
    snapshot.phase = static_cast<ScratchReleasePhase>(
        m_releaseAckPhase.load(std::memory_order_relaxed));
    snapshot.disposition = static_cast<engine::scratch::ScratchReleaseDisposition>(
        m_releaseAckDisposition.load(std::memory_order_relaxed));
    snapshot.finalCursorSeconds = m_releaseAckCursorSeconds.load(std::memory_order_relaxed);

    const auto after = m_releaseAckSequence.load(std::memory_order_acquire);
    if (before != after || (after & 1U) != 0U)
        return {};
    return snapshot;
}

bool RenderModeRouter::scratchReleaseComplete(std::uint64_t generation) const noexcept
{
    return generation != 0
        && m_completedReleaseGeneration.load(std::memory_order_acquire) >= generation;
}

void RenderModeRouter::publishReleaseSnapshot(
    std::uint64_t generation,
    ScratchReleasePhase phase,
    engine::scratch::ScratchReleaseDisposition disposition,
    double finalCursorSeconds) noexcept
{
    const auto oddSequence = m_releaseAckSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_releaseAckGeneration.store(generation, std::memory_order_relaxed);
    m_releaseAckPhase.store(static_cast<std::uint8_t>(phase), std::memory_order_relaxed);
    m_releaseAckDisposition.store(static_cast<std::uint8_t>(disposition),
                                  std::memory_order_relaxed);
    m_releaseAckCursorSeconds.store(finalCursorSeconds, std::memory_order_relaxed);
    m_releaseAckSequence.store(oddSequence + 1, std::memory_order_release);
}

bool RenderModeRouter::applyReaderHandoff(double positionSeconds,
                                           double trackSampleRate,
                                           bool releaseOwned) noexcept
{
    // The release CAS protects a new TouchTracking phase from an older callback.
    // If a re-grab has won, leave both readers masked for its queued start command.
    if (releaseOwned && !m_controller.completeHandoff())
        return false;
    m_deckLandingStarted = false;

    const double sr = std::max(1.0, trackSampleRate);
    const double position = std::max(0.0, positionSeconds);
    m_tempoTimeline.reset(position * m_outputSampleRate, signedDeckTempoRatio());
    m_timelineReady = true;
    m_readerHandoffGeneration.fetch_add(1, std::memory_order_acq_rel);
    // AudioTransportSource's PositionableAudioSource API is expressed in its
    // prepared output-rate samples and converts to the track rate internally.
    // Passing track samples here applies the conversion twice (e.g. 44.1/48)
    // and makes every scratch release jump backwards.
    const auto transportSamplePos = static_cast<juce::int64>(
        std::llround(position * std::max(1.0, m_outputSampleRate)));
    const auto sourceSamplePos = static_cast<juce::int64>(std::llround(position * sr));
    if (m_lastScratchOutputValid) {
        m_scratchExitResampler.beginContinuation(m_scratchResampler);
        m_scratchExitResampler.processBlock(m_scratchResampler.currentRate(),
                                            { &m_scratchExitAudio, 0, kCrossfadeSamples });
        m_scratchExitRemaining = kCrossfadeSamples;
    }
    m_scratchResampler.reset(position * sr);
    m_audioScratchReadPositionSamples.store(position * sr, std::memory_order_release);

    // Keep JUCE's output-rate bookkeeping and the source-rate cache cursor in
    // lockstep. CachedPlaybackAudioSource intentionally ignores ordinary JUCE
    // seeks while reversed, so the authoritative command must reach it directly.
    if (m_positionableTransportSource)
        m_positionableTransportSource->setNextReadPosition(transportSamplePos);
    if (auto* playback = m_playbackSource.load(std::memory_order_acquire))
        playback->setCommandedReadPosition(sourceSamplePos);
    if (m_hermite) {
        applyDeckTempoToHermite();
        m_hermite->resetStream();
        m_hermite->snapSmoothedRatio();
    }
    if (!releaseOwned)
        m_controller.stopScratch();
    m_audioScratchInputGeneration = 0;
    m_appliedRealtimeReleaseMotionSequence = 0;
    m_lastRealtimeScratchSnapshot = {};
    m_useScratchScaler.store(false, std::memory_order_release);
    m_prevScratchPath = false;
    m_appliedReaderSyncGeneration = m_readerSyncGeneration.load(std::memory_order_acquire);
    m_scratchExitTailPending = m_lastScratchOutputValid;
    return true;
}

void RenderModeRouter::applyNormalPlaybackHandoff(double positionSeconds,
                                                   double trackSampleRate,
                                                   std::uint64_t generation) noexcept
{
    (void) applyReaderHandoff(positionSeconds, trackSampleRate);
    m_completedHandoffCommandGeneration.store(generation, std::memory_order_release);
}

void RenderModeRouter::completeCursorHandoffAfterScratchBlock(double trackSampleRate) noexcept
{
    if (!m_cursorHandoffPending)
        return;

    const double sr = std::max(1.0, trackSampleRate);
    const double position = m_scratchResampler.readPosition() / sr;
    const auto generation = m_cursorHandoffGeneration;
    m_cursorHandoffPending = false;
    if (generation <= m_cancelledHandoffGeneration.load(std::memory_order_acquire)) {
        m_completedHandoffCommandGeneration.store(generation, std::memory_order_release);
        return;
    }
    applyNormalPlaybackHandoff(position, sr, generation);
    m_scratchDisplaySec.store(position, std::memory_order_relaxed);
    if (m_audioPlayheadSink != nullptr)
        m_audioPlayheadSink->store(position, std::memory_order_release);
}

void RenderModeRouter::snapHermiteToDeckTempo() noexcept
{
    applyDeckTempoToHermite();
    m_readerSyncPositionSeconds.store(std::max(0.0, displayPositionSeconds()), std::memory_order_relaxed);
    m_readerSyncSampleRate.store(m_trackSampleRate.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::consumeScratchReleaseCommand() noexcept
{
    const auto before = m_releaseCommandSequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0U)
        return;

    AudioReleaseCommand command;
    command.generation = m_releaseCommandGeneration.load(std::memory_order_relaxed);
    command.speed = m_releaseCommandSpeed.load(std::memory_order_relaxed);
    command.deckRate = m_releaseCommandDeckRate.load(std::memory_order_relaxed);
    command.sampleRate = m_releaseCommandSampleRate.load(std::memory_order_relaxed);
    command.playbackIntent = m_releaseCommandPlaybackIntent.load(std::memory_order_relaxed);
    command.allowInertia = m_releaseCommandAllowInertia.load(std::memory_order_relaxed);
    command.keylock = m_releaseCommandKeylock.load(std::memory_order_relaxed);
    command.reverse = m_releaseCommandReverse.load(std::memory_order_relaxed);
    command.loop = m_releaseCommandLoop.load(std::memory_order_relaxed);

    const auto after = m_releaseCommandSequence.load(std::memory_order_acquire);
    if (before != after || (after & 1U) != 0U
        || command.generation == 0
        || command.generation <= m_appliedReleaseGeneration) {
        return;
    }

    m_appliedReleaseGeneration = command.generation;
    if (command.generation
        <= m_cancelledReleaseGeneration.load(std::memory_order_acquire)) {
        publishReleaseSnapshot(command.generation,
                               ScratchReleasePhase::Idle,
                               engine::scratch::ScratchReleaseDisposition::HandoffNow,
                               readPositionSeconds(command.sampleRate));
        m_completedReleaseGeneration.store(command.generation, std::memory_order_release);
        return;
    }

    if (!m_controller.requestRelease(command.allowInertia)) {
        publishReleaseSnapshot(command.generation,
                               ScratchReleasePhase::Idle,
                               engine::scratch::ScratchReleaseDisposition::HandoffNow,
                               readPositionSeconds(command.sampleRate));
        m_completedReleaseGeneration.store(command.generation, std::memory_order_release);
        return;
    }

    m_audioReleaseCommand = command;
    m_audioReleasePhase = ScratchReleasePhase::ReleasePending;
    m_audioReleaseDisposition = engine::scratch::ScratchReleaseDisposition::HandoffNow;
    publishReleaseSnapshot(command.generation,
                           m_audioReleasePhase,
                           m_audioReleaseDisposition,
                           readPositionSeconds(command.sampleRate));
}

void RenderModeRouter::completeActiveReleaseHandoff(double trackSampleRate) noexcept
{
    const auto generation = m_audioReleaseCommand.generation;
    if (generation == 0)
        return;

    if (generation <= m_cancelledReleaseGeneration.load(std::memory_order_acquire)) {
        m_audioReleasePhase = ScratchReleasePhase::Idle;
        publishReleaseSnapshot(generation,
                               m_audioReleasePhase,
                               m_audioReleaseDisposition,
                               readPositionSeconds(trackSampleRate));
        m_completedReleaseGeneration.store(generation, std::memory_order_release);
        return;
    }

    const double sr = std::max(1.0, trackSampleRate);
    const double cursorSeconds = m_scratchResampler.readPosition() / sr;
    m_audioReleasePhase = ScratchReleasePhase::HandoffPending;
    publishReleaseSnapshot(generation,
                           m_audioReleasePhase,
                           m_audioReleaseDisposition,
                           cursorSeconds);

    if (!applyReaderHandoff(cursorSeconds, sr, true)) {
        m_audioReleasePhase = ScratchReleasePhase::Idle;
        publishReleaseSnapshot(generation,
                               m_audioReleasePhase,
                               m_audioReleaseDisposition,
                               readPositionSeconds(sr));
        m_completedReleaseGeneration.store(generation, std::memory_order_release);
        return;
    }
    m_tailReleaseGeneration = generation;
    m_audioReleasePhase = ScratchReleasePhase::TailSuppression;
    publishReleaseSnapshot(generation,
                           m_audioReleasePhase,
                           m_audioReleaseDisposition,
                           cursorSeconds);
    m_completedReleaseGeneration.store(generation, std::memory_order_release);
}

void RenderModeRouter::resolveScratchRelease() noexcept
{
    if (m_audioReleasePhase != ScratchReleasePhase::ReleasePending)
        return;

    const auto generation = m_audioReleaseCommand.generation;
    if (generation <= m_cancelledReleaseGeneration.load(std::memory_order_acquire)) {
        m_audioReleasePhase = ScratchReleasePhase::Idle;
        publishReleaseSnapshot(generation,
                               m_audioReleasePhase,
                               m_audioReleaseDisposition,
                               readPositionSeconds(m_audioReleaseCommand.sampleRate));
        m_completedReleaseGeneration.store(generation, std::memory_order_release);
        return;
    }

    double speed = m_latestReleaseSpeed.load(std::memory_order_acquire);
    engine::scratch::RealtimeScratchSnapshot realtimeSnapshot;
    if (readRealtimeScratchInput(realtimeSnapshot)
        && realtimeSnapshot.phase == engine::scratch::RealtimeScratchPhase::Released) {
        // TouchUp reaches the native ingress before the same MIDI packet can be
        // queued through Qt. It is therefore the least delayed and least filtered
        // measurement of a throw; use it for the callback-owned release decision.
        speed = realtimeSnapshot.velocity;
        m_appliedRealtimeReleaseMotionSequence = realtimeSnapshot.motionSequence;
    }
    if (!std::isfinite(speed)) {
        const double renderedSpeed = m_controller.normalizedRate();
        const double releaseEstimate = m_controller.releaseSpeedEstimate();
        speed = std::abs(releaseEstimate) > 0.0
            ? std::copysign(std::abs(renderedSpeed) > 1e-6
                                ? std::abs(renderedSpeed)
                                : std::abs(releaseEstimate),
                            releaseEstimate)
            : 0.0;
    }
    const double oneX = m_audioReleaseCommand.sampleRate / std::max(1.0, m_outputSampleRate);
    const double renderedSpeed = m_scratchResampler.currentRate() / oneX;
    const double deckSpeed = m_audioReleaseCommand.deckRate;
    if (m_audioReleaseCommand.playbackIntent && m_audioReleaseCommand.allowInertia
        && speed * deckSpeed > 0.0 && renderedSpeed * deckSpeed > 0.0
        && std::abs(speed) > std::abs(deckSpeed)
        && std::abs(renderedSpeed) >= std::abs(deckSpeed)) {
        speed = renderedSpeed;
    }
    m_audioReleaseDisposition = m_controller.releaseScratchWithSpeed(
        speed,
        m_audioReleaseCommand.allowInertia,
        m_audioReleaseCommand.deckRate,
        m_audioReleaseCommand.playbackIntent);

    switch (m_audioReleaseDisposition) {
    case engine::scratch::ScratchReleaseDisposition::CoastToDeckRate:
        // processScratchTracking already left the resampler at the velocity it
        // actually rendered. Preserve that state: snapping it to the raw MIDI
        // quotient here creates a speed edge exactly when the hand lifts.
        m_audioReleasePhase = ScratchReleasePhase::CoastToDeck;
        break;
    case engine::scratch::ScratchReleaseDisposition::CoastToStop:
        // The coast controller supplies the new target on the next block; the
        // resampler interpolates there from the rendered tracking velocity.
        m_audioReleasePhase = ScratchReleasePhase::CoastToStop;
        break;
    case engine::scratch::ScratchReleaseDisposition::HandoffNow:
        if (m_audioReleaseCommand.playbackIntent && m_audioReleaseCommand.allowInertia
            && speed * deckSpeed > 0.0 && renderedSpeed * deckSpeed > 0.0
            && std::abs(renderedSpeed) > std::abs(deckSpeed) + 1.0e-9) {
            m_audioReleasePhase = ScratchReleasePhase::CoastToDeck;
            break;
        }
        completeActiveReleaseHandoff(m_audioReleaseCommand.sampleRate);
        return;
    }

    publishReleaseSnapshot(generation,
                           m_audioReleasePhase,
                           m_audioReleaseDisposition,
                           readPositionSeconds(m_audioReleaseCommand.sampleRate));
}

void RenderModeRouter::finishCoastHandoffAfterScratchBlock(double trackSampleRate) noexcept
{
    if (m_audioReleasePhase != ScratchReleasePhase::CoastToDeck
        && m_audioReleasePhase != ScratchReleasePhase::CoastToStop
        && m_audioReleasePhase != ScratchReleasePhase::HandoffPending) {
        return;
    }

    if (m_controller.handoffPending()
        && (!m_deckLandingStarted || !m_scratchResampler.rateLandingActive()))
        completeActiveReleaseHandoff(trackSampleRate);
}

void RenderModeRouter::consumePendingAudioCommands() noexcept
{
    const auto loopGeneration = m_loopCommandGeneration.load(std::memory_order_acquire);
    if (loopGeneration != m_appliedLoopCommandGeneration) {
        m_scratchResampler.setLoopRange(m_loopInSample.load(std::memory_order_relaxed),
                                        m_loopOutSample.load(std::memory_order_relaxed),
                                        m_loopActive.load(std::memory_order_relaxed));
        m_appliedLoopCommandGeneration = loopGeneration;
    }

    const auto startGeneration = m_startCommandGeneration.load(std::memory_order_acquire);
    if (startGeneration != m_appliedStartCommandGeneration) {
        const double sr = std::max(1.0, m_startSampleRate.load(std::memory_order_relaxed));
        const double position = std::max(0.0, m_startPositionSeconds.load(std::memory_order_relaxed));
        m_scratchResampler.setTrackLengthSamples(
            std::max(0.0, m_startLengthSeconds.load(std::memory_order_relaxed)) * sr);
        m_scratchResampler.reset(position * sr);
        m_deckLandingStarted = false;
        m_scratchResampler.snapSmoothedRate(0.0);
        m_audioScratchReadPositionSamples.store(position * sr, std::memory_order_release);
        m_audioScratchInputGeneration =
            m_startScratchInputGeneration.load(std::memory_order_relaxed);
        m_audioScratchInputAnchorSamples = position * sr;
        m_appliedRealtimeReleaseMotionSequence = 0;
        m_lastRealtimeScratchSnapshot = {};
        m_useScratchScaler.store(true, std::memory_order_release);
        if (m_audioReleaseCommand.generation
            <= m_cancelledReleaseGeneration.load(std::memory_order_acquire)) {
            m_audioReleaseCommand = {};
            m_audioReleasePhase = ScratchReleasePhase::Idle;
            m_scratchExitTailPending = false;
            m_scratchExitRemaining = 0;
            m_tailReleaseGeneration = 0;
        }
        m_appliedStartCommandGeneration = startGeneration;
    }

    consumeScratchReleaseCommand();

    const auto transportIntentGeneration =
        m_transportIntentGeneration.load(std::memory_order_acquire);
    if (transportIntentGeneration != m_appliedTransportIntentGeneration) {
        const bool playbackIntent =
            m_normalPlaybackEnabled.load(std::memory_order_acquire);
        m_audioReleaseCommand.playbackIntent = playbackIntent;
        m_audioReleaseCommand.deckRate = signedDeckTempoRatio();

        if (m_audioReleasePhase == ScratchReleasePhase::CoastToDeck
            || m_audioReleasePhase == ScratchReleasePhase::CoastToStop
            || m_audioReleasePhase == ScratchReleasePhase::HandoffPending) {
            m_audioReleaseDisposition = m_controller.retargetRelease(
                playbackIntent, m_audioReleaseCommand.deckRate);
            switch (m_audioReleaseDisposition) {
            case engine::scratch::ScratchReleaseDisposition::CoastToDeckRate:
                m_audioReleasePhase = ScratchReleasePhase::CoastToDeck;
                break;
            case engine::scratch::ScratchReleaseDisposition::CoastToStop:
                m_audioReleasePhase = ScratchReleasePhase::CoastToStop;
                break;
            case engine::scratch::ScratchReleaseDisposition::HandoffNow:
                m_audioReleasePhase = ScratchReleasePhase::HandoffPending;
                break;
            }
            publishReleaseSnapshot(m_audioReleaseCommand.generation,
                                   m_audioReleasePhase,
                                   m_audioReleaseDisposition,
                                   readPositionSeconds(m_audioReleaseCommand.sampleRate));
        }
        m_appliedTransportIntentGeneration = transportIntentGeneration;
    }

    // Track replacement or an explicit scratch teardown can cancel a release
    // after the callback has already accepted it. Complete that generation
    // before consuming the reader handoff so no stale inertia state survives.
    const auto activeReleaseGeneration = m_audioReleaseCommand.generation;
    if (activeReleaseGeneration != 0
        && activeReleaseGeneration
            <= m_cancelledReleaseGeneration.load(std::memory_order_acquire)
        && m_completedReleaseGeneration.load(std::memory_order_acquire)
            < activeReleaseGeneration) {
        m_audioReleasePhase = ScratchReleasePhase::Idle;
        m_audioReleaseDisposition = engine::scratch::ScratchReleaseDisposition::HandoffNow;
        m_controller.stopScratch();
        publishReleaseSnapshot(activeReleaseGeneration,
                               m_audioReleasePhase,
                               m_audioReleaseDisposition,
                               readPositionSeconds(m_audioReleaseCommand.sampleRate));
        m_completedReleaseGeneration.store(activeReleaseGeneration,
                                           std::memory_order_release);
        m_tailReleaseGeneration = 0;
    }

    const auto handoffGeneration = m_handoffCommandGeneration.load(std::memory_order_acquire);
    if (handoffGeneration != m_appliedHandoffCommandGeneration) {
        const double sr = std::max(1.0, m_handoffSampleRate.load(std::memory_order_relaxed));
        const double position = std::max(0.0, m_handoffPositionSeconds.load(std::memory_order_relaxed));
        const bool captureCursor = m_handoffFromScratchCursor.load(std::memory_order_relaxed);
        m_appliedHandoffCommandGeneration = handoffGeneration;
        const bool cancelled = handoffGeneration
            <= m_cancelledHandoffGeneration.load(std::memory_order_acquire);
        const bool scratchPathActive = m_useScratchScaler.load(std::memory_order_acquire);
        if (cancelled) {
            m_completedHandoffCommandGeneration.store(handoffGeneration, std::memory_order_release);
        } else if (captureCursor && scratchPathActive && m_controller.touching()) {
            m_cursorHandoffPending = true;
            m_cursorHandoffGeneration = handoffGeneration;
            m_cursorHandoffSampleRate = sr;
        } else if (captureCursor && scratchPathActive) {
            applyNormalPlaybackHandoff(
                m_scratchResampler.readPosition() / sr, sr, handoffGeneration);
        } else {
            applyNormalPlaybackHandoff(position, sr, handoffGeneration);
        }
    }

    const auto syncGeneration = m_readerSyncGeneration.load(std::memory_order_acquire);
    if (syncGeneration != m_appliedReaderSyncGeneration) {
        const double sr = std::max(1.0, m_readerSyncSampleRate.load(std::memory_order_relaxed));
        const double position = std::max(0.0, m_readerSyncPositionSeconds.load(std::memory_order_relaxed));
        const auto transportSamplePos = static_cast<juce::int64>(
            std::llround(position * std::max(1.0, m_outputSampleRate)));
        m_scratchResampler.reset(position * sr);
        m_audioScratchReadPositionSamples.store(position * sr, std::memory_order_release);
        if (m_positionableTransportSource)
            m_positionableTransportSource->setNextReadPosition(transportSamplePos);
        if (auto* playback = m_playbackSource.load(std::memory_order_acquire)) {
            playback->setCommandedReadPosition(static_cast<juce::int64>(
                std::llround(position * sr)));
        }
        if (m_hermite) {
            applyDeckTempoToHermite();
            m_hermite->resetStream();
            m_hermite->snapSmoothedRatio();
        }
        m_appliedReaderSyncGeneration = syncGeneration;
    }
}

bool RenderModeRouter::readRealtimeScratchInput(
    engine::scratch::RealtimeScratchSnapshot& snapshot) noexcept
{
    if (m_audioScratchInputGeneration == 0)
        return false;

    if (auto* input = m_realtimeScratchInput.load(std::memory_order_acquire)) {
        engine::scratch::RealtimeScratchSnapshot candidate;
        if (input->tryRead(candidate)
            && candidate.generation == m_audioScratchInputGeneration) {
            m_lastRealtimeScratchSnapshot = candidate;
        }
    }

    if (m_lastRealtimeScratchSnapshot.generation != m_audioScratchInputGeneration)
        return false;

    snapshot = m_lastRealtimeScratchSnapshot;
    return true;
}

double RenderModeRouter::decayedRealtimeVelocity(
    const engine::scratch::RealtimeScratchSnapshot& snapshot) const noexcept
{
    const double velocity = std::clamp(
        std::isfinite(snapshot.velocity) ? snapshot.velocity : 0.0, -8.0, 8.0);
    const double now = engine::scratch::RealtimeScratchInput::clockSeconds();
    const double age = std::clamp(now - snapshot.lastEventTimestampSeconds,
                                  0.0, 0.120);
    const double interval = std::clamp(
        std::isfinite(snapshot.eventIntervalSeconds)
            ? snapshot.eventIntervalSeconds : 0.0,
        0.0, 0.120);
    const double hold = std::max(0.008, interval * 1.25);
    if (age <= hold)
        return velocity;

    const double tau = std::max(0.007, interval * 0.5);
    const double decayed = velocity * std::exp(-(age - hold) / tau);
    return std::abs(decayed) < 0.00005 ? 0.0 : decayed;
}

double RenderModeRouter::activePlaybackRate(double trackSampleRate, int bufferSize) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    engine::scratch::RealtimeScratchSnapshot snapshot;
    if (readRealtimeScratchInput(snapshot)
        && snapshot.phase == engine::scratch::RealtimeScratchPhase::Released
        && snapshot.motionSequence != m_appliedRealtimeReleaseMotionSequence) {
        // Rim packets following TouchUp refine a backspin/throw directly on the
        // audio side. The controller still owns the coast and handoff state; only
        // its measured velocity input bypasses the UI queue.
        m_controller.submitReleaseSpeed(snapshot.velocity);
        m_appliedRealtimeReleaseMotionSequence = snapshot.motionSequence;
    }
    return m_controller.processAudioBlock(std::max(1, bufferSize), m_outputSampleRate, sr);
}

void RenderModeRouter::applyNormalPathCrossfade(const juce::AudioSourceChannelInfo& info) noexcept
{
    int remaining = m_crossfadeRemaining.load(std::memory_order_relaxed);
    if (remaining <= 0 || !info.buffer || info.numSamples <= 0)
        return;

    const int start = info.startSample;
    const int n = info.numSamples;
    if (start < 0 || start + n > info.buffer->getNumSamples())
        return;

    const int channels = std::min(info.buffer->getNumChannels(), 2);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(remaining) / static_cast<float>(kCrossfadeSamples);
        const float gain = std::min(1.0f, 1.0f - t);
        for (int ch = 0; ch < channels; ++ch) {
            float* w = info.buffer->getWritePointer(ch, start + i);
            *w *= gain;
        }
        remaining = std::max(0, remaining - 1);
    }
    m_crossfadeRemaining.store(remaining, std::memory_order_relaxed);
}

void RenderModeRouter::captureScratchTail(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!info.buffer || info.numSamples <= 0)
        return;
    const int sample = info.startSample + info.numSamples - 1;
    if (sample < 0 || sample >= info.buffer->getNumSamples())
        return;

    const int channels = std::min(2, info.buffer->getNumChannels());
    if (channels <= 0)
        return;
    m_lastScratchOutput[0] = info.buffer->getSample(0, sample);
    m_lastScratchOutput[1] = info.buffer->getSample(channels > 1 ? 1 : 0, sample);
    m_lastScratchOutputValid = true;
}

void RenderModeRouter::applyScratchExitTail(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!m_scratchExitTailPending || !m_lastScratchOutputValid
        || !info.buffer || info.numSamples <= 0) {
        return;
    }

    const int start = info.startSample;
    const int n = info.numSamples;
    if (start < 0 || start + n > info.buffer->getNumSamples())
        return;

    const int channels = std::min(2, info.buffer->getNumChannels());
    const int fadeSamples = std::min(m_scratchExitRemaining, n);
    const int fadeStart = kCrossfadeSamples - m_scratchExitRemaining;
    for (int ch = 0; ch < channels; ++ch) {
        float* output = info.buffer->getWritePointer(ch, start);
        for (int i = 0; i < fadeSamples; ++i) {
            const float tail = m_scratchExitAudio.getSample(ch, fadeStart + i);
            const float progress = static_cast<float>(fadeStart + i + 1)
                / static_cast<float>(kCrossfadeSamples);
            const float normalMix = progress * progress * (3.0f - 2.0f * progress);
            output[i] = tail + (output[i] - tail) * normalMix;
        }
    }

    m_scratchExitRemaining -= fadeSamples;
    if (m_scratchExitRemaining > 0)
        return;

    m_scratchExitTailPending = false;
    m_lastScratchOutputValid = false;
    if (m_tailReleaseGeneration != 0) {
        const auto generation = m_tailReleaseGeneration;
        m_tailReleaseGeneration = 0;
        m_audioReleasePhase = ScratchReleasePhase::Idle;
        publishReleaseSnapshot(generation,
                               m_audioReleasePhase,
                               m_audioReleaseDisposition,
                               m_releaseAckCursorSeconds.load(std::memory_order_relaxed));
    }
}

void RenderModeRouter::applyNormalStartFade(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!info.buffer || info.numSamples <= 0)
        return;

    constexpr int startFadeSamples = 128;
    const int channels = std::min(2, info.buffer->getNumChannels());
    const int count = std::min(startFadeSamples, info.numSamples);
    for (int ch = 0; ch < channels; ++ch) {
        float* output = info.buffer->getWritePointer(ch, info.startSample);
        for (int i = 0; i < count; ++i) {
            const float gain = static_cast<float>(i + 1) / static_cast<float>(count);
            output[i] *= gain;
        }
    }
}

void RenderModeRouter::applyNormalSeekDeclick(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!m_lastNormalOutputValid || !info.buffer || info.numSamples <= 0)
        return;

    const int start = info.startSample;
    const int n = info.numSamples;
    if (start < 0 || start + n > info.buffer->getNumSamples())
        return;

    const int channels = std::min(2, info.buffer->getNumChannels());
    const int fadeSamples = std::min(kCrossfadeSamples, n);
    for (int ch = 0; ch < channels; ++ch) {
        float* output = info.buffer->getWritePointer(ch, start);
        const float tail = m_lastNormalOutput[static_cast<std::size_t>(ch)];
        for (int i = 0; i < fadeSamples; ++i) {
            const float mix = static_cast<float>(i + 1) / static_cast<float>(fadeSamples);
            output[i] = tail + (output[i] - tail) * mix;
        }
    }
}

void RenderModeRouter::captureNormalTail(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!info.buffer || info.numSamples <= 0)
        return;
    const int sample = info.startSample + info.numSamples - 1;
    if (sample < 0 || sample >= info.buffer->getNumSamples())
        return;

    const int channels = std::min(2, info.buffer->getNumChannels());
    if (channels <= 0)
        return;
    m_lastNormalOutput[0] = info.buffer->getSample(0, sample);
    m_lastNormalOutput[1] = info.buffer->getSample(channels > 1 ? 1 : 0, sample);
    m_lastNormalOutputValid = true;
}

void RenderModeRouter::applyNormalStopTail(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!m_lastNormalOutputValid || !info.buffer || info.numSamples <= 0)
        return;

    constexpr int stopFadeSamples = 128;
    const int channels = std::min(2, info.buffer->getNumChannels());
    const int count = std::min(stopFadeSamples, info.numSamples);
    for (int ch = 0; ch < channels; ++ch) {
        float* output = info.buffer->getWritePointer(ch, info.startSample);
        const float tail = m_lastNormalOutput[static_cast<std::size_t>(ch)];
        for (int i = 0; i < count; ++i) {
            const float gain = 1.0f - static_cast<float>(i + 1)
                / static_cast<float>(count);
            output[i] = tail * gain;
        }
    }
    m_lastNormalOutputValid = false;
}

void RenderModeRouter::publishScratchCursor(double readPositionSamples,
                                             double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double seconds = readPositionSamples / sr;
    m_audioScratchReadPositionSamples.store(readPositionSamples, std::memory_order_release);
    m_controller.syncReadPositionSamples(readPositionSamples);
    m_scratchDisplaySec.store(seconds, std::memory_order_relaxed);
    if (m_audioPlayheadSink != nullptr)
        m_audioPlayheadSink->store(seconds, std::memory_order_release);
}

void RenderModeRouter::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (!m_transport || !bufferToFill.buffer) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    consumePendingAudioCommands();

    if (m_audioReleasePhase == ScratchReleasePhase::ReleasePending
        && m_audioReleaseCommand.playbackIntent && m_audioReleaseCommand.allowInertia
        && !m_loopActive.load(std::memory_order_acquire)) {
        double target = m_platter.targetSamplePosition();
        double releaseSpeed = m_latestReleaseSpeed.load(std::memory_order_acquire);
        engine::scratch::RealtimeScratchSnapshot snapshot;
        if (readRealtimeScratchInput(snapshot)) {
            target = m_audioScratchInputAnchorSamples
                + snapshot.cumulativeDeltaSeconds * m_audioReleaseCommand.sampleRate;
            if (snapshot.phase == engine::scratch::RealtimeScratchPhase::Released)
                releaseSpeed = snapshot.velocity;
        }
        if (!std::isfinite(releaseSpeed))
            releaseSpeed = m_controller.releaseSpeedEstimate();
        const double deckRate = signedDeckTempoRatio();
        const double oneX = m_audioReleaseCommand.sampleRate / std::max(1.0, m_outputSampleRate);
        const double renderedRate = m_scratchResampler.currentRate() / oneX;
        if (releaseSpeed * deckRate > 0.0 && renderedRate * deckRate > 0.0
            && std::abs(renderedRate) >= std::abs(deckRate)
            && (target - m_scratchResampler.readPosition()) * deckRate <= 0.0) {
            resolveScratchRelease();
        }
    }

    // Keep the scratch path armed after inertia reaches its target. Only the
    // blockwise handoff command may expose the normal transport: its reader can
    // still point at the pre-scratch position until that command seeks it.
    const bool scratching = isScratchPathActive();
    m_prevScratchPath = scratching;
    m_activeRenderMode.store(
        scratching ? RenderMode::Scratch
                   : (m_keylockPassthrough.load(std::memory_order_relaxed)
                          ? RenderMode::Keylock : RenderMode::Direct),
        std::memory_order_release);

    if (!scratching) {
        const bool normalPlaybackEnabled =
            m_normalPlaybackEnabled.load(std::memory_order_acquire);
        if (!normalPlaybackEnabled) {
            bufferToFill.clearActiveBufferRegion();
            if (!m_scratchExitTailPending && m_normalPlaybackWasEnabled)
                applyNormalStopTail(bufferToFill);
            else
                m_lastNormalOutputValid = false;
            m_normalPlaybackWasEnabled = false;
            m_normalSeekTailPending = false;
            applyScratchExitTail(bufferToFill);
            return;
        }

        // Hermite always owns tempo and jog-rate movement. With keylock active,
        // the selected downstream time-stretch backend keeps its time ratio at
        // 1 and applies the inverse pitch scale, preserving pitch without
        // losing reader speed.
        if (m_hermite) {
            if (m_readAheadEnabled.load(std::memory_order_acquire)
                && m_readAheadHandle.isValid() && bufferToFill.numSamples <= static_cast<int>(m_normalRates.size())) {
                for (int sample = 0; sample < bufferToFill.numSamples; ++sample)
                    m_normalRates[static_cast<std::size_t>(sample)] = std::abs(m_tempoTimeline.rate(sample));
                m_hermite->getNextAudioBlockWithRates(bufferToFill, m_normalRates.data());
            } else {
                m_hermite->getNextAudioBlock(bufferToFill);
            }
        } else {
            m_transport->getNextAudioBlock(bufferToFill);
        }
        applyNormalPathCrossfade(bufferToFill);
        applyScratchExitTail(bufferToFill);
        if (!m_normalPlaybackWasEnabled) {
            // Resuming from pause jumped straight to full level with no
            // envelope, unlike the fade the stop side already gets below —
            // that abrupt onset is what read as a click/"weird" attack on
            // play. Mirror the stop-side declick with a matching fade-in.
            applyNormalStartFade(bufferToFill);
        } else if (m_normalSeekTailPending) {
            // A hot cue, quantized cue, or loop jump moved the read head while
            // this deck kept playing, which is a genuine waveform
            // discontinuity (not just silence-to-signal). Blend from the last
            // sample heard before the jump instead of splicing it in raw.
            applyNormalSeekDeclick(bufferToFill);
        }
        m_normalSeekTailPending = false;
        captureNormalTail(bufferToFill);
        m_tempoTimeline.advance(bufferToFill.numSamples);
        m_normalPlaybackWasEnabled = true;
        return;
    }

    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double oneX = sr / std::max(1.0, m_outputSampleRate);

    if (m_controller.requiresPositionTracking() || m_cursorHandoffPending) {
        // Position-authoritative scratch: a critically-damped tracker glides the
        // read head to the hand target. Exact tracking for slow/precise moves,
        // momentum across sparse UI events, no overshoot/snap-back warble.
        double target = m_platter.targetSamplePosition();
        const auto hand = m_controller.handPrediction();
        double commandedRate = hand.normalizedRate * oneX;
        double inputLead = hand.ageSeconds;
        double accumulatedLeadSamples = hand.leadSeconds * sr;
        double maxReferenceCorrectionRate = std::numeric_limits<double>::infinity();
        double tickCorrectionTimeSeconds = 0.0;
        if (!hand.inputMeasuredRate) {
            const double uiSpeed = std::abs(hand.initialNormalizedRate);
            maxReferenceCorrectionRate =
                (0.05 + 0.5 * uiSpeed + uiSpeed * uiSpeed) * oneX;
        }
        engine::scratch::RealtimeScratchSnapshot realtimeSnapshot;
        if (readRealtimeScratchInput(realtimeSnapshot)) {
            target = m_audioScratchInputAnchorSamples
                + realtimeSnapshot.fittedCumulativeDeltaSeconds * sr;
            commandedRate = decayedRealtimeVelocity(realtimeSnapshot) * oneX;
            inputLead = std::clamp(
                engine::scratch::RealtimeScratchInput::clockSeconds()
                    - realtimeSnapshot.lastEventTimestampSeconds,
                0.0, 0.030);
            accumulatedLeadSamples = std::numeric_limits<double>::quiet_NaN();
            maxReferenceCorrectionRate = std::numeric_limits<double>::infinity();
            tickCorrectionTimeSeconds = 0.015;
        } else if (engine::scratch::PointerScratchTrajectory::Estimate pointer;
                   m_pointerTrajectory.evaluate(
                       m_pointerScratchClock(), pointer)) {
            // The rendered pointer path is already continuous in position and
            // speed and is evaluated at the time it represents: no age to
            // extrapolate across, no stepped report to spread out.
            target = pointer.positionSamples;
            commandedRate = pointer.velocitySamplesPerSecond
                / std::max(1.0, m_outputSampleRate);
            inputLead = 0.0;
            accumulatedLeadSamples = 0.0;
            maxReferenceCorrectionRate = std::numeric_limits<double>::infinity();
            tickCorrectionTimeSeconds = 0.0;
        }
        const double maxAbsRate = ScratchResampler::kMaximumTrackingRate * oneX;
        // Extrapolate by the target's real age. Using the same half-interval on
        // every callback made blocks between MIDI events alternately coast and
        // brake against an unchanged target — the audible scratch judder.
        const double usedRate = m_scratchResampler.processScratchTracking(
            target, commandedRate, maxAbsRate, bufferToFill, inputLead,
            accumulatedLeadSamples, maxReferenceCorrectionRate,
            tickCorrectionTimeSeconds);
        const double readPositionSamples = m_scratchResampler.readPosition();
        const bool staleReleaseBlock = m_audioReleaseCommand.generation != 0
            && m_audioReleaseCommand.generation
                <= m_cancelledReleaseGeneration.load(std::memory_order_acquire);
        if (!staleReleaseBlock) {
            m_controller.setMeasuredNormalizedSpeed(
                usedRate / std::max(1e-9, oneX));
        }
        captureScratchTail(bufferToFill);
        publishScratchCursor(readPositionSamples, sr);
        resolveScratchRelease();
        completeCursorHandoffAfterScratchBlock(m_cursorHandoffSampleRate);
        return;
    }

    const double rate = activePlaybackRate(sr, bufferToFill.numSamples);
    const bool landingPhase = m_audioReleasePhase == ScratchReleasePhase::CoastToDeck
        || (m_deckLandingStarted && m_audioReleasePhase == ScratchReleasePhase::HandoffPending);
    const bool landAtDeckTempo = landingPhase && m_controller.handoffPending()
        && m_audioReleaseCommand.playbackIntent
        && m_scratchResampler.currentRate() * signedDeckTempoRatio() > 0.0;
    if (landAtDeckTempo) {
        const double targetRate = signedDeckTempoRatio() * oneX;
        if (!m_deckLandingStarted || m_scratchResampler.rateLandingTarget() != targetRate) {
            const double rateDifference = std::abs(m_scratchResampler.currentRate() - targetRate)
                / std::max(1.0e-9, oneX);
            const double seconds = std::clamp(rateDifference * 0.20, 0.120, 0.300);
            m_scratchResampler.beginRateLanding(targetRate,
                static_cast<int>(std::lround(seconds * m_outputSampleRate)));
            m_deckLandingStarted = true;
        }
    } else if (m_deckLandingStarted) {
        m_scratchResampler.cancelRateLanding();
        m_deckLandingStarted = false;
    }
    m_scratchResampler.processBlock(rate, bufferToFill);
    const double readPositionSamples = m_scratchResampler.readPosition();
    captureScratchTail(bufferToFill);
    publishScratchCursor(readPositionSamples, sr);
    resolveScratchRelease();
    finishCoastHandoffAfterScratchBlock(sr);
    completeCursorHandoffAfterScratchBlock(m_cursorHandoffSampleRate);
}

} // namespace engine::audio
