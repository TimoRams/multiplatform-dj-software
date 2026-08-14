#include "RenderModeRouter.h"

#include "audio/cache/CachedPlaybackAudioSource.h"
#include "audio/internal/HermiteResamplingAudioSource.h"

#include <thread>

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

void RenderModeRouter::beginTransportSwap() noexcept
{
    // Publish the gate before observing callback activity. Sequential
    // consistency prevents the swap thread and a newly entering callback from
    // both missing each other's store. Callbacks never wait; a track swap waits
    // only for the block that already owns the reader pointer.
    m_transportSwapInProgress.store(true, std::memory_order_seq_cst);
    while (m_audioCallbacksActive.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
}

void RenderModeRouter::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    m_outputSampleRate = std::max(1.0, sampleRate);
    m_blockSize = std::max(64, samplesPerBlockExpected);
    if (m_hermite)
        m_hermite->prepareToPlay(samplesPerBlockExpected, sampleRate);
    else if (m_transport)
        m_transport->prepareToPlay(samplesPerBlockExpected, sampleRate);
    m_scratchResampler.prepare(2, m_blockSize, m_outputSampleRate);
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

    const double audioAnchorSec = std::max(0.0, anchorSeconds);
    const double audioAnchorSamples = audioAnchorSec * trackSampleRate;
    const double targetSamples = audioAnchorSec * trackSampleRate;
    m_scratchResampler.prefetchAround(audioAnchorSamples);
    const double playbackSpeed = m_reverse.load(std::memory_order_relaxed)
        ? -std::abs(normalPlaybackSpeed)
        : std::abs(normalPlaybackSpeed);
    m_controller.setTrackSampleRate(trackSampleRate);
    m_platter.reset(targetSamples, trackSampleRate);
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
    // complete snapshot is readable, guaranteeing exactly one final tracking
    // block before it decides whether to coast or hand off.
    m_requestedReleaseGeneration.store(generation, std::memory_order_release);
    m_releaseCommandSequence.store(oddSequence + 1, std::memory_order_release);
    return generation;
}

void RenderModeRouter::submitScratchReleaseSpeed(double normalizedReleaseSpeed) noexcept
{
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

void RenderModeRouter::submitHandDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept
{
    m_platter.addTimeDeltaSeconds(deltaSeconds);
    m_controller.submitHandDelta(deltaSeconds, dtSeconds);
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
    // Continuous touch updates from the UI thread. The audio thread's position
    // tracker owns the read head (no readPos slam here → no UI/audio fight), so we
    // only publish the visible playhead and the hand position.
    m_controller.setHandPositionSec(displaySec);
    m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);

    if (!m_loopActive.load(std::memory_order_acquire))
        return;

    // Loop-while-scratching uses the rate-integration path; keep the read head
    // anchored to the hand position as before.
    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double audioSamples = std::max(0.0, displaySec) * sr;
    m_readerSyncPositionSeconds.store(std::max(0.0, displaySec), std::memory_order_relaxed);
    m_readerSyncSampleRate.store(sr, std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
    m_controller.syncReadPositionSamples(audioSamples);
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
}

void RenderModeRouter::setJogNudgeRatio(double ratio) noexcept
{
    m_jogNudgeRatio.store(std::clamp(ratio, 0.94, 1.06), std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(signedDeckTempoRatio());
    applyDeckTempoToHermite();
}

void RenderModeRouter::setReverse(bool reverse) noexcept
{
    m_reverse.store(reverse, std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(signedDeckTempoRatio());
    applyDeckTempoToHermite();
}

void RenderModeRouter::setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                                          double trackSampleRate) noexcept
{
    m_loopInSample.store(loopInSec * trackSampleRate, std::memory_order_relaxed);
    m_loopOutSample.store(loopOutSec * trackSampleRate, std::memory_order_relaxed);
    m_loopActive.store(active, std::memory_order_release);
    m_loopCommandGeneration.fetch_add(1, std::memory_order_release);
}

void RenderModeRouter::setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle) noexcept
{
    m_scratchResampler.setTrackCacheSource(cache, handle);
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

    const double sr = std::max(1.0, trackSampleRate);
    const double position = std::max(0.0, positionSeconds);
    // AudioTransportSource's PositionableAudioSource API is expressed in its
    // prepared output-rate samples and converts to the track rate internally.
    // Passing track samples here applies the conversion twice (e.g. 44.1/48)
    // and makes every scratch release jump backwards.
    const auto transportSamplePos = static_cast<juce::int64>(
        std::llround(position * std::max(1.0, m_outputSampleRate)));
    const auto sourceSamplePos = static_cast<juce::int64>(std::llround(position * sr));
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

void RenderModeRouter::finishReleaseDecisionAfterTrackingBlock() noexcept
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
    if (!std::isfinite(speed))
        speed = m_controller.normalizedRate();
    m_audioReleaseDisposition = m_controller.releaseScratchWithSpeed(
        speed,
        m_audioReleaseCommand.allowInertia,
        m_audioReleaseCommand.deckRate,
        m_audioReleaseCommand.playbackIntent);

    switch (m_audioReleaseDisposition) {
    case engine::scratch::ScratchReleaseDisposition::CoastToDeckRate:
        m_audioReleasePhase = ScratchReleasePhase::CoastToDeck;
        m_scratchResampler.snapSmoothedRate(
            speed * std::max(1.0, m_audioReleaseCommand.sampleRate)
            / std::max(1.0, m_outputSampleRate));
        break;
    case engine::scratch::ScratchReleaseDisposition::CoastToStop:
        m_audioReleasePhase = ScratchReleasePhase::CoastToStop;
        m_scratchResampler.snapSmoothedRate(
            speed * std::max(1.0, m_audioReleaseCommand.sampleRate)
            / std::max(1.0, m_outputSampleRate));
        break;
    case engine::scratch::ScratchReleaseDisposition::HandoffNow:
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
        && m_audioReleasePhase != ScratchReleasePhase::CoastToStop) {
        return;
    }

    if (m_controller.handoffPending())
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
        m_scratchResampler.snapSmoothedRate(0.0);
        m_audioScratchReadPositionSamples.store(position * sr, std::memory_order_release);
        m_useScratchScaler.store(true, std::memory_order_release);
        if (m_audioReleaseCommand.generation
            <= m_cancelledReleaseGeneration.load(std::memory_order_acquire)) {
            m_audioReleaseCommand = {};
            m_audioReleasePhase = ScratchReleasePhase::Idle;
            m_scratchExitTailPending = false;
            m_tailReleaseGeneration = 0;
        }
        m_appliedStartCommandGeneration = startGeneration;
    }

    consumeScratchReleaseCommand();

    // Pause or track replacement can cancel a release after the callback has
    // already accepted it. Complete that generation before consuming the
    // explicit reader handoff so no stale inertia state survives the command.
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

double RenderModeRouter::activePlaybackRate(double trackSampleRate, int bufferSize) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
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
    const int fadeSamples = std::min(kCrossfadeSamples, n);
    for (int ch = 0; ch < channels; ++ch) {
        float* output = info.buffer->getWritePointer(ch, start);
        const float tail = m_lastScratchOutput[static_cast<std::size_t>(ch)];
        for (int i = 0; i < fadeSamples; ++i) {
            const float normalMix = static_cast<float>(i + 1)
                / static_cast<float>(fadeSamples);
            output[i] = tail + (output[i] - tail) * normalMix;
        }
    }

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
    } callbackActivity { m_audioCallbacksActive };

    if (!m_transport || !bufferToFill.buffer) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    if (m_transportSwapInProgress.load(std::memory_order_seq_cst)) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    consumePendingAudioCommands();

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
            applyScratchExitTail(bufferToFill);
            return;
        }

        // Hermite always owns tempo and jog-rate movement. With keylock active,
        // the downstream RubberBand stage keeps timeRatio at 1 and applies the
        // inverse pitch scale, preserving pitch without losing reader speed.
        if (m_hermite) {
            m_hermite->getNextAudioBlock(bufferToFill);
        } else {
            m_transport->getNextAudioBlock(bufferToFill);
        }
        applyNormalPathCrossfade(bufferToFill);
        applyScratchExitTail(bufferToFill);
        captureNormalTail(bufferToFill);
        m_normalPlaybackWasEnabled = true;
        return;
    }

    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double oneX = sr / std::max(1.0, m_outputSampleRate);

    if ((m_controller.requiresPositionTracking() || m_cursorHandoffPending)
        && !m_loopActive.load(std::memory_order_acquire)) {
        // Position-authoritative scratch: a critically-damped tracker glides the
        // read head to the hand target. Exact tracking for slow/precise moves,
        // momentum across sparse UI events, no overshoot/snap-back warble.
        const double target = m_platter.targetSamplePosition();
        const double commandedRate = m_controller.commandedHandSpeed() * oneX;
        const double maxAbsRate = 8.0 * oneX;
        const double usedRate = m_scratchResampler.processScratchTracking(
            target, commandedRate, maxAbsRate, bufferToFill);
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
        finishReleaseDecisionAfterTrackingBlock();
        completeCursorHandoffAfterScratchBlock(m_cursorHandoffSampleRate);
        return;
    }

    const double rate = activePlaybackRate(sr, bufferToFill.numSamples);
    m_scratchResampler.processBlock(rate, bufferToFill);
    const double readPositionSamples = m_scratchResampler.readPosition();
    captureScratchTail(bufferToFill);
    publishScratchCursor(readPositionSamples, sr);
    finishReleaseDecisionAfterTrackingBlock();
    finishCoastHandoffAfterScratchBlock(sr);
    completeCursorHandoffAfterScratchBlock(m_cursorHandoffSampleRate);
}

} // namespace engine::audio
