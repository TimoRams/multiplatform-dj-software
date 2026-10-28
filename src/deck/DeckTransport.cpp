#include "DeckTransport.h"

#include "audio/DeckAudioPipeline.h"
#include "domain/DomainTypes.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kEndEpsilonSeconds = 0.0001;

enum PublishedFlag : std::uint32_t {
    HasTrack = 1u << 0,
    Playing = 1u << 1,
    AudioRunning = 1u << 2,
    Reverse = 1u << 3,
    Slip = 1u << 4,
    PreRoll = 1u << 5,
    AtTrackEnd = 1u << 6
};
}

DeckTransport::DeckTransport(DeckAudioPipeline& audioPipeline) noexcept
    : m_audioPipeline(audioPipeline)
{
    m_audioPipeline.setAudioPlayheadSink(&m_audioPlayhead);
    publishSnapshot();
}

bool DeckTransport::installPreparedTrack(AudioCacheHandle cacheHandle,
                                         const TrackConfiguration& configuration)
{
    if (configuration.trackGeneration <= m_trackGeneration
        || configuration.sampleRate <= 0.0
        || !std::isfinite(configuration.sampleRate)
        || !std::isfinite(configuration.lengthSeconds))
        return false;
    m_audioPipeline.installPreparedTrack({cacheHandle, configuration.sampleRate,
                                       configuration.trackGeneration});
    const auto installed = m_audioPipeline.transportSnapshot();
    if (installed.trackGeneration != configuration.trackGeneration)
        return false;
    m_hasTrack = true;
    m_trackGeneration = configuration.trackGeneration;
    m_sourceSampleRate = configuration.sampleRate;
    m_trackLengthSeconds = std::max(0.0, configuration.lengthSeconds);
    m_audiblePositionSeconds = 0.0;
    m_backgroundPositionSeconds = 0.0;
    m_heldPositionSeconds = 0.0;
    m_preRollActive = false;
    m_atTrackEnd = false;
    m_audioPipeline.setReverse(m_reverse);
    m_audioPipeline.setPlaybackRate(m_playbackRate);
    m_audioPipeline.setJogNudgeRatio(m_jogNudgeRatio);
    setSnapAnchor(0.0, false);
    if (m_playRequested)
        ensureAudioRunning();
    publishSnapshot();
    return true;
}

void DeckTransport::clearTrack(std::uint64_t invalidThroughGeneration,
                               AudioCacheReleaseMode releaseMode) noexcept
{
    m_audioPipeline.clearTrack(invalidThroughGeneration, releaseMode);
    m_trackGeneration = std::max(m_trackGeneration, invalidThroughGeneration);
    m_hasTrack = false;
    m_playRequested = false;
    m_preRollActive = false;
    m_atTrackEnd = false;
    m_audiblePositionSeconds = 0.0;
    m_backgroundPositionSeconds = 0.0;
    m_heldPositionSeconds = 0.0;
    m_trackLengthSeconds = 0.0;
    setSnapAnchor(0.0, false);
    publishSnapshot();
}

bool DeckTransport::setPlaying(bool playing) noexcept
{
    if (m_playRequested == playing)
        return false;

    m_playRequested = playing;
    if (!playing) {
        m_preRollActive = false;
        freezeAt(visualPositionSeconds(0.0));
    } else if (m_heldPositionSeconds < 0.0) {
        startPreRoll(m_heldPositionSeconds);
    } else {
        ensureAudioRunning();
    }
    publishSnapshot();
    return true;
}

bool DeckTransport::setPlayingDuringScratch(bool playing) noexcept
{
    if (m_playRequested == playing)
        return false;

    m_playRequested = playing;
    m_preRollActive = false;

    // The scratch reader remains authoritative. This command only publishes
    // the target for an in-flight release: Pause coasts to zero, Play coasts to
    // deck rate (or crosses zero before handing off when directions differ).
    m_audioPipeline.setTransportRunning(playing);
    publishSnapshot();
    return true;
}

bool DeckTransport::setReverse(bool enabled) noexcept
{
    if (m_reverse == enabled)
        return false;
    m_reverse = enabled;
    m_audioPipeline.setReverse(enabled);
    publishSnapshot();
    return true;
}

bool DeckTransport::setSlipEnabled(bool enabled) noexcept
{
    if (m_slipEnabled == enabled)
        return false;
    m_slipEnabled = enabled;
    if (enabled)
        m_backgroundPositionSeconds = positionSeconds();
    publishSnapshot();
    return true;
}

void DeckTransport::returnToSlipPosition() noexcept
{
    if (!m_hasTrack)
        return;
    seekAudioToSeconds(std::clamp(m_backgroundPositionSeconds, 0.0, m_trackLengthSeconds));
}

bool DeckTransport::setPlaybackRate(double rate) noexcept
{
    if (!std::isfinite(rate))
        return false;
    rate = std::clamp(rate, 0.01, 8.0);
    if (std::abs(m_playbackRate - rate) < 1.0e-12)
        return false;
    m_playbackRate = rate;
    m_audioPipeline.setPlaybackRate(rate);
    publishSnapshot();
    return true;
}

bool DeckTransport::setJogNudgeRatio(double ratio) noexcept
{
    if (!std::isfinite(ratio))
        return false;
    ratio = std::clamp(ratio, 0.94, 1.06);
    if (std::abs(m_jogNudgeRatio - ratio) < 1.0e-12)
        return false;
    m_jogNudgeRatio = ratio;
    m_audioPipeline.setJogNudgeRatio(ratio);
    publishSnapshot();
    return true;
}

bool DeckTransport::seekToSeconds(double seconds, bool resetSlipPosition) noexcept
{
    if (!m_hasTrack || !std::isfinite(seconds))
        return false;
    const double clamped = std::clamp(seconds, -TransportLimits::kPreRollSeconds,
                                      m_trackLengthSeconds);
    m_preRollActive = false;
    m_atTrackEnd = clamped >= m_trackLengthSeconds - kEndEpsilonSeconds;
    m_audiblePositionSeconds = clamped;
    m_heldPositionSeconds = clamped;
    if (resetSlipPosition || !m_slipEnabled)
        m_backgroundPositionSeconds = clamped;

    if (clamped < 0.0) {
        m_audioPipeline.setTransportRunning(false);
        m_audioPipeline.seekToSeconds(0.0);
        setSnapAnchor(clamped, false);
        if (m_playRequested)
            startPreRoll(clamped);
    } else {
        m_audioPipeline.seekToSeconds(clamped);
        setSnapAnchor(clamped, true);
        armVisualSeekSettle();
        ensureAudioRunning();
    }
    publishSnapshot();
    return true;
}

bool DeckTransport::seekNormalized(double progress) noexcept
{
    if (!std::isfinite(progress) || m_trackLengthSeconds <= 0.0)
        return false;
    return seekToSeconds(progress * m_trackLengthSeconds);
}

void DeckTransport::freezeAt(double seconds) noexcept
{
    if (!std::isfinite(seconds))
        seconds = 0.0;
    m_audioPipeline.setTransportRunning(false);
    m_audioPipeline.seekToSeconds(seconds);
    m_preRollActive = false;
    m_audiblePositionSeconds = seconds;
    m_heldPositionSeconds = seconds;
    if (!m_slipEnabled)
        m_backgroundPositionSeconds = seconds;
    setSnapAnchor(seconds, false);
    publishSnapshot();
}

void DeckTransport::ensureAudioRunning(bool blockedByScratch) noexcept
{
    if (!m_playRequested || m_preRollActive || blockedByScratch || !m_hasTrack)
        return;
    const auto graph = m_audioPipeline.transportSnapshot();
    if (graph.running || graph.lengthSeconds <= 0.0)
        return;
    if (graph.positionSeconds >= graph.lengthSeconds - kEndEpsilonSeconds) {
        if (!m_reverse)
            return;
        m_audioPipeline.seekToSeconds(std::max(0.0, graph.lengthSeconds - kEndEpsilonSeconds));
    }
    setSnapAnchor(graph.positionSeconds, true);
    m_audioPipeline.setTransportRunning(true);
    m_atTrackEnd = false;
    publishSnapshot();
}

void DeckTransport::setPreRollPosition(double seconds) noexcept
{
    if (!std::isfinite(seconds))
        return;
    m_heldPositionSeconds = std::clamp(seconds, -TransportLimits::kPreRollSeconds, 0.0);
    m_audiblePositionSeconds = m_heldPositionSeconds;
    m_audioPlayhead.store(m_heldPositionSeconds, std::memory_order_release);
    if (m_playRequested)
        startPreRoll(m_heldPositionSeconds);
    publishSnapshot();
}

void DeckTransport::beginPreRoll(double seconds) noexcept
{
    if (!m_hasTrack || !std::isfinite(seconds))
        return;
    startPreRoll(seconds);
    publishSnapshot();
}

void DeckTransport::cancelPreRoll() noexcept
{
    if (!m_preRollActive)
        return;
    m_preRollActive = false;
    setSnapAnchor(m_heldPositionSeconds, false);
    publishSnapshot();
}

DeckTransport::ControlUpdate DeckTransport::updateControlState(
    const LoopRegion& loop, bool scratchActive, bool cuePreviewActive) noexcept
{
    ControlUpdate result;
    auto graph = m_audioPipeline.transportSnapshot();
    if (scratchActive) {
        m_audiblePositionSeconds = m_audioPlayhead.load(std::memory_order_acquire);
        m_heldPositionSeconds = m_audiblePositionSeconds;
        result.positionChanged = true;
        publishSnapshot();
        return result;
    }

    if (graph.running && !m_playRequested && !cuePreviewActive) {
        freezeAt(graph.positionSeconds);
        graph = m_audioPipeline.transportSnapshot();
    }
    if (graph.running && m_preRollActive) {
        m_audioPipeline.setTransportRunning(false);
        graph.running = false;
    }

    if (graph.running) {
        const double elapsed = m_snapValid
            ? std::clamp(static_cast<double>(m_snapClock.nsecsElapsed()) * 1.0e-9, 0.001, 0.050)
            : 0.004;
        m_audiblePositionSeconds = graph.positionSeconds;
        m_heldPositionSeconds = graph.positionSeconds;
        m_audioPlayhead.store(m_audiblePositionSeconds, std::memory_order_release);
        if (slipDiverted(loop.active))
            m_backgroundPositionSeconds = std::min(
                m_backgroundPositionSeconds + elapsed * m_playbackRate, m_trackLengthSeconds);
        else
            m_backgroundPositionSeconds = graph.positionSeconds;

        if (loop.active && loop.endSeconds > loop.startSeconds) {
            if (m_reverse && m_audiblePositionSeconds <= loop.startSeconds) {
                m_audioPipeline.seekToSeconds(loop.endSeconds);
                m_audiblePositionSeconds = loop.endSeconds;
            } else if (!m_reverse && loop.startSeconds < 0.0
                       && m_audiblePositionSeconds >= loop.endSeconds) {
                m_audioPipeline.setTransportRunning(false);
                startPreRoll(loop.startSeconds);
                result.enteredPreRoll = true;
            }
        }
        if (!m_reverse && !loop.active && m_trackLengthSeconds > 0.0
            && m_audiblePositionSeconds >= m_trackLengthSeconds - kEndEpsilonSeconds) {
            m_audiblePositionSeconds = m_trackLengthSeconds;
            m_heldPositionSeconds = m_trackLengthSeconds;
            setSnapAnchor(m_trackLengthSeconds, false);
        } else {
            m_snapClock.restart();
        }
        result.positionChanged = true;
    } else if (m_preRollActive) {
        const double elapsed = static_cast<double>(m_preRollClock.nsecsElapsed()) * 1.0e-9;
        double position = m_preRollStartSeconds + elapsed * m_playbackRate;
        if (loop.active && loop.endSeconds <= 0.0 && loop.endSeconds > loop.startSeconds
            && position >= loop.endSeconds) {
            startPreRoll(loop.startSeconds);
            position = loop.startSeconds;
        }
        m_audiblePositionSeconds = std::min(position, 0.0);
        m_heldPositionSeconds = m_audiblePositionSeconds;
        m_audioPlayhead.store(m_audiblePositionSeconds, std::memory_order_release);
        result.positionChanged = true;
        if (position >= 0.0) {
            m_preRollActive = false;
            m_audiblePositionSeconds = 0.0;
            m_heldPositionSeconds = 0.0;
            m_audioPipeline.seekToSeconds(0.0);
            setSnapAnchor(0.0, true);
            m_audioPipeline.setTransportRunning(true);
            result.leftPreRoll = true;
        }
    } else {
        ensureAudioRunning();
        graph = m_audioPipeline.transportSnapshot();
        m_audiblePositionSeconds = m_heldPositionSeconds < 0.0
            ? m_heldPositionSeconds : graph.positionSeconds;
    }

    const bool wasAtTrackEnd = m_atTrackEnd;
    m_atTrackEnd = m_hasTrack && !m_reverse && !loop.active
        && m_trackLengthSeconds > 0.0
        && m_audiblePositionSeconds >= m_trackLengthSeconds - kEndEpsilonSeconds;
    result.reachedTrackEnd = m_atTrackEnd && !wasAtTrackEnd;
    result.audioRunning = m_audioPipeline.transportSnapshot().running;
    publishSnapshot();
    return result;
}

void DeckTransport::setLoopRegion(const LoopRegion& loop) noexcept
{
    const bool audioLoop = loop.active && loop.startSeconds >= 0.0 && loop.endSeconds > 0.0;
    m_audioPipeline.setLoopRangeSeconds(loop.startSeconds, loop.endSeconds, audioLoop,
                                     m_sourceSampleRate);
}

void DeckTransport::setPlaybackReadPositionSamples(std::int64_t samplePosition) noexcept
{
    m_audioPipeline.setPlaybackReadPositionSamples(samplePosition);
}

void DeckTransport::setAudioReverseOverride(bool enabled) noexcept
{
    m_audioPipeline.setReverse(enabled);
}

void DeckTransport::setKeylockEnabled(bool enabled) noexcept
{
    m_audioPipeline.setKeylockEnabled(enabled);
}

void DeckTransport::setKeySemitoneOffset(double semitones) noexcept
{
    m_audioPipeline.setKeySemitoneOffset(semitones);
}

void DeckTransport::startAudio() noexcept
{
    m_audioPipeline.setTransportRunning(true);
    const auto graph = m_audioPipeline.transportSnapshot();
    m_audiblePositionSeconds = graph.positionSeconds;
    setSnapAnchor(graph.positionSeconds, true);
    publishSnapshot();
}

void DeckTransport::startAudioPreservingScratchPosition() noexcept
{
    // The normal reader is repositioned by RenderModeRouter at the next audio
    // block boundary. Its current transport position is still the grab cursor.
    m_audioPipeline.setTransportRunning(true);
    setSnapAnchor(m_heldPositionSeconds, true);
    publishSnapshot();
}

void DeckTransport::stopAudio() noexcept
{
    m_audioPipeline.setTransportRunning(false);
    m_snapValid = false;
    publishSnapshot();
}

void DeckTransport::seekAudioToSeconds(double seconds) noexcept
{
    if (!std::isfinite(seconds))
        return;
    const double clamped = std::clamp(seconds, 0.0, m_trackLengthSeconds);
    m_audioPipeline.seekToSeconds(clamped);
    m_audiblePositionSeconds = clamped;
    m_heldPositionSeconds = clamped;
    if (!m_slipEnabled)
        m_backgroundPositionSeconds = clamped;
    setSnapAnchor(clamped, true);
    publishSnapshot();
}

void DeckTransport::adoptScratchHandoffPosition(double seconds) noexcept
{
    if (!std::isfinite(seconds))
        return;
    const double clamped = std::clamp(seconds, 0.0, m_trackLengthSeconds);
    m_audiblePositionSeconds = clamped;
    m_heldPositionSeconds = clamped;
    if (!m_slipEnabled)
        m_backgroundPositionSeconds = clamped;
    setSnapAnchor(clamped, true);
    armVisualSeekSettle();
    publishSnapshot();
}

DeckTransportSnapshot DeckTransport::snapshot() const noexcept
{
    DeckTransportSnapshot result;
    for (;;) {
        const auto before = m_publishedSequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        const auto flags = m_publishedFlags.load(std::memory_order_relaxed);
        result.hasTrack = (flags & HasTrack) != 0;
        result.playing = (flags & Playing) != 0;
        result.audioRunning = (flags & AudioRunning) != 0;
        result.reverse = (flags & Reverse) != 0;
        result.slipEnabled = (flags & Slip) != 0;
        result.preRollActive = (flags & PreRoll) != 0;
        result.atTrackEnd = (flags & AtTrackEnd) != 0;
        result.audiblePositionSeconds = m_publishedAudible.load(std::memory_order_relaxed);
        result.backgroundPositionSeconds = m_publishedBackground.load(std::memory_order_relaxed);
        result.trackLengthSeconds = m_publishedLength.load(std::memory_order_relaxed);
        result.playbackRate = m_publishedRate.load(std::memory_order_relaxed);
        result.preRollPositionSeconds = m_publishedPreRoll.load(std::memory_order_relaxed);
        result.sourceSampleRate = m_publishedSourceRate.load(std::memory_order_relaxed);
        result.trackGeneration = m_publishedTrackGeneration.load(std::memory_order_relaxed);
        result.stateGeneration = m_publishedStateGeneration.load(std::memory_order_relaxed);
        const auto after = m_publishedSequence.load(std::memory_order_acquire);
        if (before == after)
            return result;
    }
}

double DeckTransport::positionSeconds(bool scratchActive) const noexcept
{
    if (scratchActive || m_preRollActive)
        return m_audioPlayhead.load(std::memory_order_acquire);
    if (m_heldPositionSeconds < 0.0)
        return m_heldPositionSeconds;
    return m_audioPipeline.transportSnapshot().positionSeconds;
}

double DeckTransport::visualPositionSeconds(double latencyCompensationSeconds) const noexcept
{
    if (m_preRollActive) {
        const double elapsed = static_cast<double>(m_preRollClock.nsecsElapsed()) * 1.0e-9;
        return std::min(m_preRollStartSeconds + elapsed * m_playbackRate, 0.0);
    }
    const auto graph = m_audioPipeline.transportSnapshot();
    if (!m_reverse && m_trackLengthSeconds > 0.0
        && graph.positionSeconds >= m_trackLengthSeconds - kEndEpsilonSeconds)
        return m_trackLengthSeconds;
    if (!m_snapValid || !graph.running)
        return positionSeconds();

    const auto& presentation = graph.presentation;
    if (!graph.presentationCurrent)
        return graph.positionSeconds;
    double latencyBlend = 1.0;
    if (m_visualSeekSettleClock.isValid()) {
        constexpr double settleSeconds = 0.090;
        const double settle = static_cast<double>(m_visualSeekSettleClock.nsecsElapsed()) * 1.0e-9;
        if (settle < settleSeconds) {
            const double t = std::clamp(settle / settleSeconds, 0.0, 1.0);
            latencyBlend = t * t * (3.0 - 2.0 * t);
        }
    }
    const double position = presentation.positionAt(
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        latencyCompensationSeconds * latencyBlend);
    return std::clamp(position, -TransportLimits::kPreRollSeconds,
                      m_trackLengthSeconds > 0.0 ? m_trackLengthSeconds : position);
}

double DeckTransport::playheadPositionAtomic() const noexcept
{
    return m_audioPlayhead.load(std::memory_order_acquire);
}

bool DeckTransport::audioRunning() const noexcept
{
    return m_audioPipeline.transportSnapshot().running;
}

double DeckTransport::audioPositionSeconds() const noexcept
{
    return m_audioPipeline.transportSnapshot().positionSeconds;
}

int DeckTransport::keylockLatencySamples() const noexcept
{
    return m_audioPipeline.keylockLatencySamples();
}

bool DeckTransport::slipDiverted(bool loopActive, bool scratchActive) const noexcept
{
    return m_slipEnabled && (loopActive || m_reverse || scratchActive);
}

void DeckTransport::publishScratchPosition(double seconds) noexcept
{
    if (!std::isfinite(seconds))
        return;
    m_audioPlayhead.store(seconds, std::memory_order_release);
    m_audiblePositionSeconds = seconds;
    m_heldPositionSeconds = seconds;
    publishSnapshot();
}

void DeckTransport::adoptScratchRenderedPosition(double seconds, double elapsedSeconds) noexcept
{
    if (!std::isfinite(seconds))
        return;
    m_audiblePositionSeconds = seconds;
    m_heldPositionSeconds = seconds;
    if (m_slipEnabled && m_playRequested && std::isfinite(elapsedSeconds)
        && elapsedSeconds > 0.0) {
        m_backgroundPositionSeconds = std::min(
            m_backgroundPositionSeconds + elapsedSeconds * m_playbackRate,
            m_trackLengthSeconds);
    }
    publishSnapshot();
}

void DeckTransport::setHeldPosition(double seconds) noexcept
{
    if (!std::isfinite(seconds))
        return;
    m_heldPositionSeconds = seconds;
    m_audiblePositionSeconds = seconds;
    m_audioPlayhead.store(seconds, std::memory_order_release);
    publishSnapshot();
}

void DeckTransport::armVisualSeekSettle() noexcept
{
    m_visualSeekSettleClock.restart();
}

void DeckTransport::setVisualAnchor(double seconds, bool valid) noexcept
{
    setSnapAnchor(seconds, valid);
    m_audiblePositionSeconds = seconds;
    m_heldPositionSeconds = seconds;
    publishSnapshot();
}

void DeckTransport::publishSnapshot() noexcept
{
    ++m_stateGeneration;
    m_publishedSequence.fetch_add(1, std::memory_order_acq_rel);
    std::uint32_t flags = 0;
    if (m_hasTrack) flags |= HasTrack;
    if (m_playRequested) flags |= Playing;
    if (m_audioPipeline.transportSnapshot().running) flags |= AudioRunning;
    if (m_reverse) flags |= Reverse;
    if (m_slipEnabled) flags |= Slip;
    if (m_preRollActive) flags |= PreRoll;
    if (m_atTrackEnd) flags |= AtTrackEnd;
    m_publishedFlags.store(flags, std::memory_order_relaxed);
    m_publishedAudible.store(m_audiblePositionSeconds, std::memory_order_relaxed);
    m_publishedBackground.store(m_backgroundPositionSeconds, std::memory_order_relaxed);
    m_publishedLength.store(m_trackLengthSeconds, std::memory_order_relaxed);
    m_publishedRate.store(m_playbackRate * m_jogNudgeRatio, std::memory_order_relaxed);
    m_publishedPreRoll.store(m_heldPositionSeconds < 0.0 ? m_heldPositionSeconds : 0.0,
                             std::memory_order_relaxed);
    m_publishedSourceRate.store(m_sourceSampleRate, std::memory_order_relaxed);
    m_publishedTrackGeneration.store(m_trackGeneration, std::memory_order_relaxed);
    m_publishedStateGeneration.store(m_stateGeneration, std::memory_order_relaxed);
    m_publishedSequence.fetch_add(1, std::memory_order_release);
}

void DeckTransport::setSnapAnchor(double seconds, bool valid) noexcept
{
    m_snapClock.restart();
    m_snapValid = valid;
    m_audioPlayhead.store(seconds, std::memory_order_release);
}

void DeckTransport::startPreRoll(double seconds) noexcept
{
    m_audioPipeline.setTransportRunning(false);
    m_preRollActive = true;
    m_preRollStartSeconds = std::clamp(seconds, -TransportLimits::kPreRollSeconds, 0.0);
    m_heldPositionSeconds = m_preRollStartSeconds;
    m_audiblePositionSeconds = m_preRollStartSeconds;
    m_preRollClock.restart();
    setSnapAnchor(m_preRollStartSeconds, false);
}
