#include "ScratchDeckBridge.hpp"

#include "HermiteResamplingAudioSource.h"

namespace engine::audio {

ScratchDeckBridge::ScratchDeckBridge(juce::AudioSource* inputSource, bool deleteInputWhenDeleted)
    : m_transport(inputSource, deleteInputWhenDeleted)
{
    if (m_transport) {
        m_positionableTransportSource = dynamic_cast<juce::PositionableAudioSource*>(m_transport.get());
        m_hermite = std::make_unique<HermiteResamplingAudioSource>(m_transport.get(), false);
    }
}

ScratchDeckBridge::~ScratchDeckBridge() = default;

void ScratchDeckBridge::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
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

void ScratchDeckBridge::releaseResources()
{
    if (m_hermite)
        m_hermite->releaseResources();
    else if (m_transport)
        m_transport->releaseResources();
}

void ScratchDeckBridge::applyDeckTempoToHermite() noexcept
{
    if (!m_hermite)
        return;

    m_hermite->setResamplingRatio(effectiveDeckTempoRatio());
}

double ScratchDeckBridge::effectiveDeckTempoRatio() const noexcept
{
    // Direction is carried by CachedPlaybackAudioSource.  Hermite/JUCE
    // resampling remains a positive-rate boundary; a negative ratio here would
    // apply reverse a second time and leaves its forward-oriented pull path
    // starved on several block sizes.
    return std::abs(m_deckTempoRatio.load(std::memory_order_relaxed)
                    * m_jogNudgeRatio.load(std::memory_order_relaxed));
}

bool ScratchDeckBridge::isScratchPathActive() const noexcept
{
    return m_useScratchScaler.load(std::memory_order_acquire)
        || m_controller.isActive()
        || m_controller.isInertiaActive();
}

void ScratchDeckBridge::beginScratch(double anchorSeconds,
                                     double trackSampleRate,
                                     double trackLengthSeconds,
                                     bool wasPlayingBeforeScratch,
                                     double normalPlaybackSpeed)
{
    m_trackSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_trackLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);

    const double audioAnchorSec = std::max(0.0, anchorSeconds);
    const double audioAnchorSamples = audioAnchorSec * trackSampleRate;
    const double targetSamples = audioAnchorSec * trackSampleRate;
    const double playbackSpeed = m_reverse.load(std::memory_order_relaxed)
        ? -std::abs(normalPlaybackSpeed)
        : std::abs(normalPlaybackSpeed);
    m_controller.setTrackSampleRate(trackSampleRate);
    m_platter.reset(targetSamples, trackSampleRate);
    m_platter.setSamplesPerTick((60.0 / (33.0 + 1.0 / 3.0) / 1500.0) * trackSampleRate);
    m_controller.startScratch(audioAnchorSamples, wasPlayingBeforeScratch, playbackSpeed);
    m_controller.setHandPositionSec(anchorSeconds);
    m_startPositionSeconds.store(audioAnchorSec, std::memory_order_relaxed);
    m_startSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_startLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
    m_startCommandGeneration.fetch_add(1, std::memory_order_release);
}

engine::scratch::ScratchReleaseDisposition ScratchDeckBridge::endScratch(bool allowInertia)
{
    if (allowInertia)
        return m_controller.releaseScratch();

    m_controller.stopScratch();
    return engine::scratch::ScratchReleaseDisposition::HandoffNow;
}

void ScratchDeckBridge::engageScratchDuringInertia() noexcept
{
    if (!m_controller.isInertiaActive())
        return;

    const bool wasPlaying = m_controller.wasPlayingBeforeScratch();
    const double normalSpeed = effectiveDeckTempoRatio();
    // The callback remains the sole owner of the resampler state. The controller
    // preserves its inertia velocity, so re-grabbing stays continuous without a
    // control-thread read-head reset.
    m_controller.startScratch(m_controller.readPositionSamples(), wasPlaying, normalSpeed);
    m_useScratchScaler.store(true, std::memory_order_release);
}

void ScratchDeckBridge::addTargetDeltaSeconds(double deltaSeconds, double trackSampleRate) noexcept
{
    m_platter.addTimeDeltaSeconds(deltaSeconds);
    const double next = m_scratchDisplaySec.load(std::memory_order_relaxed) + deltaSeconds;
    m_scratchDisplaySec.store(next, std::memory_order_relaxed);
    juce::ignoreUnused(trackSampleRate);
}

void ScratchDeckBridge::submitHandDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept
{
    m_platter.addTimeDeltaSeconds(deltaSeconds);
    m_controller.submitHandDelta(deltaSeconds, dtSeconds);
}

void ScratchDeckBridge::submitReleaseDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept
{
    m_controller.submitReleaseDelta(deltaSeconds, dtSeconds);
}

void ScratchDeckBridge::syncScratchReadPosition(double displaySec, double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double audioSec = std::max(0.0, displaySec);
    const double audioSamples = audioSec * sr;

    m_readerSyncPositionSeconds.store(audioSec, std::memory_order_relaxed);
    m_readerSyncSampleRate.store(sr, std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
    m_controller.syncReadPositionSamples(audioSamples);
    m_controller.setHandPositionSec(displaySec);

    if (m_audioPlayheadSink != nullptr)
        m_audioPlayheadSink->store(displaySec, std::memory_order_release);
    m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);
}

void ScratchDeckBridge::publishScratchDisplay(double displaySec) noexcept
{
    // Continuous touch updates from the UI thread. The audio thread's position
    // tracker owns the read head (no readPos slam here → no UI/audio fight), so we
    // only publish the visible playhead and the hand position.
    m_controller.setHandPositionSec(displaySec);
    m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);
    if (m_audioPlayheadSink != nullptr)
        m_audioPlayheadSink->store(displaySec, std::memory_order_release);

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

void ScratchDeckBridge::configureTrack(double trackSampleRate, double trackLengthSeconds) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    m_trackSampleRate.store(sr, std::memory_order_relaxed);
    m_trackLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
    m_controller.setTrackSampleRate(sr);
    m_startLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
}

void ScratchDeckBridge::syncReadPositionSeconds(double positionSeconds, double trackSampleRate) noexcept
{
    m_readerSyncPositionSeconds.store(std::max(0.0, positionSeconds), std::memory_order_relaxed);
    m_readerSyncSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
}

void ScratchDeckBridge::prepareNormalPlaybackHandoff(double positionSeconds, double trackSampleRate) noexcept
{
    // The callback owns both the scratch reader and JUCE transport. Publishing a
    // generation here prevents a control-thread reset from racing a live block.
    m_handoffPositionSeconds.store(std::max(0.0, positionSeconds), std::memory_order_relaxed);
    m_handoffSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_handoffCommandGeneration.fetch_add(1, std::memory_order_release);
}

void ScratchDeckBridge::exitScratchMode(double positionSeconds, double trackSampleRate) noexcept
{
    prepareNormalPlaybackHandoff(positionSeconds, trackSampleRate);
}

void ScratchDeckBridge::setDeckTempoRatio(double ratio) noexcept
{
    const double clamped = std::clamp(ratio, 0.01, 8.0);
    m_deckTempoRatio.store(clamped, std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(effectiveDeckTempoRatio());
    applyDeckTempoToHermite();
}

void ScratchDeckBridge::setJogNudgeRatio(double ratio) noexcept
{
    m_jogNudgeRatio.store(std::clamp(ratio, 0.94, 1.06), std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(effectiveDeckTempoRatio());
    applyDeckTempoToHermite();
}

void ScratchDeckBridge::setReverse(bool reverse) noexcept
{
    m_reverse.store(reverse, std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(effectiveDeckTempoRatio());
    applyDeckTempoToHermite();
}

void ScratchDeckBridge::setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                                          double trackSampleRate) noexcept
{
    m_loopInSample.store(loopInSec * trackSampleRate, std::memory_order_relaxed);
    m_loopOutSample.store(loopOutSec * trackSampleRate, std::memory_order_relaxed);
    m_loopActive.store(active, std::memory_order_release);
    m_loopCommandGeneration.fetch_add(1, std::memory_order_release);
}

void ScratchDeckBridge::setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle) noexcept
{
    m_scratchResampler.setTrackCacheSource(cache, handle);
}

bool ScratchDeckBridge::isScratching() const noexcept
{
    return m_controller.isScratching();
}

bool ScratchDeckBridge::isInertiaActive() const noexcept
{
    return m_controller.isInertiaActive();
}

double ScratchDeckBridge::scratchRate() const noexcept
{
    return m_controller.normalizedRate();
}

double ScratchDeckBridge::readPositionSeconds(double trackSampleRate) const noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    return m_audioScratchReadPositionSamples.load(std::memory_order_acquire) / sr;
}

double ScratchDeckBridge::displayPositionSeconds() const noexcept
{
    return m_scratchDisplaySec.load(std::memory_order_relaxed);
}

void ScratchDeckBridge::snapHermiteToDeckTempo() noexcept
{
    applyDeckTempoToHermite();
    m_readerSyncPositionSeconds.store(std::max(0.0, displayPositionSeconds()), std::memory_order_relaxed);
    m_readerSyncSampleRate.store(m_trackSampleRate.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_readerSyncGeneration.fetch_add(1, std::memory_order_release);
}

void ScratchDeckBridge::consumePendingAudioCommands() noexcept
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
        m_appliedStartCommandGeneration = startGeneration;
    }

    const auto handoffGeneration = m_handoffCommandGeneration.load(std::memory_order_acquire);
    if (handoffGeneration != m_appliedHandoffCommandGeneration) {
        const double sr = std::max(1.0, m_handoffSampleRate.load(std::memory_order_relaxed));
        const double position = std::max(0.0, m_handoffPositionSeconds.load(std::memory_order_relaxed));
        const auto samplePos = static_cast<juce::int64>(std::llround(position * sr));
        m_scratchResampler.reset(position * sr);
        m_audioScratchReadPositionSamples.store(position * sr, std::memory_order_release);
        if (m_positionableTransportSource)
            m_positionableTransportSource->setNextReadPosition(samplePos);
        if (m_hermite) {
            applyDeckTempoToHermite();
            m_hermite->resetStream();
            m_hermite->snapSmoothedRatio();
        }
        m_controller.stopScratch();
        m_useScratchScaler.store(false, std::memory_order_release);
        m_prevScratchPath = false;
        m_appliedHandoffCommandGeneration = handoffGeneration;
        // A queued scratch-position sync from before release must not seek the
        // normal transport back after the handoff has selected its final frame.
        m_appliedReaderSyncGeneration = m_readerSyncGeneration.load(std::memory_order_acquire);
    }

    const auto syncGeneration = m_readerSyncGeneration.load(std::memory_order_acquire);
    if (syncGeneration != m_appliedReaderSyncGeneration) {
        const double sr = std::max(1.0, m_readerSyncSampleRate.load(std::memory_order_relaxed));
        const double position = std::max(0.0, m_readerSyncPositionSeconds.load(std::memory_order_relaxed));
        const auto samplePos = static_cast<juce::int64>(std::llround(position * sr));
        m_scratchResampler.reset(position * sr);
        m_audioScratchReadPositionSamples.store(position * sr, std::memory_order_release);
        if (m_positionableTransportSource)
            m_positionableTransportSource->setNextReadPosition(samplePos);
        if (m_hermite) {
            applyDeckTempoToHermite();
            m_hermite->resetStream();
            m_hermite->snapSmoothedRatio();
        }
        m_appliedReaderSyncGeneration = syncGeneration;
    }
}

double ScratchDeckBridge::activePlaybackRate(double trackSampleRate, int bufferSize) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    return m_controller.processAudioBlock(std::max(1, bufferSize), m_outputSampleRate, sr);
}

void ScratchDeckBridge::applyNormalPathCrossfade(const juce::AudioSourceChannelInfo& info) noexcept
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

void ScratchDeckBridge::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (!m_transport || !bufferToFill.buffer) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    if (m_transportSwapInProgress.load(std::memory_order_acquire)) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    consumePendingAudioCommands();

    if (m_useScratchScaler.load(std::memory_order_acquire)
            && !m_controller.isActive()
            && !m_controller.isInertiaActive()) {
        m_useScratchScaler.store(false, std::memory_order_release);
    }

    const bool scratching = isScratchPathActive();
    m_prevScratchPath = scratching;

    if (!scratching) {
        if (m_keylockPassthrough.load(std::memory_order_relaxed)) {
            m_transport->getNextAudioBlock(bufferToFill);
        } else if (m_hermite) {
            m_hermite->getNextAudioBlock(bufferToFill);
        } else {
            m_transport->getNextAudioBlock(bufferToFill);
        }
        applyNormalPathCrossfade(bufferToFill);
        return;
    }

    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double oneX = sr / std::max(1.0, m_outputSampleRate);

    if (m_controller.touching() && !m_loopActive.load(std::memory_order_acquire)) {
        // Position-authoritative scratch: a critically-damped tracker glides the
        // read head to the hand target. Exact tracking for slow/precise moves,
        // momentum across sparse UI events, no overshoot/snap-back warble.
        const double target = m_platter.targetSamplePosition();
        const double maxAbsRate = 8.0 * oneX;
        const double usedRate = m_scratchResampler.processScratchTracking(
            target, maxAbsRate, bufferToFill);
        const double readPositionSamples = m_scratchResampler.readPosition();
        m_audioScratchReadPositionSamples.store(readPositionSamples, std::memory_order_release);
        m_controller.syncReadPositionSamples(readPositionSamples);
        m_controller.setMeasuredNormalizedSpeed(usedRate / std::max(1e-9, oneX));
        // Display is published by the UI thread via publishScratchDisplay().
        return;
    }

    const double rate = activePlaybackRate(sr, bufferToFill.numSamples);
    m_scratchResampler.processBlock(rate, bufferToFill);
    const double readPositionSamples = m_scratchResampler.readPosition();
    m_audioScratchReadPositionSamples.store(readPositionSamples, std::memory_order_release);
    m_controller.syncReadPositionSamples(readPositionSamples);

    // While the finger is down, only the UI thread may publish the visible
    // playhead — republishing a cached value here races with fresh drag deltas.
    if (m_audioPlayheadSink != nullptr && !m_controller.touching()) {
        // On release, publish the resampler's actual cursor instead of a
        // parallel velocity integration. The final normal-playback handoff
        // then starts at exactly the frame that produced this block.
        const double displaySec = readPositionSamples / std::max(1.0, sr);
        m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);
        m_audioPlayheadSink->store(displaySec, std::memory_order_relaxed);
    }
}

} // namespace engine::audio
