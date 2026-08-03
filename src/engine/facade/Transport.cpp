#include "FacadeIncludes.h"

#include <QPointer>
#include "audio/device/AudioDeviceService.h"


float DjEngine::getProgress() const
{
    const auto transport = m_transport->snapshot();
    if (transport.trackLengthSeconds > 0.0)
        return static_cast<float>(transport.audiblePositionSeconds / transport.trackLengthSeconds);
    return 0.0f;
}


float DjEngine::getDuration() const
{
    return static_cast<float>(m_transport->trackLengthSeconds());
}


double DjEngine::trackDurationSec() const
{
    return m_transport->trackLengthSeconds();
}



double DjEngine::getPosition() const
{
    return m_transport->positionSeconds(m_scratch.scrubbing() || m_scratch.releaseGlide());
}


double DjEngine::getVisualPosition() const
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return m_transport->playheadPositionAtomic();
    return m_transport->visualPositionSeconds(
        static_cast<double>(m_visualLatencyCompensationSeconds.load(std::memory_order_relaxed)));
}


double DjEngine::getVisualPositionQml() const
{
    return getVisualPosition();
}


double DjEngine::loopPreviewOutPosition() const
{
    if (m_cueLoopController.activeLoop().active && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec)
        return m_cueLoopController.activeLoop().outSec;

    if (!m_cueLoopController.activeLoop().inSet)
        return m_cueLoopController.activeLoop().outSec;

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return m_cueLoopController.activeLoop().inSec;

    double outPos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        outPos = quantizedBeatAt(outPos);

    outPos = std::clamp(outPos, -PRE_ROLL_SECONDS, trackLen);

    constexpr double minLenSec = 0.001;
    if (outPos <= m_cueLoopController.activeLoop().inSec + minLenSec) {
        const double beatDur = beatDurationAround(m_cueLoopController.activeLoop().inSec);
        if (beatDur > 1e-4)
            outPos = std::min(trackLen, m_cueLoopController.activeLoop().inSec + beatDur);
    }

    return outPos;
}


double DjEngine::getPlayheadPositionAtomic() const
{
    return m_transport->playheadPositionAtomic();
}


QImage DjEngine::currentCoverImage() const
{
    if (!m_coverProvider || m_deckId.isEmpty())
        return {};

    return m_coverProvider->coverImage(m_deckId);
}


bool DjEngine::isPlaying() const
{
    return m_transport->playRequested();
}


TrackData* DjEngine::getTrackData() const
{
    return m_trackData;
}


void DjEngine::resetTrackLoadState()
{
    m_trackData->clear();
    m_currentSegments.clear();
    // Reset every cue/loop state under the new track generation, including
    // deferred quantized commands and any active loop.
    m_cueLoopController.beginTrack(m_trackLoader.currentGeneration());
    emit segmentsChanged();
    emit hotCuesChanged();
    emit savedLoopsChanged();
    emit beatgridLockedChanged();
}


void DjEngine::updateTrackDuration(double durationSec)
{
    const int mins = static_cast<int>(durationSec) / 60;
    const int secs = static_cast<int>(durationSec) % 60;
    m_trackDuration = QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
}


void DjEngine::attachCacheToTransport(AudioCacheHandle cacheHandle, double trackSampleRate,
                                      double trackDurationSeconds)
{
    const double scratchResumePos = m_transport->heldPosition() >= 0.0 ? m_transport->heldPosition() : 0.0;
    m_transport->installPreparedTrack(cacheHandle,
        {m_trackLoader.currentGeneration(), trackSampleRate, trackDurationSeconds});
    syncScratchBridgeToTransport();
    terminateScratchSession(scratchResumePos);
    ensureTransportRunningForPlayIntent();
}


void DjEngine::releaseTransportReaders()
{
    terminateScratchSession(0.0);
    m_transport->clearTrack(m_trackLoader.currentGeneration());
}


void DjEngine::ejectTrack()
{
    m_trackLoader.requestCancel();
    if (!m_trackLoadError.isEmpty()) {
        m_trackLoadError.clear();
        emit trackLoadErrorChanged();
    }

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->stopAnalysis();
    }

    releaseTransportReaders();

    // Mark empty before clear()/dataCleared — waveform/FLX10 handlers must not
    // treat a mid-eject clear as a live track update.
    m_hasTrack = false;
    m_transport->setHeldPosition(0.0);

    resetTrackLoadState();

    m_trackTitle.clear();   m_trackArtist.clear();  m_trackAlbum.clear(); m_trackNumber.clear();
    m_trackFilePath.clear();
    m_trackGenre.clear();   m_trackComment.clear();
    m_trackKey.clear();     m_trackDuration.clear();
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
    if (m_transport->playRequested()) {
        if (m_audioGraph->mixerPtr())
            m_audioGraph->mixer().armClickFreeTransition();
        resetMainCueButtonState();
        m_transport->setPlaying(false);
    } else {
        m_transport->setPlaying(true);
        ensureTransportRunningForPlayIntent();
        alignToSyncMasterOnPlay();
    }

    emit playingChanged();
}


void DjEngine::play()
{
    const bool changed = m_transport->setPlaying(true);
    ensureTransportRunningForPlayIntent();
    alignToSyncMasterOnPlay();
    if (changed)
        emit playingChanged();
}


void DjEngine::pause()
{
    if (!m_transport->playRequested() && !m_transport->audioRunning() && !m_transport->preRollActive())
        return; // Already paused

    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().armClickFreeTransition();
    resetMainCueButtonState();
    if (m_transport->setPlaying(false))
        emit playingChanged();
}


void DjEngine::ensureTransportRunningForPlayIntent()
{
    if (!m_transport->playRequested())
        return;

    // Pre-roll countdown manages its own transport start — don't interfere.
    if (m_transport->preRollActive())
        return;

    if (m_scratch.scrubbing() || m_scratch.releaseGlide() || !m_hasTrack) {
        return;
    }

    if (m_audioDeviceService.manager().getCurrentAudioDevice() == nullptr) {
        qWarning() << "[DjEngine] Play requested without active audio device; trying to recover";
        if (!m_audioDeviceService.ensureDeviceAvailable()) {
            qWarning() << "[DjEngine] Could not recover audio device on play";
            return;
        }
        refreshHardwareLatency();
    }

    if (m_transport->audioRunning()) {
        return;
    }

    const double len = m_transport->trackLengthSeconds();
    if (len <= 0.0) {
        qWarning() << "[DjEngine] cannot start transport: invalid length" << len;
        return;
    }

    // Keep transport stopped at true EOF. As soon as the playhead is moved
    // back from the end, this function resumes playback automatically.
    const double pos = m_transport->audioPositionSeconds();
    if (pos >= len - 0.0001) {
        return;
    }

    m_transport->ensureAudioRunning();
}


void DjEngine::setSnapAnchor(double positionSec, bool valid)
{
    m_transport->setVisualAnchor(positionSec, valid);
}


void DjEngine::armVisualSeekSettle()
{
    m_transport->armVisualSeekSettle();
}


void DjEngine::armSnapFromTransportPosition()
{
    m_transport->setVisualAnchor(m_transport->audioPositionSeconds(), true);
}


void DjEngine::freezeTransportAt(double positionSec)
{
    cancelQuantizedCueJump();
    m_transport->freezeAt(positionSec);
}


void DjEngine::syncScratchBridgeToTransport()
{
    if (!m_audioGraph->scratchPtr())
        return;

    const double len = std::max(0.0, m_transport->trackLengthSeconds());
    m_audioGraph->scratch().configureTrack(m_transport->sourceSampleRate(), len);
    m_audioGraph->scratch().setReverse(m_transport->reverse());
    m_audioGraph->scratch().setLoopRangeSeconds(m_cueLoopController.activeLoop().inSec, m_cueLoopController.activeLoop().outSec, m_cueLoopController.activeLoop().active, m_transport->sourceSampleRate());
    m_audioGraph->scratch().setDeckTempoRatio(getTempoRatio());
    m_audioGraph->scratch().setKeylockPassthrough(m_keylock);

    const double pos = std::max(0.0, m_transport->audioPositionSeconds());
    m_audioGraph->scratch().syncReadPositionSeconds(pos, m_transport->sourceSampleRate());
}


void DjEngine::setPosition(float progress)
{
    cancelQuantizedCueJump();
    double len = m_transport->trackLengthSeconds();
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
        m_transport->seekToSeconds(newPos);
        ensureTransportRunningForPlayIntent();
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(std::max(0.0, newPos));
    }
    emit progressChanged();
}


float DjEngine::vuLevelL() const
{
    return m_audioGraph->mixerPtr() ? m_audioGraph->mixer().m_peakL.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::vuLevelR() const
{
    return m_audioGraph->mixerPtr() ? m_audioGraph->mixer().m_peakR.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::preFaderVuLevelL() const
{
    return m_audioGraph->mixerPtr() ? m_audioGraph->mixer().m_preFaderPeakL.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::preFaderVuLevelR() const
{
    return m_audioGraph->mixerPtr() ? m_audioGraph->mixer().m_preFaderPeakR.load(std::memory_order_relaxed) : 0.0f;
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


IDeckAudioEndpoint& DjEngine::audioEndpoint() const noexcept
{
    return *m_audioGraph;
}

bool DjEngine::isReverse() const { return m_transport->reverse(); }
bool DjEngine::slipActive() const { return m_transport->slipEnabled(); }
bool DjEngine::isSlipDiverted() const
{
    return m_transport->slipDiverted(m_cueLoopController.activeLoop().active);
}

void DjEngine::setCueEnabled(bool value)
{
    const bool previous = m_audioGraph->cueEnabledForMix();
    m_audioGraph->setCueEnabledForMix(value);
    if (previous != value)
        emit cueEnabledChanged();
}

bool DjEngine::cueEnabled() const
{
    return m_audioGraph->cueEnabledForMix();
}

bool DjEngine::masterCueEnabled() const
{
    return DjMasterBus::masterCueEnabled();
}

double DjEngine::headphoneMix() const
{
    return static_cast<double>(DjMasterBus::headphoneMix());
}

void DjEngine::setMasterCueEnabled(bool value)
{
    const bool previous = DjMasterBus::masterCueEnabled();
    DjMasterBus::setMasterCueEnabled(value);
    if (previous != value)
        emit masterCueEnabledChanged();
}

void DjEngine::setHeadphoneMix(double value)
{
    const float clamped = static_cast<float>(std::clamp(value, 0.0, 1.0));
    const float previous = static_cast<float>(DjMasterBus::headphoneMix());
    DjMasterBus::setHeadphoneMix(clamped);
    if (std::abs(previous - clamped) > 0.0001f)
        emit headphoneMixChanged();
}

void DjEngine::setQuantizeEnabled(bool enabled)
{
    if (m_quantizeEnabled == enabled)
        return;
    m_quantizeEnabled = enabled;
    emit quantizeEnabledChanged();
}

void DjEngine::setReverse(bool on)
{
    if (m_transport->reverse() == on)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_transport->setReverse(on);
    if (m_cueLoopController.activeLoop().active)
        applyLoopRangeToAudioSource();
    updateSpeedAndPitch();
    if (!on && wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit reverseChanged();
}

void DjEngine::setSlip(bool on)
{
    if (m_transport->setSlipEnabled(on))
        emit slipChanged();
}

void DjEngine::returnToSlipPosition()
{
    m_transport->returnToSlipPosition();
}


void DjEngine::loadTrack(const QString& rawPath)
{
    QPointer<DjEngine> safeThis(this);
    AudioPageCache* const cache = &m_audioPageCache;
    m_trackLoader.loadTrack(rawPath, [safeThis, cache](TrackLoadResult result) mutable {
        if (!safeThis) {
            cache->releaseTrack(result.cacheHandle);
            return;
        }
        QMetaObject::invokeMethod(safeThis.data(),
            [safeThis, cache, result = std::move(result)]() mutable {
                if (safeThis)
                    safeThis->applyPreparedTrack(std::move(result));
                else
                    cache->releaseTrack(result.cacheHandle);
            },
            Qt::QueuedConnection);
    });
}

void DjEngine::applyPreparedTrack(TrackLoadResult result)
{
    if (result.generation != m_trackLoader.currentGeneration()) {
        m_audioPageCache.releaseTrack(result.cacheHandle);
        return;
    }
    if (!result.succeeded()) {
        qWarning() << "[DeckTrackLoader] load failed:" << result.errorMessage;
        if (m_trackLoadError != result.errorMessage) {
            m_trackLoadError = result.errorMessage;
            emit trackLoadErrorChanged();
        }
        return;
    }

    if (!m_trackLoadError.isEmpty()) {
        m_trackLoadError.clear();
        emit trackLoadErrorChanged();
    }

    if (m_analyzer)
        m_analyzer->stopAnalysis();

    auto preparedCacheHandle = result.cacheHandle;

    resetTrackLoadState();
    m_trackTitle = result.metadata.title;
    m_trackArtist = result.metadata.artist;
    m_trackAlbum = result.metadata.album;
    m_trackNumber = result.metadata.trackNumber;
    m_trackGenre = result.metadata.genre;
    m_trackComment = result.metadata.comment;
    m_trackKey = result.metadata.key;
    m_trackFilePath = result.canonicalPath;
    m_trackDuration.clear();
    m_hasCoverArt = false;
    m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);

    if (result.metadata.tagBpm > 0.0)
        m_trackData->setBpmData(result.metadata.tagBpm, 0, result.metadata.sampleRate);

    m_hasTrack = true;
    updateTrackDuration(result.metadata.durationSec);
    attachCacheToTransport(preparedCacheHandle, result.metadata.sampleRate,
                           result.metadata.durationSec);
    clearLoop();

    const bool hasDbAnalysis = hydrateLibraryStateForTrack(
        result.canonicalPath, result.metadata.durationSec);

    if (result.waveformCacheLoaded) {
        const int expected = result.waveformCache.totalExpected > 0
            ? result.waveformCache.totalExpected : result.waveformCache.waveform.size();
        m_trackData->setTotalExpected(expected);
        m_trackData->replaceAllData(std::move(result.waveformCache.waveform),
                                    std::max(0.001f, result.waveformCache.globalMaxPeak));
        m_trackData->setRgbWaveformData(std::move(result.waveformCache.rgb));
        if (!result.waveformCache.peakMip.isEmpty())
            m_trackData->setPeakMipData(std::move(result.waveformCache.peakMip));
    } else if (!result.instantOverview.isEmpty()) {
        m_trackData->setTotalExpected(std::max(1, result.instantOverviewExpected));
        m_trackData->setOverviewRgbData(std::move(result.instantOverview));
    }

    if (!(result.waveformCacheLoaded && hasDbAnalysis) && m_analyzer)
        m_analyzer->startAnalysis(result.canonicalPath, m_transport->audioPositionSeconds(),
                                  result.generation, m_trackData->createAnalysisSeed());

    if (!result.coverImage.isNull() && m_coverProvider) {
        m_coverProvider->setCoverImage(m_deckId, result.coverImage);
        m_coverArtUrl = QString("image://coverart/%1?t=%2")
                            .arg(m_deckId)
                            .arg(QDateTime::currentMSecsSinceEpoch());
        m_hasCoverArt = true;
        if (m_libraryCoverService && !m_currentTrackId.isEmpty() && !result.coverBytes.isEmpty())
            m_libraryCoverService->publishCover(m_currentTrackId, result.coverBytes);
    }

    if (result.autoCueSec > 0.0
        && m_cueLoopController.mainCue().positionSec < 0.0
        && !m_transport->playRequested()
        && !m_transport->audioRunning()) {
        m_cueLoopController.mainCue().positionSec = result.autoCueSec;
        emit mainCueChanged();
        m_transport->seekAudioToSeconds(result.autoCueSec);
    }

    emit trackMetadataChanged();
    emit trackLoaded();
    emit progressChanged();
}
