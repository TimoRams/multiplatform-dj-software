#include "ScratchDeckBridge.hpp"

#include "HermiteResamplingAudioSource.h"

#include <juce_audio_formats/juce_audio_formats.h>

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
    const double rate = m_deckTempoRatio.load(std::memory_order_relaxed);
    return m_reverse.load(std::memory_order_relaxed) ? -rate : rate;
}

juce::PositionableAudioSource* ScratchDeckBridge::positionableScratchSource() const noexcept
{
    if (m_scratchInput != nullptr)
        return m_positionableScratchInput;
    return m_positionableTransportSource;
}

bool ScratchDeckBridge::isScratchPathActive() const noexcept
{
    return m_useScratchScaler
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
    const double targetSamples = anchorSeconds * trackSampleRate;
    const double playbackSpeed = m_reverse.load(std::memory_order_relaxed)
        ? -std::abs(normalPlaybackSpeed)
        : std::abs(normalPlaybackSpeed);
    m_controller.setTrackSampleRate(trackSampleRate);
    m_platter.reset(targetSamples, trackSampleRate);
    m_platter.setSamplesPerTick((60.0 / (33.0 + 1.0 / 3.0) / 1500.0) * trackSampleRate);
    m_controller.startScratch(audioAnchorSamples, wasPlayingBeforeScratch, playbackSpeed);
    m_controller.setHandPositionSec(anchorSeconds);
    m_scratchResampler.setTrackLengthSamples(trackLengthSeconds * trackSampleRate);
    m_scratchResampler.reset(audioAnchorSamples);
    m_scratchResampler.snapSmoothedRate(0.0);
    m_useScratchScaler = true;

    if (auto* positionable = positionableScratchSource()) {
        positionable->setNextReadPosition(static_cast<juce::int64>(std::llround(audioAnchorSamples)));
    }
}

void ScratchDeckBridge::endScratch(bool allowInertia)
{
    if (allowInertia)
        m_controller.releaseScratch();
    else
        m_controller.stopScratch();

    if (!m_controller.isInertiaActive())
        m_useScratchScaler = false;
}

void ScratchDeckBridge::engageScratchDuringInertia() noexcept
{
    if (!m_controller.isInertiaActive())
        return;

    const double audioPos = m_scratchResampler.readPosition();
    const bool wasPlaying = m_controller.wasPlayingBeforeScratch();
    const double normalSpeed = effectiveDeckTempoRatio();
    // Seed the position tracker with the current inertia velocity so re-grabbing a
    // spinning platter is continuous instead of snapping to a standstill.
    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double oneX = sr / std::max(1.0, m_outputSampleRate);
    m_scratchResampler.primeTrackerVelocity(m_controller.normalizedRate() * oneX);
    m_controller.startScratch(audioPos, wasPlaying, normalSpeed);
    m_useScratchScaler = true;
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

void ScratchDeckBridge::syncScratchReadPosition(double displaySec, double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double audioSec = std::max(0.0, displaySec);
    const double audioSamples = audioSec * sr;

    m_scratchResampler.setReadPositionSamples(audioSamples);
    m_controller.syncReadPositionSamples(audioSamples);
    m_controller.setHandPositionSec(displaySec);

    const double resamplerRate = m_controller.smoothedSpeed()
        * (sr / std::max(1.0, m_outputSampleRate));
    m_scratchResampler.snapSmoothedRate(resamplerRate);

    const juce::int64 samplePos = static_cast<juce::int64>(std::llround(audioSamples));
    if (auto* positionable = positionableScratchSource())
        positionable->setNextReadPosition(samplePos);

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

    if (!m_loopActive)
        return;

    // Loop-while-scratching uses the rate-integration path; keep the read head
    // anchored to the hand position as before.
    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double audioSamples = std::max(0.0, displaySec) * sr;
    m_scratchResampler.setReadPositionSamples(audioSamples);
    m_controller.syncReadPositionSamples(audioSamples);
    const double resamplerRate = m_controller.smoothedSpeed()
        * (sr / std::max(1.0, m_outputSampleRate));
    m_scratchResampler.snapSmoothedRate(resamplerRate);
    if (auto* positionable = positionableScratchSource())
        positionable->setNextReadPosition(static_cast<juce::int64>(std::llround(audioSamples)));
}

void ScratchDeckBridge::configureTrack(double trackSampleRate, double trackLengthSeconds) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    m_trackSampleRate.store(sr, std::memory_order_relaxed);
    m_trackLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);
    m_controller.setTrackSampleRate(sr);
    m_scratchResampler.setTrackLengthSamples(trackLengthSeconds * sr);
}

void ScratchDeckBridge::syncReadPositionSeconds(double positionSeconds, double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double audioSec = std::max(0.0, positionSeconds);
    const juce::int64 samplePos = static_cast<juce::int64>(std::llround(audioSec * sr));
    m_scratchResampler.reset(audioSec * sr);

    if (auto* positionable = positionableScratchSource()) {
        positionable->setNextReadPosition(samplePos);
    }

    if (m_positionableTransportSource) {
        m_positionableTransportSource->setNextReadPosition(samplePos);
    }

    if (m_hermite) {
        applyDeckTempoToHermite();
        m_hermite->resetStream();
        m_hermite->snapSmoothedRatio();
    }
}

void ScratchDeckBridge::prepareNormalPlaybackHandoff(double positionSeconds, double trackSampleRate) noexcept
{
    // Hand/scratch session position is authoritative at release — the resampler
    // read head can drift from rate integration between UI drag events.
    const double syncSec = std::max(0.0, positionSeconds);
    syncReadPositionSeconds(syncSec, trackSampleRate);
    m_controller.stopScratch();
    m_useScratchScaler = false;
    m_prevScratchPath = false;
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

void ScratchDeckBridge::setReverse(bool reverse) noexcept
{
    m_reverse.store(reverse, std::memory_order_relaxed);
    m_controller.setNormalPlaybackSpeed(effectiveDeckTempoRatio());
    applyDeckTempoToHermite();
}

void ScratchDeckBridge::setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                                          double trackSampleRate) noexcept
{
    m_loopActive = active;
    m_loopInSample = loopInSec * trackSampleRate;
    m_loopOutSample = loopOutSec * trackSampleRate;
    m_scratchResampler.setLoopRange(m_loopInSample, m_loopOutSample, active);
}

void ScratchDeckBridge::setScratchInputSource(juce::AudioSource* source) noexcept
{
    m_scratchInput = source;
    m_positionableScratchInput = dynamic_cast<juce::PositionableAudioSource*>(source);

    juce::AudioFormatReader* reader = nullptr;
    if (auto* readerSource = dynamic_cast<juce::AudioFormatReaderSource*>(source))
        reader = readerSource->getAudioFormatReader();
    m_scratchResampler.setFormatReader(reader);
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
    return m_scratchResampler.readPosition() / sr;
}

double ScratchDeckBridge::displayPositionSeconds() const noexcept
{
    return m_scratchDisplaySec.load(std::memory_order_relaxed);
}

void ScratchDeckBridge::snapHermiteToDeckTempo() noexcept
{
    if (!m_hermite)
        return;
    applyDeckTempoToHermite();
    m_hermite->resetStream();
    m_hermite->snapSmoothedRatio();
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

    if (m_useScratchScaler
            && !m_controller.isActive()
            && !m_controller.isInertiaActive()) {
        m_useScratchScaler = false;
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

    juce::AudioSource* scratchInput = m_scratchInput != nullptr ? m_scratchInput : m_transport.get();
    if (!scratchInput) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const double sr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double oneX = sr / std::max(1.0, m_outputSampleRate);

    if (m_controller.touching() && !m_loopActive) {
        // Position-authoritative scratch: a critically-damped tracker glides the
        // read head to the hand target. Exact tracking for slow/precise moves,
        // momentum across sparse UI events, no overshoot/snap-back warble.
        const double target = m_platter.targetSamplePosition();
        const double maxAbsRate = 8.0 * oneX;
        const double usedRate = m_scratchResampler.processScratchTracking(
            *scratchInput, target, maxAbsRate, bufferToFill);
        m_controller.setMeasuredNormalizedSpeed(usedRate / std::max(1e-9, oneX));
        // Display is published by the UI thread via publishScratchDisplay().
        return;
    }

    const double rate = activePlaybackRate(sr, bufferToFill.numSamples);
    m_scratchResampler.processBlock(*scratchInput, rate, bufferToFill);

    // While the finger is down, only the UI thread may publish the visible
    // playhead — republishing a cached value here races with fresh drag deltas.
    if (m_audioPlayheadSink != nullptr && !m_controller.touching()) {
        const double normRate = m_controller.normalizedRate();
        const double blockSec = static_cast<double>(bufferToFill.numSamples)
                              / std::max(1.0, m_outputSampleRate);
        double displaySec = m_scratchDisplaySec.load(std::memory_order_relaxed);
        if (m_controller.isInertiaActive())
            displaySec += normRate * blockSec;
        m_scratchDisplaySec.store(displaySec, std::memory_order_relaxed);
        m_audioPlayheadSink->store(displaySec, std::memory_order_relaxed);
    }
}

} // namespace engine::audio
