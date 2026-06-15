#include "DjEngine.h"
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

float DjEngine::getProgress() const
{
    if (transportSource.getTotalLength() > 0.0)
        return static_cast<float>(transportSource.getCurrentPosition() / transportSource.getLengthInSeconds());
    return 0.0f;
}


float DjEngine::getDuration() const
{
    return static_cast<float>(transportSource.getLengthInSeconds());
}



double DjEngine::getPosition() const
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide() || m_preRollCountdownActive)
        return m_atomicPlayheadPos.load(std::memory_order_relaxed);
    // In pre-roll, transport is clamped at 0 while the visual position is
    // negative. m_scrubHoldPosition is the authoritative visual position here —
    // it is kept in sync by freezeTransportAt(), cue operations, and hot cues.
    if (m_scrubHoldPosition < 0.0)
        return m_scrubHoldPosition;
    return transportSource.getCurrentPosition();
}


double DjEngine::getVisualPosition() const
{
    // Active drag / release glide: playhead published via syncScratchReadPosition().
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return m_atomicPlayheadPos.load(std::memory_order_acquire);

    // Pre-roll countdown: interpolate using the pre-roll wall clock so the waveform
    // scrolls smoothly at sub-frame granularity, just like the normal snap-clock path.
    // Clamp at 0 to avoid briefly overshooting track start before tickTransportStopped()
    // clears the flag — the timer may lag up to one tick (~16 ms) behind the clock.
    if (m_preRollCountdownActive) {
        const double elapsed = static_cast<double>(m_preRollClock.nsecsElapsed()) * 1e-9;
        return std::min(m_preRollVisualStartPos + elapsed * getTempoRatio(), 0.0);
    }

    // When stopped/paused: return the frozen position (set by togglePlay).
    if (!m_snapValid || !transportSource.isPlaying())
        return getPosition();

    // Forward-interpolate from the last snapshot using elapsed wall-clock time.
    // This keeps the waveform smooth between onTimer() ticks.
    // Use the tempo ratio captured at snapshot time to avoid micro speed
    // discontinuities within a timer interval.
    const double elapsed = (static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9)
        * std::max(0.0001, m_snapTempoRatio);

    // When reverse is on, interpolate backwards instead of forwards
    double interpolated = m_isReverse
        ? m_snapPosition - elapsed
        : m_snapPosition + elapsed;

    // Render the scrolling waveform at the speaker-time playhead. Without this,
    // larger hardware buffers make the UI visibly lead/lag the audible beat.
    const double visualLatency = static_cast<double>(
        m_visualLatencyCompensationSeconds.load(std::memory_order_relaxed));
    double latencyBlend = 1.0;
    if (m_visualSeekSettleClock.isValid()) {
        constexpr double kVisualSeekSettleSeconds = 0.090;
        const double settleElapsed = static_cast<double>(m_visualSeekSettleClock.nsecsElapsed()) * 1e-9;
        if (settleElapsed < kVisualSeekSettleSeconds) {
            const double t = std::clamp(settleElapsed / kVisualSeekSettleSeconds, 0.0, 1.0);
            latencyBlend = t * t * (3.0 - 2.0 * t);
        }
    }
    interpolated += m_isReverse ? visualLatency * latencyBlend : -visualLatency * latencyBlend;

    double len = transportSource.getLengthInSeconds();
    interpolated = std::clamp(interpolated, -PRE_ROLL_SECONDS, len > 0.0 ? len : interpolated);

    return interpolated;
}


double DjEngine::getVisualPositionQml() const
{
    return getVisualPosition();
}


double DjEngine::loopPreviewOutPosition() const
{
    if (m_loopActive && m_loopOutSec > m_loopInSec)
        return m_loopOutSec;

    if (!m_loopInSet)
        return m_loopOutSec;

    const double trackLen = m_trackDurationSec;
    if (trackLen <= 0.0)
        return m_loopInSec;

    double outPos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        outPos = quantizedBeatAt(outPos);

    outPos = std::clamp(outPos, -PRE_ROLL_SECONDS, trackLen);

    constexpr double minLenSec = 0.001;
    if (outPos <= m_loopInSec + minLenSec) {
        const double beatDur = beatDurationAround(m_loopInSec);
        if (beatDur > 1e-4)
            outPos = std::min(trackLen, m_loopInSec + beatDur);
    }

    return outPos;
}


double DjEngine::getPlayheadPositionAtomic() const
{
    return m_atomicPlayheadPos.load(std::memory_order_acquire);
}


QImage DjEngine::currentCoverImage() const
{
    if (!m_coverProvider || m_deckId.isEmpty())
        return {};

    return m_coverProvider->coverImage(m_deckId);
}


bool DjEngine::isPlaying() const
{
    return m_playRequested;
}


TrackData* DjEngine::getTrackData() const
{
    return m_trackData;
}


void DjEngine::resetTrackLoadState()
{
    m_trackData->clear();
    m_currentSegments.clear();
    clearHotCueState();
    clearSavedLoopState();
    // Sentinel clearly outside the renderable pre-roll range so no cue is drawn
    // while no track is loaded.  The load path sets a real value after analysing
    // the first audible frame, so this value is never visible during playback.
    m_mainCueSec = -(PRE_ROLL_SECONDS + 1.0);
    resetMainCueButtonState();
    emit segmentsChanged();
    emit hotCuesChanged();
    emit savedLoopsChanged();
    emit beatgridLockedChanged();
}


void DjEngine::updateTrackDuration(double durationSec)
{
    m_trackDurationSec = durationSec;
    const int mins = static_cast<int>(durationSec) / 60;
    const int secs = static_cast<int>(durationSec) % 60;
    m_trackDuration = QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
}


void DjEngine::attachReaderToTransport(juce::AudioFormatReader* bufferedReader,
                                       juce::AudioFormatReader* directReader)
{
    static constexpr int kReaderReadAheadSamples = 1 << 18;
    static constexpr int kReaderReadAheadChannels = 2;

    const double scratchResumePos = m_scrubHoldPosition >= 0.0 ? m_scrubHoldPosition : 0.0;
    transportSource.stop();
    if (scratchBridge)
        scratchBridge->beginTransportSwap();
    transportSource.setSource(nullptr);

    reverseWrapSource.reset();
    bufferedReaderSource.reset();
    directReaderSource.reset();
    readerSource.reset();

    readerSource = std::make_unique<juce::AudioFormatReaderSource>(bufferedReader, true);
    directReaderSource = std::make_unique<juce::AudioFormatReaderSource>(directReader, true);
    bufferedReaderSource = std::make_unique<juce::BufferingAudioSource>(
        readerSource.get(),
        readAheadThread,
        false,
        kReaderReadAheadSamples,
        kReaderReadAheadChannels,
        true);
    reverseWrapSource = std::make_unique<ReverseStreamAudioSource>(
        bufferedReaderSource.get(),
        directReaderSource.get());
    reverseWrapSource->setReverse(m_isReverse);
    transportSource.setSource(reverseWrapSource.get(), 0, nullptr, bufferedReader->sampleRate);
    m_loadedTrackSampleRate = bufferedReader->sampleRate;
    transportSource.setPosition(0.0);
    syncScratchBridgeToTransport();
    terminateScratchSession(scratchResumePos);
    if (scratchBridge)
        scratchBridge->endTransportSwap();
    ensureTransportRunningForPlayIntent();
}


void DjEngine::releaseTransportReaders()
{
    transportSource.stop();
    transportSource.setSource(nullptr);

    if (scratchBridge)
        scratchBridge->beginTransportSwap();
    // Exit scratch while reader sources are still alive — scratchBridge holds a raw
    // pointer to directReaderSource via setScratchInputSource().
    terminateScratchSession(0.0);
    if (scratchBridge)
        scratchBridge->setScratchInputSource(nullptr);

    reverseWrapSource.reset();
    bufferedReaderSource.reset();
    directReaderSource.reset();
    readerSource.reset();

    if (scratchBridge)
        scratchBridge->endTransportSwap();
}


void DjEngine::ejectTrack()
{
    ++m_loadGen;

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->stopAnalysis();
    }

    m_playRequested = false;
    releaseTransportReaders();

    // Mark empty before clear()/dataCleared — waveform/FLX10 handlers must not
    // treat a mid-eject clear as a live track update.
    m_hasTrack = false;
    m_scrubHoldPosition = 0.0;
    m_atomicPlayheadPos.store(0.0, std::memory_order_relaxed);
    m_snapValid = false;

    resetTrackLoadState();

    m_trackTitle.clear();   m_trackArtist.clear();  m_trackAlbum.clear();
    m_trackGenre.clear();   m_trackComment.clear();
    m_trackKey.clear();     m_trackDuration.clear(); m_trackDurationSec = 0.0;
    m_hasCoverArt = false; m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);

    emit trackEjected();
    emit trackMetadataChanged();
    emit progressChanged();
    emit playingChanged();
}


void DjEngine::togglePlay()
{
    if (m_playRequested) {
        m_playRequested = false;
        if (mixerSource)
            mixerSource->armClickFreeTransition();
        resetMainCueButtonState();
        m_preRollCountdownActive = false;
        // Always freeze: calling stop() on an already-stopped transport is safe.
        // Skipping this when isPlaying()==false left the transport in a live state
        // whenever there was a brief race between the audio thread and this call.
        freezeTransportAt(getVisualPosition());
    } else {
        m_playRequested = true;
        if (m_scrubHoldPosition < 0.0) {
            m_preRollCountdownActive = true;
            m_preRollVisualStartPos = m_scrubHoldPosition;
            m_preRollClock.restart();
        } else {
            ensureTransportRunningForPlayIntent();
        }
    }

    emit playingChanged();
}


void DjEngine::play()
{
    if (!m_playRequested)
        m_playRequested = true;

    if (m_scrubHoldPosition < 0.0 && !m_preRollCountdownActive) {
        m_preRollCountdownActive = true;
        m_preRollVisualStartPos = m_scrubHoldPosition;
        m_preRollClock.restart();
    } else {
        ensureTransportRunningForPlayIntent();
    }
    emit playingChanged();
}


void DjEngine::pause()
{
    if (!m_playRequested && !transportSource.isPlaying() && !m_preRollCountdownActive)
        return; // Already paused

    m_playRequested = false;
    if (mixerSource)
        mixerSource->armClickFreeTransition();
    resetMainCueButtonState();
    m_preRollCountdownActive = false;
    freezeTransportAt(getVisualPosition());
    emit playingChanged();
}


void DjEngine::ensureTransportRunningForPlayIntent()
{
    if (!m_playRequested)
        return;

    // Pre-roll countdown manages its own transport start — don't interfere.
    if (m_preRollCountdownActive)
        return;

    if (m_scratch.scrubbing() || m_scratch.releaseGlide() || !m_hasTrack) {
        return;
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr) {
        qWarning() << "[DjEngine] Play requested without active audio device; trying to recover";
        const juce::String initErr = deviceManager.initialiseWithDefaultDevices(0, 2);
        if (initErr.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr) {
            qWarning() << "[DjEngine] Could not recover audio device on play";
            return;
        }
        refreshHardwareLatency();
    }

    if (transportSource.isPlaying()) {
        return;
    }

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0) {
        qWarning() << "[DjEngine] cannot start transport: invalid length" << len;
        return;
    }

    // Keep transport stopped at true EOF. As soon as the playhead is moved
    // back from the end, this function resumes playback automatically.
    const double pos = transportSource.getCurrentPosition();
    if (pos >= len - 0.0001) {
        return;
    }

    armSnapFromTransportPosition();
    transportSource.start();
}


void DjEngine::setSnapAnchor(double positionSec, bool valid)
{
    m_snapPosition = positionSec;
    m_snapTempoRatio = getTempoRatio();
    m_snapClock.restart();
    m_snapValid = valid;
    m_atomicPlayheadPos.store(positionSec, std::memory_order_relaxed);
}


void DjEngine::armVisualSeekSettle()
{
    m_visualSeekSettleClock.restart();
}


void DjEngine::armSnapFromTransportPosition()
{
    setSnapAnchor(transportSource.getCurrentPosition(), true);
}


void DjEngine::freezeTransportAt(double positionSec)
{
    transportSource.stop();
    transportSource.setPosition(std::max(0.0, positionSec));
    m_snapValid = false;
    // Keep m_scrubHoldPosition in sync so getPosition() returns the right value
    // when stopped in pre-roll (where transport is clamped at 0).
    m_scrubHoldPosition = positionSec;
    m_atomicPlayheadPos.store(positionSec, std::memory_order_relaxed);
}


void DjEngine::syncScratchBridgeToTransport()
{
    if (!scratchBridge)
        return;

    const double len = std::max(0.0, transportSource.getLengthInSeconds());
    scratchBridge->configureTrack(m_loadedTrackSampleRate, len);
    // Scratch pulls must seek the file directly — the buffered transport reader
    // causes dropouts/aliasing when the scratch resampler jumps read positions.
    scratchBridge->setScratchInputSource(directReaderSource
                                            ? static_cast<juce::AudioSource*>(directReaderSource.get())
                                            : reverseWrapSource.get());
    scratchBridge->setReverse(m_isReverse);
    scratchBridge->setLoopRangeSeconds(m_loopInSec, m_loopOutSec, m_loopActive, m_loadedTrackSampleRate);
    scratchBridge->setDeckTempoRatio(getTempoRatio());
    scratchBridge->setKeylockPassthrough(m_keylock);

    const double pos = std::max(0.0, transportSource.getCurrentPosition());
    scratchBridge->syncReadPositionSeconds(pos, m_loadedTrackSampleRate);
}


void DjEngine::setPosition(float progress)
{
    double len = transportSource.getLengthInSeconds();
    if (len > 0.0) {
        const double newPos = std::clamp(static_cast<double>(progress) * len,
                                         -PRE_ROLL_SECONDS,
                                         len);
        const double previousPos = getVisualPosition();
        if (m_playLogged && (newPos < previousPos - 15.0 || newPos <= len * 0.10)) {
            m_playLogged = false;
            m_playedAccumSec = 0.0;
            m_playHistoryClock.restart();
        }
        m_preRollCountdownActive = false;
        if (newPos < 0.0) {
            transportSource.stop();
            transportSource.setPosition(0.0);
            m_scrubHoldPosition = newPos;
            m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
            m_snapValid = false;
            if (m_playRequested) {
                m_preRollCountdownActive = true;
                m_preRollVisualStartPos = newPos;
                m_preRollClock.restart();
            }
        } else {
            transportSource.setPosition(newPos);
            m_scrubHoldPosition = newPos;
            ensureTransportRunningForPlayIntent();
            armSnapFromTransportPosition();
            armVisualSeekSettle();
        }
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(std::max(0.0, newPos));
    }
    emit progressChanged();
}


float DjEngine::vuLevelL() const
{
    return mixerSource ? mixerSource->m_peakL.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::vuLevelR() const
{
    return mixerSource ? mixerSource->m_peakR.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::preFaderVuLevelL() const
{
    return mixerSource ? mixerSource->m_preFaderPeakL.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::preFaderVuLevelR() const
{
    return mixerSource ? mixerSource->m_preFaderPeakR.load(std::memory_order_relaxed) : 0.0f;
}


bool DjEngine::clipDetected() const
{
    // Master clip detection now comes from DjMasterBus (summed signal).
    return DjMasterBus::masterClipDetected_s();
}


float DjEngine::gainReduction() const
{
    return DjMasterBus::gainReduction();
}


juce::AudioSource* DjEngine::getAudioSource() const
{
    return mixerSource.get();
}


const juce::AudioBuffer<float>& DjEngine::getPflBuffer() const
{
    return mixerSource->getPflBuffer();
}

