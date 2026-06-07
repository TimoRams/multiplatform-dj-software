#include "DjEngine.h"
#include "audio/TimeStretchAudioSource.h"
#include "audio/MixerDspSource.h"
#include "DjMasterBus.h"
#include "audio/ReverseStreamAudioSource.h"
#include "audio/AudioDeviceUtils.h"
#include "audio/MetadataUtils.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "fx/FxProcessor.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "WaveformCache.h"
#include "WaveformAnalyzer.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QVariantMap>
#include <QImage>
#include <QBuffer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <ranges>
#include <vector>
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

namespace {

constexpr double kVolumeMin = 0.0;
constexpr double kVolumeMax = 1.0;
constexpr double kTrimMin = 0.0;
constexpr double kTrimMax = 2.0;
constexpr double kEqMin = -1.0;
constexpr double kEqMax = 1.0;
constexpr double kFilterMin = -1.0;
constexpr double kFilterMax = 1.0;

double playHistoryThresholdSeconds(double durationSec)
{
    if (durationSec <= 0.0)
        return 12.0;

    if (durationSec <= 45.0)
        return std::clamp(durationSec * 0.35, 5.0, 12.0);

    return std::clamp(durationSec * 0.12, 10.0, 20.0);
}

QString defaultHotCueColor(int index)
{
    static const char* kColors[] = {
        "#e04040", "#e08030", "#e0c030", "#40c040",
        "#3080e0", "#8040e0", "#e040a0", "#40c0c0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

QString defaultSavedLoopColor(int index)
{
    static const char* kColors[] = {
        "#30b050", "#3080e0", "#e08030", "#8040e0",
        "#e04040", "#40c0c0", "#e0c030", "#e040a0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

} // namespace

engine::scratch::ScratchLoopCtx DjEngine::scratchLoopCtx() const noexcept
{
    engine::scratch::ScratchLoopCtx ctx;
    ctx.active = m_loopActive && (m_loopOutSec > m_loopInSec);
    ctx.inSec = m_loopInSec;
    ctx.outSec = m_loopOutSec;
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
        // Scratch audio is driven by the scratch resampler — do not hard-seek transport
        // every UI tick; that desyncs the reader and causes zipper/beep artifacts.
        m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
        m_snapPosition = m_scrubHoldPosition;
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
        // Waveform follows actual audio read head, not the UI target platter position.
        m_scrubHoldPosition = scratchBridge->readPositionSeconds(m_loadedTrackSampleRate);
        updateScrubPlayheadAnchor();
    }

    if (!m_scratch.scrubbing() && m_scratch.releaseGlide() && !scratchBridge->isInertiaActive()) {
        m_scratch.setReleaseGlide(false);
        restorePostScrubPlaybackState();
        if (mixerSource)
            mixerSource->setScratchTimbre(0.0f);
        emit scrubbingChanged();
    }

    m_snapTempoRatio = getTempoRatio();
    emitPlaybackStateChanged();
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
    const double resumeSec = std::max(0.0, m_scrubHoldPosition);
    transportSource.setPosition(resumeSec);
    m_scrubHoldPosition = resumeSec;
    m_atomicPlayheadPos.store(resumeSec, std::memory_order_relaxed);
    m_snapPosition = resumeSec;

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
    if (m_loopActive)
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
    if (m_scratch.scrubbing())
        return;

    if (mixerSource)
        mixerSource->armClickFreeTransition();

    m_phaseNudge      = 0.0;
    m_jogNudgePercent = 0.0;
    m_resyncBoost = false;

    const bool wasPlayingBeforeGrab = m_playRequested || transportSource.isPlaying();
    m_scratch.setReleaseGlide(false);
    m_scratch.setWasPlaying(wasPlayingBeforeGrab);
    m_scratch.setScrubbing(true);
    m_snapValid = false;
    m_scratchSnapReadPending = false;

    // During pre-roll countdown, m_scrubHoldPosition is negative; don't clobber it
    // with transport position (which is always 0 before beat 1).
    m_preRollCountdownActive = false;

    // Stop transport before anchoring — while playing, audio truth is transport
    // position, not latency-compensated visual position from QML.
    if (transportSource.isPlaying())
        transportSource.stop();

    if (wasPlayingBeforeGrab) {
        m_scrubHoldPosition = transportSource.getCurrentPosition();
    } else if (anchorPositionSec >= 0.0) {
        m_scrubHoldPosition = anchorPositionSec;
    } else if (m_scrubHoldPosition >= 0.0) {
        // keep frozen hold
    } else {
        m_scrubHoldPosition = transportSource.getCurrentPosition();
    }

    const double len = transportSource.getLengthInSeconds();
    const auto loopCtx = scratchLoopCtx();
    m_scrubHoldPosition = m_scratch.armGrab(m_scrubHoldPosition, len, loopCtx);

    if (scratchBridge) {
        scratchBridge->beginScratch(m_scrubHoldPosition,
                                    m_loadedTrackSampleRate,
                                    std::max(0.0, len),
                                    wasPlayingBeforeGrab,
                                    getTempoRatio());
        scratchBridge->setReverse(m_isReverse);
        scratchBridge->setKeylockPassthrough(false);
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

    emit progressChanged();
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

    emit progressChanged();
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

    m_scrubHoldPosition = std::max(0.0, scratchBridge->readPositionSeconds(m_loadedTrackSampleRate));
    m_scratch.setScrubbing(false);

    constexpr double kInertiaThreshold = 0.20;
    if (std::abs(scratchBridge->scratchRate()) > kInertiaThreshold) {
        scratchBridge->endScratch(true);
        m_scratch.setReleaseGlide(true);
        emit scrubbingChanged();
        return;
    }

    scratchBridge->exitScratchMode(m_scrubHoldPosition, m_loadedTrackSampleRate);
    restorePostScrubPlaybackState();

    emit scrubbingChanged();
}


void DjEngine::applyScratchReleaseJog(double deltaSeconds)
{
    if (!m_scratch.releaseGlide() || deltaSeconds == 0.0 || !scratchBridge)
        return;

    scratchBridge->addTargetDeltaSeconds(deltaSeconds, m_loadedTrackSampleRate);
    m_scrubHoldPosition = scratchBridge->targetPositionSeconds(m_loadedTrackSampleRate);
    updateScrubPlayheadAnchor();
    m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
    emit progressChanged();
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

