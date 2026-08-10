#include "../DjEngine.h"

#include "audio/DeckAudioPipeline.h"
#include "audio/DeckChannelProcessor.h"
#include "audio/RenderModeRouter.h"
#include "audio/TimeStretchProcessor.h"
#include "deck/DeckTransport.h"
#include "engine/deck/JogNudgePolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <juce_core/juce_core.h>


engine::scratch::ScratchLoopCtx DjEngine::scratchLoopCtx() const noexcept
{
    engine::scratch::ScratchLoopCtx ctx;
    ctx.active = m_cueLoopController.activeLoop().active && (m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec);
    ctx.inSec = m_cueLoopController.activeLoop().inSec;
    ctx.outSec = m_cueLoopController.activeLoop().outSec;
    return ctx;
}


void DjEngine::terminateScratchSession(double positionSec)
{
    m_pendingScratchReleaseGeneration = 0;
    m_scratch.clear();
    m_scratchSnapReadPending = false;

    if (m_audioPipeline->renderModeRouterPtr())
        m_audioPipeline->renderModeRouter().exitScratchMode(
            std::max(0.0, positionSec), m_transport->sourceSampleRate());
}


void DjEngine::updateScrubPlayheadAnchor()
{
    // Pre-roll: no audio data exists before t=0.  Stop the transport to prevent
    // frame-0 audio leaking through the resampler while the platter is in silence.
    if (m_transport->heldPosition() < 0.0 && m_transport->audioRunning())
        m_transport->stopAudio();

    if ((m_scratch.scrubbing() || m_scratch.releaseGlide()) && m_transport->heldPosition() >= 0.0) {
        // During scratch the audio callback owns m_transport->audioPlayheadSink() — do not overwrite here.
        return;
    }

    // Only sync from transport when not in pre-roll: transport is clamped at 0
    // while visual position is negative, so syncing would erase the pre-roll offset.
    if (m_transport->heldPosition() >= 0.0)
        m_transport->setHeldPosition(m_transport->audioPositionSeconds());
}


void DjEngine::tickScratchPhysics()
{
    if (!m_audioPipeline->renderModeRouterPtr())
        return;

    auto& physicsClock = m_scratch.physicsClock();
    const double dtSec = physicsClock.isValid()
        ? std::clamp(static_cast<double>(physicsClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
        : 0.016;
    physicsClock.restart();

    const double scratchRate = m_scratch.tick(m_audioPipeline->renderModeRouterPtr(), dtSec);
    const double absRate = std::abs(scratchRate);

    if (m_audioPipeline->mixerPtr()) {
        const double timbreSignal = std::clamp(std::sqrt(std::max(absRate, 0.08)), 0.18, 1.0);
        m_audioPipeline->mixer().setScratchTimbre(static_cast<float>(timbreSignal));
    }

    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        m_transport->adoptScratchRenderedPosition(
            m_transport->playheadPositionAtomic());
    }

    if (m_scratch.releaseGlide() && m_pendingScratchReleaseGeneration != 0) {
        const auto release = m_audioPipeline->renderModeRouter().scratchReleaseSnapshot();
        if (release.generation == m_pendingScratchReleaseGeneration) {
            using AudioPhase = engine::audio::ScratchReleasePhase;
            using SessionPhase = engine::scratch::ScratchPhase;
            switch (release.phase) {
            case AudioPhase::ReleasePending:
                m_scratch.setPhase(SessionPhase::ReleasePending);
                break;
            case AudioPhase::CoastToDeck:
                m_scratch.setPhase(SessionPhase::CoastToDeckRate);
                break;
            case AudioPhase::CoastToStop:
                m_scratch.setPhase(SessionPhase::CoastToStop);
                break;
            case AudioPhase::HandoffPending:
                m_scratch.setPhase(SessionPhase::HandoffPending);
                break;
            case AudioPhase::Idle:
            case AudioPhase::TailSuppression:
                break;
            }

            const bool finalSnapshot = release.phase == AudioPhase::TailSuppression
                || release.phase == AudioPhase::Idle;
            if (finalSnapshot
                && m_audioPipeline->renderModeRouter().scratchReleaseComplete(
                    m_pendingScratchReleaseGeneration)) {
                const double finalCursor = release.finalCursorSeconds;
                m_pendingScratchReleaseGeneration = 0;
                m_scratch.setReleaseGlide(false);
                restorePostScrubPlaybackState(finalCursor);
                if (m_audioPipeline->mixerPtr())
                    m_audioPipeline->mixer().setScratchTimbre(0.0f);
                emit scrubbingChanged();
                emit playingChanged();
                emit progressChanged();
            }
        }
    }

    // Atomic playhead + QML FrameAnimation drive the waveform during scratch.
    // Avoid emitPlaybackStateChanged() here — it fires progress/VU/gr NOTIFYs
    // at 250 Hz and stalls the UI thread at scratch start.
    notifyProgressIfNeeded();
}


void DjEngine::decayJogNudge()
{
    if (m_jogNudgeCommandPercent == 0.0 && m_jogNudgePercent == 0.0)
        return;

    const double idleSec = m_lastJogNudgeClock.isValid()
        ? static_cast<double>(m_lastJogNudgeClock.nsecsElapsed()) * 1e-9
        : 1.0;
    const double nextPercent = engine::deck::decayedJogNudgePercent(
        m_jogNudgeCommandPercent, idleSec);
    if (nextPercent == 0.0)
        m_jogNudgeCommandPercent = 0.0;

    if (nextPercent != m_jogNudgePercent) {
        m_jogNudgePercent = nextPercent;
        updateSpeedAndPitch();
    }
}


void DjEngine::applyScratchNeutralRouting()
{
    if (m_audioPipeline->timeStretchPtr())
        m_audioPipeline->timeStretch().enterScratchBypass();
    if (m_audioPipeline->renderModeRouterPtr())
        m_audioPipeline->renderModeRouter().setKeylockEnabled(false);
}


void DjEngine::restorePostScrubPlaybackState(double finalCursorSeconds)
{
    const double audioSec = std::clamp(
        std::isfinite(finalCursorSeconds) ? finalCursorSeconds : 0.0,
        0.0,
        m_transport->trackLengthSeconds());

    // Reader seek, Hermite reset and scratch exit were already committed in one
    // callback. This only adopts the callback's acknowledged cursor.
    m_transport->adoptScratchHandoffPosition(audioSec);

    m_transport->setAudioReverseOverride(m_transport->reverse());

    if (m_scratch.wasPlaying() && !m_transport->playRequested())
        m_transport->setPlaying(true);

    if (!m_transport->playRequested())
        m_transport->stopAudio();

    // Resume at the live deck tempo — never hard-reset to 1.0× first, which
    // causes a brief slow-down when the tempo fader is above/below center.
    updateSpeedAndPitch();
    if (m_audioPipeline->timeStretchPtr())
        m_audioPipeline->timeStretch().endScratchBypass();

    // Re-apply loop range to the audio source — scratch neutral routing may have
    // changed the reverse state, which gates loop enforcement in applyLoopRangeToAudioSource.
    if (m_cueLoopController.activeLoop().active)
        applyLoopRangeToAudioSource();

    if (m_transport->playRequested()) {
        if (m_transport->heldPosition() < 0.0) {
            // Scratch glide may have started the transport from position 0; stop it
            // before the pre-roll countdown takes over so it does not race with the
            // countdown logic and cause a snap to 0 via getVisualPosition().
            m_transport->stopAudio();
            m_transport->beginPreRoll(m_transport->heldPosition());
        } else if (!m_transport->audioRunning()) {
            m_transport->startAudioPreservingScratchPosition();
        }
    }

    m_scratch.setWasPlaying(false);
    m_transport->setVisualAnchor(m_transport->heldPosition(), !m_transport->preRollActive());
}


void DjEngine::pauseForScrub(double anchorPositionSec)
{
    // Capture before scratch / pre-roll flags change (negative pre-roll is valid).
    const double visualAtGrab = getVisualPosition();
    const double len          = m_transport->trackLengthSeconds();

    m_jogNudgePercent = 0.0;
    m_jogNudgeCommandPercent = 0.0;
    m_syncController->resetPhaseCorrection();

    const bool regrabActiveScratch = m_scratch.scrubbing() || m_scratch.releaseGlide();
    const bool wasPlayingBeforeGrab = regrabActiveScratch
        ? m_scratch.wasPlaying()
        : (m_transport->playRequested() || m_transport->audioRunning());
    m_scratch.setReleaseGlide(false);
    m_scratch.setWasPlaying(wasPlayingBeforeGrab);
    m_scratch.setScrubbing(true);
    m_pendingScratchReleaseGeneration = 0;
    m_transport->setVisualAnchor(visualAtGrab, false);
    m_scratchSnapReadPending = false;

    m_transport->cancelPreRoll();

    if (m_transport->audioRunning())
        m_transport->stopAudio();

    const auto clampVirtual = [len](double sec) {
        return std::clamp(sec, -SCRATCH_PRE_ROLL_SECONDS, len > 0.0 ? len : sec);
    };

    double grabSec = m_transport->heldPosition();
    if (anchorPositionSec != -1.0) {
        grabSec = clampVirtual(anchorPositionSec);
    } else if (regrabActiveScratch) {
        grabSec = m_transport->playheadPositionAtomic();
    } else if (wasPlayingBeforeGrab || visualAtGrab < 0.0 || grabSec < 0.0) {
        grabSec = visualAtGrab;
    } else if (grabSec >= 0.0) {
        // keep frozen hold (paused mid-track)
    } else {
        grabSec = std::max(0.0, m_transport->audioPositionSeconds());
    }
    grabSec = clampVirtual(grabSec);

    const auto loopCtx = scratchLoopCtx();
    m_transport->publishScratchPosition(m_scratch.armGrab(grabSec, len, loopCtx));

    if (m_audioPipeline->renderModeRouterPtr()) {
        m_audioPipeline->renderModeRouter().beginScratch(m_transport->heldPosition(),
                                    m_transport->sourceSampleRate(),
                                    std::max(0.0, len),
                                    wasPlayingBeforeGrab,
                                    getTempoRatio());
        const bool scratchLoopActive = m_cueLoopController.activeLoop().active
            && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec
            && m_cueLoopController.activeLoop().inSec >= 0.0
            && m_cueLoopController.activeLoop().outSec > 0.0;
        m_audioPipeline->renderModeRouter().setLoopRangeSeconds(m_cueLoopController.activeLoop().inSec,
                                           m_cueLoopController.activeLoop().outSec,
                                           scratchLoopActive,
                                           m_transport->sourceSampleRate());
        m_audioPipeline->renderModeRouter().setReverse(m_transport->reverse());
        m_audioPipeline->renderModeRouter().setKeylockEnabled(false);
        m_audioPipeline->renderModeRouter().syncScratchReadPosition(m_transport->heldPosition(), m_transport->sourceSampleRate());
    }

    emit scrubbingChanged();

    applyScratchNeutralRouting();
}


void DjEngine::scratchBySeconds(double deltaSeconds, bool vinylOneToOnePosition)
{
    juce::ignoreUnused(vinylOneToOnePosition);
    if (deltaSeconds == 0.0)
        return;

    if (!m_scratch.scrubbing() || !m_audioPipeline->renderModeRouterPtr())
        return;

    if (!m_scratch.submitRelative(m_audioPipeline->renderModeRouterPtr(), deltaSeconds, m_transport->sourceSampleRate()))
        return;
}


void DjEngine::scratchBySecondsTimed(double deltaSeconds, double eventIntervalSeconds)
{
    if (deltaSeconds == 0.0)
        return;

    if (!m_scratch.scrubbing() || !m_audioPipeline->renderModeRouterPtr())
        return;

    const double interval = std::isfinite(eventIntervalSeconds)
            && eventIntervalSeconds > 0.0
        ? eventIntervalSeconds
        : 0.016;
    (void) m_scratch.submitRelativeAtInterval(
        m_audioPipeline->renderModeRouterPtr(),
        deltaSeconds,
        m_transport->sourceSampleRate(),
        interval);
}


void DjEngine::setScrubPosition(double positionSeconds)
{
    if (!m_scratch.scrubbing() || !m_audioPipeline->renderModeRouterPtr())
        return;

    const double len = m_transport->trackLengthSeconds();
    if (len <= 0.0)
        return;

    if (!m_scratch.submitAbsolute(m_audioPipeline->renderModeRouterPtr(),
                                  positionSeconds,
                                  m_transport->sourceSampleRate(),
                                  len,
                                  SCRATCH_PRE_ROLL_SECONDS,
                                  scratchLoopCtx())) {
        return;
    }
}


double DjEngine::platterAngleDegrees() const
{
    if (!m_audioPipeline->renderModeRouterPtr())
        return 0.0;
    return m_audioPipeline->renderModeRouter().platter().displayAngleDegrees();
}


void DjEngine::completeScratchRelease(bool allowInertia)
{
    if (!m_audioPipeline->renderModeRouterPtr())
        return;
    // UI/mouse release has no hardware-rate snapshot. NaN asks the callback to
    // decide from the final block it actually rendered.
    requestScratchRelease(std::numeric_limits<double>::quiet_NaN(), allowInertia);
}


void DjEngine::requestScratchRelease(double normalizedReleaseSpeed, bool allowInertia)
{
    if ((!m_scratch.scrubbing() && !m_scratch.releaseGlide())
        || !m_audioPipeline->renderModeRouterPtr()) {
        return;
    }
    if (m_pendingScratchReleaseGeneration != 0)
        return;

    auto& bridge = m_audioPipeline->renderModeRouter();
    m_transport->setAudioReverseOverride(m_transport->reverse());
    bridge.setKeylockEnabled(m_keylock);
    if (m_cueLoopController.activeLoop().active)
        applyLoopRangeToAudioSource();

    m_scratch.setPhase(engine::scratch::ScratchPhase::ReleasePending);
    m_pendingScratchReleaseGeneration = bridge.requestScratchRelease(
        normalizedReleaseSpeed,
        allowInertia);

    // The normal reader runs behind the still-authoritative scratch path. It is
    // not audible until the callback seeks and commits the handoff.
    if (m_scratch.wasPlaying() && m_transport->heldPosition() >= 0.0
        && !m_transport->audioRunning()) {
        m_transport->startAudioPreservingScratchPosition();
    }

    emit scrubbingChanged();
}


void DjEngine::submitScratchReleaseSpeed(double normalizedReleaseSpeed)
{
    if (!m_scratch.releaseGlide() || m_pendingScratchReleaseGeneration == 0
        || !m_audioPipeline->renderModeRouterPtr() || !std::isfinite(normalizedReleaseSpeed)) {
        return;
    }
    m_audioPipeline->renderModeRouter().submitScratchReleaseSpeed(normalizedReleaseSpeed);
}


void DjEngine::resumeAfterScrub()
{
    if (!m_scratch.scrubbing())
        return;

    completeScratchRelease(true);
}


void DjEngine::applyScratchReleaseJog(double deltaSeconds)
{
    if (!m_scratch.releaseGlide() || deltaSeconds == 0.0 || !m_audioPipeline->renderModeRouterPtr())
        return;

    m_scratch.submitReleaseRelative(m_audioPipeline->renderModeRouterPtr(), deltaSeconds);
    m_transport->adoptScratchRenderedPosition(
        m_audioPipeline->renderModeRouter().readPositionSeconds(m_transport->sourceSampleRate()));
    notifyProgressIfNeeded();
}


void DjEngine::finishScrubWithoutInertia()
{
    if (!m_scratch.scrubbing() && !m_scratch.releaseGlide())
        return;

    completeScratchRelease(false);
    emit playingChanged();
    emit progressChanged();
}


void DjEngine::applyJogNudge(double signedTicks)
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return;

    // Mixxx's FLX10 mapping divides jog ticks by 16; its 0.1 jog sensitivity
    // corresponds to a 0.625% temporary rate change per controller tick.
    m_jogNudgeCommandPercent = engine::deck::jogNudgeCommandPercent(signedTicks);
    m_jogNudgePercent = m_jogNudgeCommandPercent;
    m_lastJogNudgeClock.restart();
    updateSpeedAndPitch();
}
