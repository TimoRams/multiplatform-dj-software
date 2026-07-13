#include "DjEngineCommonIncludes.h"


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
    m_scratch.clear();
    m_scratchSnapReadPending = false;

    if (m_audioGraph->scratchPtr())
        m_audioGraph->scratch().exitScratchMode(std::max(0.0, positionSec), m_transport->sourceSampleRate());
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
    if (!m_audioGraph->scratchPtr())
        return;

    auto& physicsClock = m_scratch.physicsClock();
    const double dtSec = physicsClock.isValid()
        ? std::clamp(static_cast<double>(physicsClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
        : 0.016;
    physicsClock.restart();

    const double scratchRate = m_scratch.tick(m_audioGraph->scratchPtr(), dtSec);
    const double absRate = std::abs(scratchRate);

    if (m_audioGraph->mixerPtr()) {
        const double timbreSignal = std::clamp(std::sqrt(std::max(absRate, 0.08)), 0.18, 1.0);
        m_audioGraph->mixer().setScratchTimbre(static_cast<float>(timbreSignal));
    }

    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        // During active drag the session delta sum is authoritative for display.
        if (m_scratch.scrubbing()) {
            m_transport->publishScratchPosition(m_scratch.lastRawSec());
        } else {
            m_transport->publishScratchPosition(m_transport->playheadPositionAtomic());
        }
    }

    if (!m_scratch.scrubbing() && m_scratch.releaseGlide() && !m_audioGraph->scratch().isInertiaActive()) {
        m_scratch.setReleaseGlide(false);
        m_transport->publishScratchPosition(m_transport->playheadPositionAtomic());
        m_audioGraph->scratch().endScratch(false);
        restorePostScrubPlaybackState();
        if (m_audioGraph->mixerPtr())
            m_audioGraph->mixer().setScratchTimbre(0.0f);
        emit scrubbingChanged();
    }

    // Atomic playhead + QML FrameAnimation drive the waveform during scratch.
    // Avoid emitPlaybackStateChanged() here — it fires progress/VU/gr NOTIFYs
    // at 250 Hz and stalls the UI thread at scratch start.
    notifyProgressIfNeeded();
}


void DjEngine::decayJogNudge()
{
    if (m_jogNudgePercent == 0.0)
        return;

    // Fade jog outer-rim nudge back to 0% after ~150ms of no new jog events.
    const double idleSec = m_lastJogNudgeClock.isValid()
        ? static_cast<double>(m_lastJogNudgeClock.nsecsElapsed()) * 1e-9
        : 1.0;
    if (idleSec > 0.080) {
        constexpr double kNudgeDecayTau = 0.080;
        const double alpha = 1.0 - std::exp(-idleSec / kNudgeDecayTau);
        m_jogNudgePercent -= m_jogNudgePercent * alpha;
        if (std::abs(m_jogNudgePercent) < 0.05)
            m_jogNudgePercent = 0.0;
        updateSpeedAndPitch();
    }
}

// Returns true if onTimer should continue to the phase-correction/VU path,
// false if it should return immediately (pre-roll loop wrap triggered).

void DjEngine::syncReverseReaderToHold() noexcept
{
    if (!m_transport->hasTrack() || m_transport->sourceSampleRate() <= 0.0)
        return;

    const juce::int64 samplePos = static_cast<juce::int64>(
        std::lround(std::max(0.0, m_transport->heldPosition()) * m_transport->sourceSampleRate()));
    m_transport->setPlaybackReadPositionSamples(samplePos);
}


void DjEngine::applyScratchNeutralRouting()
{
    if (m_audioGraph->timeStretchPtr())
        m_audioGraph->timeStretch().enterScratchBypass();
    if (m_audioGraph->scratchPtr())
        m_audioGraph->scratch().setKeylockPassthrough(false);
    m_transport->setAudioReverseOverride(false);
}


void DjEngine::restorePostScrubPlaybackState()
{
    const double resumeSec = m_transport->heldPosition();
    const double audioSec  = std::max(0.0, resumeSec);

    if (m_audioGraph->scratchPtr())
        m_audioGraph->scratch().prepareNormalPlaybackHandoff(audioSec, m_transport->sourceSampleRate());

    m_transport->seekAudioToSeconds(audioSec);
    m_transport->setHeldPosition(resumeSec);
    m_transport->setVisualAnchor(audioSec, true);
    syncReverseReaderToHold();
    armVisualSeekSettle();

    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().armClickFreeTransition();

    m_transport->setAudioReverseOverride(m_transport->reverse());

    if (m_scratch.wasPlaying() && !m_transport->playRequested())
        m_transport->setPlaying(true);

    if (!m_transport->playRequested())
        m_transport->stopAudio();

    if (m_audioGraph->timeStretchPtr())
        m_audioGraph->timeStretch().endScratchBypass();

    // Resume at the live deck tempo — never hard-reset to 1.0× first, which
    // causes a brief slow-down when the tempo fader is above/below center.
    updateSpeedAndPitch();

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
        } else {
            m_transport->startAudio();
        }
    }

    m_scratch.setWasPlaying(false);
    m_transport->setVisualAnchor(m_transport->heldPosition(), !m_transport->preRollActive());
}


void DjEngine::pauseForScrub(double anchorPositionSec)
{
    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().armClickFreeTransition();

    // Capture before scratch / pre-roll flags change (negative pre-roll is valid).
    const double visualAtGrab = getVisualPosition();
    const double len          = m_transport->trackLengthSeconds();

    m_phaseNudge      = 0.0;
    m_jogNudgePercent = 0.0;
    m_resyncBoost = false;

    const bool regrabActiveScratch = m_scratch.scrubbing() || m_scratch.releaseGlide();
    const bool wasPlayingBeforeGrab = regrabActiveScratch
        ? m_scratch.wasPlaying()
        : (m_transport->playRequested() || m_transport->audioRunning());
    m_scratch.setReleaseGlide(false);
    m_scratch.setWasPlaying(wasPlayingBeforeGrab);
    m_scratch.setScrubbing(true);
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

    if (m_audioGraph->scratchPtr()) {
        m_audioGraph->scratch().beginScratch(m_transport->heldPosition(),
                                    m_transport->sourceSampleRate(),
                                    std::max(0.0, len),
                                    wasPlayingBeforeGrab,
                                    getTempoRatio());
        const bool scratchLoopActive = m_cueLoopController.activeLoop().active
            && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec
            && m_cueLoopController.activeLoop().inSec >= 0.0
            && m_cueLoopController.activeLoop().outSec > 0.0;
        m_audioGraph->scratch().setLoopRangeSeconds(m_cueLoopController.activeLoop().inSec,
                                           m_cueLoopController.activeLoop().outSec,
                                           scratchLoopActive,
                                           m_transport->sourceSampleRate());
        m_audioGraph->scratch().setReverse(m_transport->reverse());
        m_audioGraph->scratch().setKeylockPassthrough(false);
        m_audioGraph->scratch().syncScratchReadPosition(m_transport->heldPosition(), m_transport->sourceSampleRate());
    }

    emit scrubbingChanged();

    applyScratchNeutralRouting();
    clearLoopRangeOnAudioSource();
}


void DjEngine::scratchBySeconds(double deltaSeconds, bool vinylOneToOnePosition)
{
    juce::ignoreUnused(vinylOneToOnePosition);
    if (deltaSeconds == 0.0)
        return;

    if (!m_scratch.scrubbing() || !m_audioGraph->scratchPtr())
        return;

    if (!m_scratch.submitRelative(m_audioGraph->scratchPtr(), deltaSeconds, m_transport->sourceSampleRate()))
        return;
}


void DjEngine::setScrubPosition(double positionSeconds)
{
    if (!m_scratch.scrubbing() || !m_audioGraph->scratchPtr())
        return;

    const double len = m_transport->trackLengthSeconds();
    if (len <= 0.0)
        return;

    if (!m_scratch.submitAbsolute(m_audioGraph->scratchPtr(),
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
    if (!m_audioGraph->scratchPtr())
        return 0.0;
    return m_audioGraph->scratch().platter().displayAngleDegrees();
}


void DjEngine::resumeAfterScrub()
{
    if (!m_scratch.scrubbing() || !m_audioGraph->scratchPtr())
        return;

    const double len = m_transport->trackLengthSeconds();
    m_transport->publishScratchPosition(std::clamp(m_scratch.lastRawSec(),
                                     -SCRATCH_PRE_ROLL_SECONDS,
                                     len > 0.0 ? len : m_scratch.lastRawSec()));
    m_scratch.setScrubbing(false);

    constexpr double kInertiaThreshold = 0.20;
    if (std::abs(m_audioGraph->scratch().scratchRate()) > kInertiaThreshold) {
        m_audioGraph->scratch().endScratch(true);
        m_scratch.setReleaseGlide(true);
        emit scrubbingChanged();
        return;
    }

    m_audioGraph->scratch().endScratch(false);
    restorePostScrubPlaybackState();

    emit scrubbingChanged();
}


void DjEngine::applyScratchReleaseJog(double deltaSeconds)
{
    if (!m_scratch.releaseGlide() || deltaSeconds == 0.0 || !m_audioGraph->scratchPtr())
        return;

    m_audioGraph->scratch().addTargetDeltaSeconds(deltaSeconds, m_transport->sourceSampleRate());
    m_transport->publishScratchPosition(m_audioGraph->scratch().displayPositionSeconds());
    notifyProgressIfNeeded();
}


void DjEngine::finishScrubWithoutInertia()
{
    if (!m_scratch.scrubbing() && !m_scratch.releaseGlide())
        return;

    terminateScratchSession(m_transport->heldPosition());

    restorePostScrubPlaybackState();
    emit scrubbingChanged();
    emit playingChanged();
    emit progressChanged();
}


void DjEngine::applyJogNudge(double signedTicks)
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return;

    // FLX10 rim ticks are relative jog deltas, not coarse tempo-percent steps.
    // Keep pitch bend gentle; fast rim turns still reach the clamp naturally.
    constexpr double kPercentPerTick = 0.75;
    constexpr double kMaxNudgePercent = 6.0;
    m_jogNudgePercent = std::clamp(signedTicks * kPercentPerTick, -kMaxNudgePercent, kMaxNudgePercent);
    m_lastJogNudgeClock.restart();
    updateSpeedAndPitch();
}
