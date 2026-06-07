#include "ScratchDeckBridge.hpp"

#include "HermiteResamplingAudioSource.h"

namespace engine::audio {

ScratchDeckBridge::ScratchDeckBridge(juce::AudioSource* inputSource, bool deleteInputWhenDeleted)
    : m_transport(inputSource, deleteInputWhenDeleted)
{
    if (m_transport)
        m_hermite = std::make_unique<HermiteResamplingAudioSource>(m_transport.get(), false);
}

void ScratchDeckBridge::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    m_outputSampleRate = std::max(1.0, sampleRate);
    m_blockSize = std::max(64, samplesPerBlockExpected);
    // Hermite prepares the transport input once; avoid double prepare/release.
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

    double rate = m_deckTempoRatio.load(std::memory_order_relaxed);
    if (m_reverse.load(std::memory_order_relaxed))
        rate = -rate;
    m_hermite->setResamplingRatio(rate);
}

bool ScratchDeckBridge::isScratchPathActive() const noexcept
{
    return m_useScratchScaler
        || m_controller.isScratching()
        || m_controller.isInertiaActive();
}

void ScratchDeckBridge::beginScratch(double anchorSeconds, double trackSampleRate, double trackLengthSeconds)
{
    m_trackSampleRate.store(std::max(1.0, trackSampleRate), std::memory_order_relaxed);
    m_trackLengthSeconds.store(std::max(0.0, trackLengthSeconds), std::memory_order_relaxed);

    const double audioAnchorSec = std::max(0.0, anchorSeconds);
    const double audioAnchorSamples = audioAnchorSec * trackSampleRate;
    const double targetSamples = anchorSeconds * trackSampleRate;
    m_controller.setTrackSampleRate(trackSampleRate);
    m_platter.reset(targetSamples, trackSampleRate);
    m_platter.setSamplesPerTick((60.0 / (33.0 + 1.0 / 3.0) / 12000.0) * trackSampleRate);
    m_controller.startScratch(audioAnchorSamples, targetSamples);
    m_scratchResampler.setTrackLengthSamples(trackLengthSeconds * trackSampleRate);
    m_scratchResampler.reset(audioAnchorSamples);
    m_useScratchScaler = true;

    if (auto* positionable = dynamic_cast<juce::PositionableAudioSource*>(m_scratchInput)) {
        positionable->setNextReadPosition(static_cast<juce::int64>(std::llround(audioAnchorSamples)));
    } else if (m_scratchInput == nullptr && m_transport != nullptr) {
        if (auto* positionable = dynamic_cast<juce::PositionableAudioSource*>(m_transport.get()))
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
    const double targetPos = m_controller.targetSamplePosition();
    m_controller.startScratch(audioPos, targetPos);
    m_useScratchScaler = true;
}

void ScratchDeckBridge::syncTargetFromPlatter(const engine::scratch::VirtualTurntable& platter) noexcept
{
    m_controller.setTargetSamplePosition(platter.targetSamplePosition());
}

void ScratchDeckBridge::addTargetDeltaSeconds(double deltaSeconds, double trackSampleRate) noexcept
{
    m_platter.addTimeDeltaSeconds(deltaSeconds);
    m_controller.addTargetSampleDelta(deltaSeconds * trackSampleRate);
}

void ScratchDeckBridge::setAbsoluteTargetSeconds(double seconds, double trackSampleRate) noexcept
{
    m_platter.setAbsoluteTimeSeconds(seconds);
    m_controller.setTargetSamplePosition(seconds * trackSampleRate);
    m_controller.notifyTargetMoved();
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
    m_scratchResampler.reset(positionSeconds * sr);

    juce::PositionableAudioSource* positionable = nullptr;
    if (m_scratchInput != nullptr)
        positionable = dynamic_cast<juce::PositionableAudioSource*>(m_scratchInput);
    else if (m_transport != nullptr)
        positionable = dynamic_cast<juce::PositionableAudioSource*>(m_transport.get());

    if (positionable != nullptr) {
        positionable->setNextReadPosition(
            static_cast<juce::int64>(std::llround(std::max(0.0, positionSeconds * sr))));
    }

    if (m_hermite) {
        applyDeckTempoToHermite();
        m_hermite->resetStream();
        m_hermite->snapSmoothedRatio();
    }
}

void ScratchDeckBridge::exitScratchMode(double positionSeconds, double trackSampleRate) noexcept
{
    m_controller.stopScratch();
    m_useScratchScaler = false;
    m_prevScratchPath = false;
    m_crossfadeRemaining.store(0, std::memory_order_relaxed);
    syncReadPositionSeconds(positionSeconds, trackSampleRate);
}

void ScratchDeckBridge::setDeckTempoRatio(double ratio) noexcept
{
    m_deckTempoRatio.store(std::clamp(ratio, 0.01, 8.0), std::memory_order_relaxed);
    applyDeckTempoToHermite();
}

void ScratchDeckBridge::setReverse(bool reverse) noexcept
{
    m_reverse.store(reverse, std::memory_order_relaxed);
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

void ScratchDeckBridge::tickControlThread(double dtSeconds) noexcept
{
    m_controller.tickInertia(dtSeconds);
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
    const double sr = std::max(1.0, m_trackSampleRate.load(std::memory_order_relaxed));
    double rate = m_controller.rate() * (sr / m_outputSampleRate);
    if (m_reverse.load(std::memory_order_relaxed))
        rate = -rate;
    return rate;
}

double ScratchDeckBridge::readPositionSeconds(double trackSampleRate) const noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    return m_scratchResampler.readPosition() / sr;
}

double ScratchDeckBridge::targetPositionSeconds(double trackSampleRate) const noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    return m_controller.targetSamplePosition() / sr;
}

void ScratchDeckBridge::syncReadToTarget(double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double targetSamples = m_controller.targetSamplePosition();
    const double audioSamples = std::max(0.0, targetSamples);
    m_scratchResampler.reset(audioSamples);
    m_scratchResampler.snapSmoothedRate(m_controller.rate());
    m_controller.startScratch(audioSamples, targetSamples);
}

void ScratchDeckBridge::snapHermiteToDeckTempo() noexcept
{
    if (!m_hermite)
        return;
    applyDeckTempoToHermite();
    m_hermite->resetStream();
    m_hermite->snapSmoothedRatio();
}

double ScratchDeckBridge::activePlaybackRate(double trackSampleRate) noexcept
{
    const double sr = std::max(1.0, trackSampleRate);
    const double audioPos = m_scratchResampler.readPosition();
    const double baseRatio = m_outputSampleRate / sr;

    const double pdRate = m_controller.processAudioBlock(audioPos,
                                                         m_blockSize,
                                                         m_outputSampleRate,
                                                         baseRatio,
                                                         m_loopActive,
                                                         m_loopInSample,
                                                         m_loopOutSample);

    // PD rate is tuned like deck varispeed (device-rate samples / output sample).
    // The scratch resampler reads raw track samples, so convert to track samples / output sample.
    double rate = pdRate * (sr / m_outputSampleRate);
    if (m_reverse.load(std::memory_order_relaxed))
        rate = -rate;
    return rate;
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
            && !m_controller.isScratching()
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
    const double rate = activePlaybackRate(sr);
    m_scratchResampler.processBlock(*scratchInput, rate, bufferToFill);
}

} // namespace engine::audio
