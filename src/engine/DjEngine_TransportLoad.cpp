#include "DjEngineCommonIncludes.h"

#include <QPointer>

void DjEngine::loadTrack(const QString& rawPath)
{
    QPointer<DjEngine> safeThis(this);
    m_trackLoader.loadTrack(rawPath, [safeThis](TrackLoadResult result) mutable {
        if (!safeThis)
            return;
        QMetaObject::invokeMethod(safeThis.data(),
            [safeThis, result = std::move(result)]() mutable {
                if (safeThis)
                    safeThis->applyPreparedTrack(std::move(result));
            },
            Qt::QueuedConnection);
    });
}

void DjEngine::applyPreparedTrack(TrackLoadResult result)
{
    if (result.generation != m_trackLoader.currentGeneration())
        return;
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

    auto preparedCacheHandle = m_audioPageCache.openTrack({result.canonicalPath});

    resetTrackLoadState();
    m_trackTitle = result.metadata.title;
    m_trackArtist = result.metadata.artist;
    m_trackAlbum = result.metadata.album;
    m_trackGenre = result.metadata.genre;
    m_trackComment = result.metadata.comment;
    m_trackKey = result.metadata.key;
    m_trackFilePath = result.canonicalPath;
    m_trackDuration.clear();
    m_hasCoverArt = false;
    m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);

    if (result.metadata.tagBpm > 0.0) {
        m_trackData->setBpmData(result.metadata.tagBpm, 0, result.metadata.sampleRate);
    }

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
        m_analyzer->startAnalysis(result.canonicalPath, m_transport->audioPositionSeconds());

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
