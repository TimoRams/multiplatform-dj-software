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

    if (scratchBridge)
        scratchBridge->exitScratchMode(std::max(0.0, positionSec), m_loadedTrackSampleRate);
}


void DjEngine::updateScrubPlayheadAnchor()
{
    // Pre-roll: no audio data exists before t=0.  Stop the transport to prevent
    // frame-0 audio leaking through the resampler while the platter is in silence.
    if (m_scrubHoldPosition < 0.0 && transportSource.isPlaying())
        transportSource.stop();

    if ((m_scratch.scrubbing() || m_scratch.releaseGlide()) && m_scrubHoldPosition >= 0.0) {
        // During scratch the audio callback owns m_atomicPlayheadPos — do not overwrite here.
        return;
    }

    // Only sync from transport when not in pre-roll: transport is clamped at 0
    // while visual position is negative, so syncing would erase the pre-roll offset.
    if (m_scrubHoldPosition >= 0.0)
        m_scrubHoldPosition = transportSource.getCurrentPosition();
    m_atomicPlayheadPos.store(m_scrubHoldPosition,
                              std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
}


void DjEngine::tickScratchPhysics()
{
    if (!scratchBridge)
        return;

    auto& physicsClock = m_scratch.physicsClock();
    const double dtSec = physicsClock.isValid()
        ? std::clamp(static_cast<double>(physicsClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
        : 0.016;
    physicsClock.restart();

    const double scratchRate = m_scratch.tick(scratchBridge.get(), dtSec);
    const double absRate = std::abs(scratchRate);

    if (mixerSource) {
        const double timbreSignal = std::clamp(std::sqrt(std::max(absRate, 0.08)), 0.18, 1.0);
        mixerSource->setScratchTimbre(static_cast<float>(timbreSignal));
    }

    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        // During active drag the session delta sum is authoritative for display.
        if (m_scratch.scrubbing()) {
            m_scrubHoldPosition = m_scratch.lastRawSec();
            m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
        } else {
            m_scrubHoldPosition = m_atomicPlayheadPos.load(std::memory_order_acquire);
        }
    }

    if (!m_scratch.scrubbing() && m_scratch.releaseGlide() && !scratchBridge->isInertiaActive()) {
        m_scratch.setReleaseGlide(false);
        m_scrubHoldPosition = m_atomicPlayheadPos.load(std::memory_order_acquire);
        scratchBridge->endScratch(false);
        restorePostScrubPlaybackState();
        if (mixerSource)
            mixerSource->setScratchTimbre(0.0f);
        emit scrubbingChanged();
    }

    m_snapTempoRatio = getTempoRatio();
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
    if (!reverseWrapSource || m_loadedTrackSampleRate <= 0.0)
        return;

    const juce::int64 samplePos = static_cast<juce::int64>(
        std::lround(std::max(0.0, m_scrubHoldPosition) * m_loadedTrackSampleRate));
    reverseWrapSource->setNextReadPosition(samplePos);
}


void DjEngine::applyScratchNeutralRouting()
{
    if (timeStretchSource)
        timeStretchSource->enterScratchBypass();
    if (scratchBridge)
        scratchBridge->setKeylockPassthrough(false);
    if (reverseWrapSource)
        reverseWrapSource->setReverse(false);
}


void DjEngine::restorePostScrubPlaybackState()
{
    const double resumeSec = m_scrubHoldPosition;
    const double audioSec  = std::max(0.0, resumeSec);

    if (scratchBridge)
        scratchBridge->prepareNormalPlaybackHandoff(audioSec, m_loadedTrackSampleRate);

    transportSource.setPosition(audioSec);
    m_scrubHoldPosition = resumeSec;
    m_atomicPlayheadPos.store(resumeSec, std::memory_order_release);
    m_snapPosition = audioSec;
    syncReverseReaderToHold();
    armVisualSeekSettle();

    if (mixerSource)
        mixerSource->armClickFreeTransition();

    if (reverseWrapSource)
        reverseWrapSource->setReverse(m_isReverse);

    if (m_scratch.wasPlaying() && !m_playRequested)
        m_playRequested = true;

    if (!m_playRequested)
        transportSource.stop();

    if (timeStretchSource)
        timeStretchSource->endScratchBypass();

    // Resume at the live deck tempo — never hard-reset to 1.0× first, which
    // causes a brief slow-down when the tempo fader is above/below center.
    updateSpeedAndPitch();

    // Re-apply loop range to the audio source — scratch neutral routing may have
    // changed the reverse state, which gates loop enforcement in applyLoopRangeToAudioSource.
    if (m_cueLoopController.activeLoop().active)
        applyLoopRangeToAudioSource();

    if (m_playRequested) {
        if (m_scrubHoldPosition < 0.0) {
            // Scratch glide may have started the transport from position 0; stop it
            // before the pre-roll countdown takes over so it does not race with the
            // countdown logic and cause a snap to 0 via getVisualPosition().
            transportSource.stop();
            m_preRollCountdownActive = true;
            m_preRollVisualStartPos = m_scrubHoldPosition;
            m_preRollClock.restart();
        } else {
            transportSource.start();
        }
    }

    m_scratch.setWasPlaying(false);
    m_snapTempoRatio = getTempoRatio();
    m_snapClock.restart();
    m_snapValid = !m_preRollCountdownActive;
}


void DjEngine::pauseForScrub(double anchorPositionSec)
{
    if (mixerSource)
        mixerSource->armClickFreeTransition();

    // Capture before scratch / pre-roll flags change (negative pre-roll is valid).
    const double visualAtGrab = getVisualPosition();
    const double len          = transportSource.getLengthInSeconds();

    m_phaseNudge      = 0.0;
    m_jogNudgePercent = 0.0;
    m_resyncBoost = false;

    const bool regrabActiveScratch = m_scratch.scrubbing() || m_scratch.releaseGlide();
    const bool wasPlayingBeforeGrab = regrabActiveScratch
        ? m_scratch.wasPlaying()
        : (m_playRequested || transportSource.isPlaying());
    m_scratch.setReleaseGlide(false);
    m_scratch.setWasPlaying(wasPlayingBeforeGrab);
    m_scratch.setScrubbing(true);
    m_snapValid = false;
    m_scratchSnapReadPending = false;

    m_preRollCountdownActive = false;

    if (transportSource.isPlaying())
        transportSource.stop();

    const auto clampVirtual = [len](double sec) {
        return std::clamp(sec, -SCRATCH_PRE_ROLL_SECONDS, len > 0.0 ? len : sec);
    };

    double grabSec = m_scrubHoldPosition;
    if (anchorPositionSec != -1.0) {
        grabSec = clampVirtual(anchorPositionSec);
    } else if (regrabActiveScratch) {
        grabSec = m_atomicPlayheadPos.load(std::memory_order_relaxed);
    } else if (wasPlayingBeforeGrab || visualAtGrab < 0.0 || grabSec < 0.0) {
        grabSec = visualAtGrab;
    } else if (grabSec >= 0.0) {
        // keep frozen hold (paused mid-track)
    } else {
        grabSec = std::max(0.0, transportSource.getCurrentPosition());
    }
    grabSec = clampVirtual(grabSec);

    const auto loopCtx = scratchLoopCtx();
    m_scrubHoldPosition = m_scratch.armGrab(grabSec, len, loopCtx);
    m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);

    if (scratchBridge) {
        scratchBridge->beginScratch(m_scrubHoldPosition,
                                    m_loadedTrackSampleRate,
                                    std::max(0.0, len),
                                    wasPlayingBeforeGrab,
                                    getTempoRatio());
        const bool scratchLoopActive = m_cueLoopController.activeLoop().active
            && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec
            && m_cueLoopController.activeLoop().inSec >= 0.0
            && m_cueLoopController.activeLoop().outSec > 0.0;
        scratchBridge->setLoopRangeSeconds(m_cueLoopController.activeLoop().inSec,
                                           m_cueLoopController.activeLoop().outSec,
                                           scratchLoopActive,
                                           m_loadedTrackSampleRate);
        scratchBridge->setReverse(m_isReverse);
        scratchBridge->setKeylockPassthrough(false);
        scratchBridge->syncScratchReadPosition(m_scrubHoldPosition, m_loadedTrackSampleRate);
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

    if (!m_scratch.scrubbing() || !scratchBridge)
        return;

    if (!m_scratch.submitRelative(scratchBridge.get(), deltaSeconds, m_loadedTrackSampleRate))
        return;
}


void DjEngine::setScrubPosition(double positionSeconds)
{
    if (!m_scratch.scrubbing() || !scratchBridge)
        return;

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0)
        return;

    if (!m_scratch.submitAbsolute(scratchBridge.get(),
                                  positionSeconds,
                                  m_loadedTrackSampleRate,
                                  len,
                                  SCRATCH_PRE_ROLL_SECONDS,
                                  scratchLoopCtx())) {
        return;
    }
}


double DjEngine::platterAngleDegrees() const
{
    if (!scratchBridge)
        return 0.0;
    return scratchBridge->platter().displayAngleDegrees();
}


void DjEngine::resumeAfterScrub()
{
    if (!m_scratch.scrubbing() || !scratchBridge)
        return;

    const double len = transportSource.getLengthInSeconds();
    m_scrubHoldPosition = std::clamp(m_scratch.lastRawSec(),
                                     -SCRATCH_PRE_ROLL_SECONDS,
                                     len > 0.0 ? len : m_scratch.lastRawSec());
    m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_release);
    m_scratch.setScrubbing(false);

    constexpr double kInertiaThreshold = 0.20;
    if (std::abs(scratchBridge->scratchRate()) > kInertiaThreshold) {
        scratchBridge->endScratch(true);
        m_scratch.setReleaseGlide(true);
        emit scrubbingChanged();
        return;
    }

    scratchBridge->endScratch(false);
    restorePostScrubPlaybackState();

    emit scrubbingChanged();
}


void DjEngine::applyScratchReleaseJog(double deltaSeconds)
{
    if (!m_scratch.releaseGlide() || deltaSeconds == 0.0 || !scratchBridge)
        return;

    scratchBridge->addTargetDeltaSeconds(deltaSeconds, m_loadedTrackSampleRate);
    m_scrubHoldPosition = scratchBridge->displayPositionSeconds();
    m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
    notifyProgressIfNeeded();
}


void DjEngine::finishScrubWithoutInertia()
{
    if (!m_scratch.scrubbing() && !m_scratch.releaseGlide())
        return;

    terminateScratchSession(m_scrubHoldPosition);

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
