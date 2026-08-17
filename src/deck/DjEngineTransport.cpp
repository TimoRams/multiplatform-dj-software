#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"
#include "audio/DeckAudioPipeline.h"
#include "audio/DeckChannelProcessor.h"
#include "audio/RenderModeRouter.h"
#include "audio/TimeStretchProcessor.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "deck/DeckTransport.h"
#include "deck/MetadataUtils.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "waveform/WaveformAnalyzer.h"
#include "waveform/WaveformCache.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
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


const std::atomic<double>* DjEngine::audioPlayheadSink() const noexcept
{
    return m_transport ? &m_transport->audioPlayheadSink() : nullptr;
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
    m_memoryCues.clear();
    m_currentTrackId.clear();
    m_externalSourceId.clear();
    m_readOnlyExternalTrack = false;
    // Reset every cue/loop state under the new track generation, including
    // deferred quantized commands and any active loop.
    m_cueLoopController.beginTrack(m_trackLoader.currentGeneration());
    emit segmentsChanged();
    emit hotCuesChanged();
    emit savedLoopsChanged();
    emit memoryCuesChanged();
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
    m_transport->installPreparedTrack(cacheHandle,
        {m_trackLoader.currentGeneration(), trackSampleRate, trackDurationSeconds});
    syncScratchBridgeToTransport();
    // A newly loaded track starts at its beginning, and installPreparedTrack has
    // just zeroed every transport position to match. The handoff position given
    // to the render router has to agree: it used to be read from the outgoing
    // track's held position, so loading a new song into a deck left the readers
    // parked at the previous song's playhead instead of at 0.
    terminateScratchSession(0.0);
    ensureTransportRunningForPlayIntent();
}


void DjEngine::releaseTransportReaders()
{
    terminateScratchSession(0.0);
    m_transport->clearTrack(m_trackLoader.currentGeneration());
}


void DjEngine::ejectTrack()
{
    resetExternalCache();
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
    if (m_transport->playRequested())
        pause();
    else
        play();
}


void DjEngine::play()
{
    const bool scratchActive = m_scratch.scrubbing() || m_scratch.releaseGlide();
    const bool changed = scratchActive
        ? m_transport->setPlayingDuringScratch(true)
        : m_transport->setPlaying(true);
    ensureTransportRunningForPlayIntent();
    alignToSyncMasterOnPlay();
    if (changed)
        emit playingChanged();
}


void DjEngine::pause()
{
    const bool scratchActive = m_scratch.scrubbing() || m_scratch.releaseGlide();
    if (!m_transport->playRequested() && !m_transport->audioRunning()
        && !m_transport->preRollActive() && !scratchActive) {
        return; // Already paused
    }

    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().armClickFreeTransition();
    resetMainCueButtonState();

    // During scratch/release the platter reader owns the cursor. Pause changes
    // its destination to 0 instead of cancelling the session and cutting the
    // sound at the current sample. The eventual callback-owned handoff freezes
    // the normal reader at the exact coast endpoint.
    const bool playingChangedNow = scratchActive
        ? m_transport->setPlayingDuringScratch(false)
        : m_transport->setPlaying(false);
    if (playingChangedNow)
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
        qWarning() << "[DjEngine] Play requested without a selected audio device; output remains silent";
        return;
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
    if (!m_audioPipeline->renderModeRouterPtr())
        return;

    const double len = std::max(0.0, m_transport->trackLengthSeconds());
    m_audioPipeline->renderModeRouter().configureTrack(m_transport->sourceSampleRate(), len);
    m_audioPipeline->renderModeRouter().setReverse(m_transport->reverse());
    m_audioPipeline->renderModeRouter().setLoopRangeSeconds(m_cueLoopController.activeLoop().inSec, m_cueLoopController.activeLoop().outSec, m_cueLoopController.activeLoop().active, m_transport->sourceSampleRate());
    m_audioPipeline->renderModeRouter().setDeckTempoRatio(getTempoRatio());
    m_audioPipeline->renderModeRouter().setKeylockEnabled(m_keylock);

    const double pos = std::max(0.0, m_transport->audioPositionSeconds());
    m_audioPipeline->renderModeRouter().syncReadPositionSeconds(pos, m_transport->sourceSampleRate());
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
        m_trackLoader.setWaveformSeekHint(std::max(0.0, newPos));
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(std::max(0.0, newPos));
    }
    emit progressChanged();
}

void DjEngine::updateWaveformDemand(waveform::WaveformDemand demand)
{
    demand.generation = m_trackLoader.currentGeneration();
    m_waveformDemand = demand;
    m_trackLoader.setWaveformDemand(demand);
    if (m_analyzer)
        m_analyzer->setWaveformDemand(demand);
}


float DjEngine::vuLevelL() const
{
    return m_audioPipeline->mixerPtr() ? m_audioPipeline->mixer().m_peakL.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::vuLevelR() const
{
    return m_audioPipeline->mixerPtr() ? m_audioPipeline->mixer().m_peakR.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::preFaderVuLevelL() const
{
    return m_audioPipeline->mixerPtr() ? m_audioPipeline->mixer().m_preFaderPeakL.load(std::memory_order_relaxed) : 0.0f;
}


float DjEngine::preFaderVuLevelR() const
{
    return m_audioPipeline->mixerPtr() ? m_audioPipeline->mixer().m_preFaderPeakR.load(std::memory_order_relaxed) : 0.0f;
}


bool DjEngine::clipDetected() const
{
    // Master clip detection now comes from AudioEngine (summed signal).
    return AudioEngine::masterClipDetected_s();
}


float DjEngine::gainReduction() const
{
    return AudioEngine::gainReduction();
}


DeckAudioPipeline& DjEngine::audioEndpoint() const noexcept
{
    return *m_audioPipeline;
}

bool DjEngine::isReverse() const { return m_transport->reverse(); }
bool DjEngine::slipActive() const { return m_transport->slipEnabled(); }
bool DjEngine::isSlipDiverted() const
{
    return m_transport->slipDiverted(m_cueLoopController.activeLoop().active);
}

void DjEngine::setCueEnabled(bool value)
{
    const bool previous = AudioEngine::pflEnabled(m_deckIndex);
    AudioEngine::setPflEnabled(m_deckIndex, value);
    if (previous != value)
        emit cueEnabledChanged();
}

bool DjEngine::cueEnabled() const
{
    return AudioEngine::pflEnabled(m_deckIndex);
}

bool DjEngine::masterCueEnabled() const
{
    return AudioEngine::masterCueEnabled();
}

double DjEngine::headphoneMix() const
{
    return static_cast<double>(AudioEngine::headphoneMix());
}

void DjEngine::setMasterCueEnabled(bool value)
{
    const bool previous = AudioEngine::masterCueEnabled();
    AudioEngine::setMasterCueEnabled(value);
    if (previous != value)
        emit masterCueEnabledChanged();
}

void DjEngine::setHeadphoneMix(double value)
{
    const float clamped = static_cast<float>(std::clamp(value, 0.0, 1.0));
    const float previous = static_cast<float>(AudioEngine::headphoneMix());
    AudioEngine::setHeadphoneMix(clamped);
    if (std::abs(previous - clamped) > 0.0001f)
        emit headphoneMixChanged();
}

void DjEngine::setQuantizeEnabled(bool enabled)
{
    if (m_quantizeEnabled == enabled)
        return;
    m_quantizeEnabled = enabled;
    persistPerformanceToggle("quantize", enabled);
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
    const bool wasSlipDiverted = isSlipDiverted();
    if (!m_transport->setSlipEnabled(on))
        return;
    if (!on && wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit slipChanged();
}

void DjEngine::returnToSlipPosition()
{
    m_transport->returnToSlipPosition();
}


void DjEngine::loadTrack(const QString& rawPath)
{
    beginTrackLoad(rawPath);
}

void DjEngine::loadExternalTrack(const QVariantMap& request)
{
    if (!request.value(QStringLiteral("readOnlyExternal")).toBool()
        || request.value(QStringLiteral("sourceType")).toString()
            != QStringLiteral("rekordboxDevice")) {
        const QString error = QStringLiteral("Rejected untrusted external track request");
        if (m_trackLoadError != error) {
            m_trackLoadError = error;
            emit trackLoadErrorChanged();
        }
        return;
    }

    const QString path = request.value(QStringLiteral("filePath")).toString();
    ExternalTrackLoadSnapshot external;
    external.sourceId = request.value(QStringLiteral("sourceId")).toString();
    external.sourceAwareId = request.value(QStringLiteral("trackId")).toString();
    external.artworkPath = request.value(QStringLiteral("artworkPath")).toString();
    external.metadata.title = request.value(QStringLiteral("title")).toString();
    external.metadata.artist = request.value(QStringLiteral("artist")).toString();
    external.metadata.album = request.value(QStringLiteral("album")).toString();
    external.metadata.genre = request.value(QStringLiteral("genre")).toString();
    external.metadata.comment = request.value(QStringLiteral("comment")).toString();
    external.metadata.key = request.value(QStringLiteral("key")).toString();
    external.metadata.tagBpm = request.value(QStringLiteral("bpm")).toDouble();
    external.metadata.durationSec = request.value(QStringLiteral("durationSec")).toDouble();

    const QVariantList beatValues = request.value(QStringLiteral("beats")).toList();
    external.analysis.beats.reserve(static_cast<size_t>(beatValues.size()));
    double lastPosition = -1.0;
    double lastTempo = -1.0;
    int barIndex = 0;
    bool sawBeat = false;
    bool dynamicTempo = false;
    for (const QVariant& value : beatValues) {
        const QVariantMap map = value.toMap();
        const double position = map.value(QStringLiteral("positionSec")).toDouble();
        const double bpm = map.value(QStringLiteral("bpm")).toDouble();
        const int beatInBar = map.value(QStringLiteral("beatInBar")).toInt();
        if (!std::isfinite(position) || position < 0.0 || position <= lastPosition
            || !std::isfinite(bpm) || bpm <= 0.0 || bpm > 400.0
            || beatInBar < 1 || beatInBar > 4) {
            continue;
        }
        if (sawBeat && beatInBar == 1)
            ++barIndex;
        TrackData::BeatMarker marker;
        marker.positionSec = position;
        marker.isDownbeat = beatInBar == 1;
        marker.barIndex = barIndex;
        marker.barNumber = barIndex + 1;
        marker.beatInBar = beatInBar;
        marker.confidence = 1.0f;
        marker.lockedByUser = true;
        external.analysis.beats.push_back(marker);
        if (lastTempo < 0.0 || std::abs(lastTempo - bpm) > 0.001) {
            if (lastTempo > 0.0)
                dynamicTempo = true;
            external.analysis.beatGridInfo.tempoNodes.push_back(
                {position, bpm, 1.0f});
            lastTempo = bpm;
        }
        if (external.analysis.bpm <= 0.0)
            external.analysis.bpm = bpm;
        lastPosition = position;
        sawBeat = true;
    }
    if (external.analysis.hasBeatgrid()) {
        external.analysis.beatGridInfo.type = dynamicTempo
            ? TrackData::BeatGridType::DynamicTempo
            : TrackData::BeatGridType::ConstantTempo;
        external.analysis.beatGridInfo.lockedByUser = true;
        external.analysis.beatGridInfo.origin = TrackData::AnalysisOrigin::RekordboxDevice;
    }

    auto decodeCues = [](const QVariantList& values) {
        QVector<ImportedCueSnapshot> result;
        result.reserve(std::min<qsizetype>(values.size(), 64));
        for (const QVariant& value : values) {
            const QVariantMap map = value.toMap();
            ImportedCueSnapshot cue;
            cue.index = map.value(QStringLiteral("hotCueIndex"), -1).toInt();
            cue.positionSec = map.value(QStringLiteral("positionSec")).toDouble();
            cue.loopEndSec = map.value(QStringLiteral("loopEndSec"), -1.0).toDouble();
            cue.label = map.value(QStringLiteral("label")).toString();
            cue.color = map.value(QStringLiteral("color")).toString();
            if (std::isfinite(cue.positionSec) && cue.positionSec >= 0.0)
                result.append(cue);
            if (result.size() >= 64)
                break;
        }
        return result;
    };
    external.analysis.hotCues = decodeCues(
        request.value(QStringLiteral("hotCues")).toList());
    external.analysis.memoryCues = decodeCues(
        request.value(QStringLiteral("memoryCues")).toList());
    external.analysis.loops = decodeCues(
        request.value(QStringLiteral("loops")).toList());
    beginTrackLoad(path, std::move(external));
}

void DjEngine::externalSourceUnavailable(const QString& sourceId)
{
    if (!m_readOnlyExternalTrack || sourceId != m_externalSourceId)
        return;
    const QString error = m_externalCacheReady
        ? QStringLiteral("USB device removed; this track is fully cached and will continue playing")
        : QStringLiteral("USB device removed; buffered playback may continue until more data is needed");
    if (m_trackLoadError != error) {
        m_trackLoadError = error;
        emit trackLoadErrorChanged();
    }
}

void DjEngine::ejectExternalSource(const QString& sourceId)
{
    if (m_readOnlyExternalTrack && sourceId == m_externalSourceId && !m_externalCacheReady)
        ejectTrack();
}

void DjEngine::beginTrackLoad(
    QString rawPath, std::optional<ExternalTrackLoadSnapshot> external)
{
    resetExternalCache();
    QPointer<DjEngine> safeThis(this);
    AudioPageCache* const cache = &m_audioPageCache;
    auto completion = [safeThis, cache](TrackLoadResult result) mutable {
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
        };
    auto renderChunk = [safeThis](std::uint64_t generation, int totalLines,
                   int linesPerSecond, WaveformLineBatch chunks) {
            if (!safeThis)
                return;
            QMetaObject::invokeMethod(safeThis.data(),
                [safeThis, generation, totalLines, linesPerSecond,
                 chunks = std::move(chunks)]() mutable {
                    if (!safeThis
                        || generation != safeThis->m_trackLoader.currentGeneration()
                        || !safeThis->m_trackData) {
                        return;
                    }
                    safeThis->m_trackData->applyCachedWaveformLineBatch(
                        totalLines, linesPerSecond, std::move(chunks));
                },
                Qt::QueuedConnection);
        };
    const auto visualGeneration = external
        ? m_trackLoader.loadExternalTrack(std::move(rawPath), std::move(*external),
                                          std::move(completion), std::move(renderChunk))
        : m_trackLoader.loadTrack(std::move(rawPath), std::move(completion),
                                  std::move(renderChunk));

    // Invalidate the previous track's complete visual generation immediately.
    // Worker callbacks are queued back to this owner thread, so this happens
    // before any cache batch for the new request can become visible.
    if (m_trackData)
        m_trackData->beginVisualTrackLoad(visualGeneration);
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

    const bool hasExternalGrid = result.external
        && result.external->analysis.hasBeatgrid();
    if (result.metadata.tagBpm > 0.0 && !hasExternalGrid) {
        m_trackData->setBpmData(result.metadata.tagBpm, 0, result.metadata.sampleRate);
        m_trackData->ensureProvisionalBeatgrid(result.metadata.durationSec);
    }

    m_hasTrack = true;
    updateTrackDuration(result.metadata.durationSec);
    attachCacheToTransport(preparedCacheHandle, result.metadata.sampleRate,
                           result.metadata.durationSec);
    clearLoop();

    bool hasReusableAnalysis = false;
    if (result.external) {
        m_readOnlyExternalTrack = true;
        m_externalSourceId = result.external->sourceId;
        beginExternalCache(preparedCacheHandle);
        m_currentTrackId.clear();
        m_playLogged = false;
        if (hasExternalGrid) {
            const auto& imported = result.external->analysis;
            TrackData::ConfidenceInfo confidence;
            confidence.bpmConfidence = 1.0f;
            confidence.beatConfidence = 1.0f;
            confidence.downbeatConfidence = 1.0f;
            confidence.gridConfidence = 1.0f;
            const qint64 firstBeatSample = static_cast<qint64>(std::llround(
                imported.beats.front().positionSec * result.metadata.sampleRate));
            m_trackData->setBpmData(imported.bpm, firstBeatSample,
                                    result.metadata.sampleRate, imported.beats,
                                    confidence, imported.beatGridInfo);
            hasReusableAnalysis = true;
        }
        installExternalCues(*result.external);
    } else {
        hasReusableAnalysis = hydrateLibraryStateForTrack(
            result.canonicalPath, result.metadata.durationSec);
    }

    if (result.waveformCacheLoaded) {
        const int expected = result.waveformCache.totalExpected > 0
            ? result.waveformCache.totalExpected : result.waveformCache.waveform.size();
        m_trackData->setTotalExpected(expected);
        m_trackData->installCachedWaveform(
            std::move(result.waveformCache.waveform),
            result.waveformCache.globalMaxPeak,
            std::move(result.waveformCache.rgb),
            std::move(result.waveformCache.peakMip),
            std::move(result.waveformCache.preparedLines),
            std::move(result.instantOverview));
    } else if (result.waveformRenderCacheDeferred) {
        m_trackData->initializeCachedWaveformLines(
            result.waveformRenderTotalLines,
            result.waveformRenderLinesPerSecond,
            std::move(result.instantOverview));
    } else if (!result.instantOverview.isEmpty()) {
        m_trackData->setTotalExpected(std::max(1, result.instantOverviewExpected));
        m_trackData->setOverviewRgbData(std::move(result.instantOverview));
    }

    const bool hasReusableWaveform = result.waveformCacheLoaded
        || result.waveformRenderCacheAvailable;
    if (!(hasReusableWaveform && hasReusableAnalysis) && m_analyzer) {
        // Audio publication and first-page priming have already completed.
        // Start the background worker now; a fixed post-load delay only made
        // overview/detail latency depend on a timer and did not protect the
        // realtime callback (which never executes this analysis code).
        m_analyzer->startAnalysis(
            result.canonicalPath, m_transport->audioPositionSeconds(),
            result.generation, m_trackData->createAnalysisSeed());
    }

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

#include "deck/DjEngine.h"

#include "audio/DeckAudioPipeline.h"
#include "deck/DeckTransport.h"
#include "domain/TrackData.h"
#include "sync/SyncCoordinator.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>

namespace {

constexpr int kBeatsPerBar = 4;

QString normalizedTrackPath(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

double finiteOr(double value, double fallback = 0.0) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

} // namespace

double DjEngine::keylockLatencySeconds() const
{
    if (!m_keylock)
        return 0.0;
    const double sampleRate = m_transport->sourceSampleRate() > 0.0
        ? m_transport->sourceSampleRate() : 44100.0;
    return static_cast<double>(m_transport->keylockLatencySamples()) / sampleRate;
}

double DjEngine::getBeatPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (!std::isfinite(bpm) || bpm <= 0.0) return 0.0;

    const double position = getPosition();
    if (m_trackData->getBeatGrid().size() >= 2) {
        const auto [previous, beatLength] = beatIntervalAt(position);
        return std::clamp((position - previous) / beatLength, 0.0, 0.9999);
    }
    const double phase = std::fmod(position / (60.0 / bpm), 1.0);
    return phase < 0.0 ? phase + 1.0 : phase;
}

double DjEngine::getBarPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (!std::isfinite(bpm) || bpm <= 0.0) return 0.0;

    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const double beatPosition = getBeatPosition();
        int downbeatOffset = 0;
        if (const auto it = std::ranges::find_if(grid, &TrackData::BeatMarker::isDownbeat);
            it != grid.end()) {
            downbeatOffset = static_cast<int>(std::distance(grid.begin(), it) % kBeatsPerBar);
        }
        const double relative = beatPosition - static_cast<double>(downbeatOffset);
        const double barPosition = std::fmod(std::fmod(relative, kBeatsPerBar) + kBeatsPerBar,
                                             kBeatsPerBar);
        return barPosition / static_cast<double>(kBeatsPerBar);
    }

    const double sampleRate = m_trackData->getSampleRate();
    const double firstBeat = sampleRate > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sampleRate : 0.0;
    const double barLength = static_cast<double>(kBeatsPerBar) * 60.0 / bpm;
    const double relative = getPosition() - firstBeat;
    return std::fmod(std::fmod(relative, barLength) + barLength, barLength) / barLength;
}

double DjEngine::getBeatPosition() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (!std::isfinite(bpm) || bpm <= 0.0) return 0.0;

    const double position = getPosition();
    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const auto it = std::upper_bound(grid.begin(), grid.end(), position,
            [](double value, const TrackData::BeatMarker& marker) {
                return value < marker.positionSec;
            });
        const auto previous = it != grid.begin() ? std::prev(it) : grid.begin();
        const int beatIndex = static_cast<int>(std::distance(grid.begin(), previous));
        double beatLength = 60.0 / bpm;
        if (std::next(previous) != grid.end()) {
            const double candidate = std::next(previous)->positionSec - previous->positionSec;
            if (candidate > 0.001)
                beatLength = candidate;
        }
        return static_cast<double>(beatIndex) + ((position - previous->positionSec) / beatLength);
    }

    const double sampleRate = m_trackData->getSampleRate();
    const double firstBeat = sampleRate > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sampleRate : 0.0;
    return (position - firstBeat) / (60.0 / bpm);
}

engine::sync::DeckSyncInputSnapshot DjEngine::buildSyncInputSnapshot() const
{
    engine::sync::DeckSyncInputSnapshot input;
    const DeckTransportSnapshot transport = m_transport->snapshot();
    const double bpm = m_trackData ? m_trackData->getBpm() : 0.0;
    const auto& grid = m_trackData->getBeatGrid();
    const bool validBpm = std::isfinite(bpm) && bpm > 0.0;
    const bool gridValid = validBpm && grid.size() >= 2;
    const bool downbeatValid = gridValid
        && std::ranges::any_of(grid, &TrackData::BeatMarker::isDownbeat);

    input.hasTrack = transport.hasTrack && m_trackData;
    input.playing = transport.playing;
    input.scratching = m_scratch.scrubbing();
    input.scratchRelease = m_scratch.releaseGlide();
    input.reverse = transport.reverse;
    input.slipEnabled = transport.slipEnabled;
    input.loopActive = loopActive();
    input.keylockEnabled = m_keylock;
    input.beatgridValid = gridValid;
    input.downbeatValid = downbeatValid;
    input.trackBpm = validBpm ? bpm : 0.0;
    input.effectiveBpm = validBpm ? finiteOr(getCurrentBpm()) : 0.0;
    input.playbackRate = finiteOr(transport.playbackRate, 1.0);
    input.audiblePositionSeconds = finiteOr(transport.audiblePositionSeconds);
    input.beatPosition = finiteOr(getBeatPosition());
    input.barPosition = finiteOr(getBarPhase());
    input.beatPhase = finiteOr(getBeatPhase());
    input.beatLengthSeconds = validBpm
        ? finiteOr(beatIntervalAt(input.audiblePositionSeconds).lengthSec, 60.0 / bpm) : 0.0;
    input.keylockLatencySeconds = finiteOr(keylockLatencySeconds());
    input.beatConfidence = gridValid ? 1.0 : (validBpm ? 0.5 : 0.0);
    input.downbeatConfidence = downbeatValid ? 1.0 : 0.0;
    const QString path = normalizedTrackPath(m_trackFilePath);
    input.trackIdentity = path.isEmpty() ? 0 : static_cast<std::uint64_t>(qHash(path));
    if (!path.isEmpty() && input.trackIdentity == 0)
        input.trackIdentity = 1;
    input.trackGeneration = transport.trackGeneration;
    input.transportGeneration = transport.stateGeneration;
    return input;
}

void DjEngine::publishSyncInputAndApplyActions()
{
    m_syncCoordinator.updateDeck(m_deckIndex, buildSyncInputSnapshot());
    applyPendingSyncActions();
    refreshSyncFacadeSignals();
}

void DjEngine::applyPendingSyncActions()
{
    const auto actions = m_syncController->takeActions();
    const auto controllerState = m_syncController->snapshot();
    if (actions.masterGeneration != 0
        && actions.masterGeneration != controllerState.masterGeneration)
        return;
    if (actions.targetTrackGeneration != 0
        && actions.targetTrackGeneration != m_transport->trackGeneration())
        return;

    bool speedUpdateRequired = false;
    if (actions.tempoChanged) {
        const double clamped = std::clamp(actions.targetTempoPercent, -100.0, 100.0);
        if (std::abs(clamped - m_tempoPercent) > 1.0e-9) {
            m_tempoPercent = clamped;
            emit tempoChanged();
            speedUpdateRequired = true;
        }
    }
    if (actions.phaseNudgeChanged)
        speedUpdateRequired = true;
    if (speedUpdateRequired)
        updateSpeedAndPitch();
    if (actions.seekRequested)
        applySyncSeekOffset(actions.seekOffsetSeconds);
}

void DjEngine::refreshSyncFacadeSignals()
{
    const auto state = m_syncController->snapshot();
    if (state.syncEnabled != m_lastPublishedSyncEnabled) {
        m_lastPublishedSyncEnabled = state.syncEnabled;
        emit syncChanged();
    }
    if (state.isMaster != m_lastPublishedSyncMaster) {
        m_lastPublishedSyncMaster = state.isMaster;
        emit syncMasterChanged();
    }
}

void DjEngine::applySyncSeekOffset(double seekOffset)
{
    const double length = m_transport->trackLengthSeconds();
    const double newPosition = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
                                           length > 0.0 ? length : PRE_ROLL_SECONDS);
    if (newPosition < 0.0) {
        if (m_transport->preRollActive())
            m_transport->beginPreRoll(newPosition);
        else
            m_transport->setHeldPosition(newPosition);
    } else {
        m_transport->seekAudioToSeconds(newPosition);
        armSnapFromTransportPosition();
    }
}

void DjEngine::alignToSyncMasterOnPlay()
{
    if (!syncEnabled() || isSyncMaster())
        return;
    publishSyncInputAndApplyActions();
    m_syncCoordinator.requestPhaseArrange(m_deckIndex);
    applyPendingSyncActions();
}

void DjEngine::setSyncEnabled(bool enabled)
{
    if (syncEnabled() == enabled)
        return;
    m_syncCoordinator.setDeckSyncEnabled(m_deckIndex, enabled);
    applyPendingSyncActions();
    refreshSyncFacadeSignals();
}

void DjEngine::reSync()
{
    if (!syncEnabled() || isSyncMaster())
        return;
    publishSyncInputAndApplyActions();
    m_syncCoordinator.requestPhaseArrange(m_deckIndex, true);
    applyPendingSyncActions();
}
