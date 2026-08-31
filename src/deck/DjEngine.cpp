#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"
#include "audio/DeckAudioPipeline.h"
#include "audio/DeckChannelProcessor.h"
#include "audio/RenderModeRouter.h"
#include "audio/TimeStretchProcessor.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "deck/DeckTransport.h"
#include "deck/MetadataUtils.h"
#include "fx/FxProcessor.h"
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
#include "audio/device/AudioDeviceService.h"
#include "app/SettingsManager.h"
#include "sync/SyncCoordinator.h"


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

} // namespace

DjEngine::DjEngine(AudioDeviceService& audioDeviceService, AudioPageCache& audioPageCache,
                   DeckAudioPipeline& audioPipeline,
                   ControlClock& controlClock, engine::sync::SyncCoordinator& syncCoordinator,
                   int deckIndex, QObject* parent)
    : QObject(parent)
    , m_audioDeviceService(audioDeviceService)
    , m_audioPageCache(audioPageCache)
    , m_audioPipeline(&audioPipeline)
    , m_transport(std::make_unique<DeckTransport>(*m_audioPipeline))
    , m_trackLoader(audioPageCache, static_cast<int>(WAVEFORM_POINTS_PER_SECOND))
    , m_syncCoordinator(syncCoordinator)
    , m_controlClock(controlClock)
    , m_deckIndex(deckIndex)
    , m_syncController(std::make_unique<engine::sync::DeckSyncController>(
          engine::sync::DeckSyncController::Configuration {deckIndex}))
{
    m_syncCoordinator.registerDeck(m_deckIndex, *m_syncController);
    connect(this, &DjEngine::loopChanged, this, &DjEngine::slipPreviewChanged);
    connect(this, &DjEngine::scrubbingChanged, this, &DjEngine::slipPreviewChanged);
    connect(this, &DjEngine::slipChanged, this, &DjEngine::slipPreviewChanged);
    connect(this, &DjEngine::reverseChanged, this, &DjEngine::slipPreviewChanged);
    connect(this, &DjEngine::playingChanged, this, &DjEngine::slipPreviewChanged);
    connect(this, &DjEngine::seekPreviewChanged, this, &DjEngine::slipPreviewChanged);

    m_trackData = new TrackData(this);
    m_analyzer = std::make_unique<WaveformAnalyzer>(
        &formatManager,
        static_cast<int>(WAVEFORM_POINTS_PER_SECOND));
    m_analysisMailbox = std::make_shared<AnalyzerResultMailbox>();
    const auto analysisMailbox = m_analysisMailbox;
    m_analyzer->setProgressCallback([analysisMailbox](double progress, bool active, auto generation) {
        analysisMailbox->publishProgress(progress, active, generation);
    });
    m_analyzer->setChunkCallback([analysisMailbox](auto generation, int firstBin, int totalBins,
                                                    auto waveform, auto rgb,
                                                    auto normalizationState) {
        AnalyzerResultMailbox::Chunk chunk;
        chunk.generation = generation;
        chunk.firstBin = firstBin;
        chunk.totalBins = totalBins;
        chunk.waveform = std::make_shared<const QVector<TrackData::WaveformBin>>(std::move(waveform));
        chunk.rgb = std::make_shared<const QVector<TrackData::RgbWaveformFrame>>(std::move(rgb));
        chunk.normalizationState = normalizationState;
        analysisMailbox->publishChunk(std::move(chunk));
    });
    m_analyzer->setOverviewCallback(
        [analysisMailbox](auto generation, int totalBins, auto overview) {
            AnalyzerResultMailbox::Overview publication;
            publication.generation = generation;
            publication.totalBins = totalBins;
            publication.samples = std::make_shared<const QVector<
                TrackData::RgbWaveformFrame>>(std::move(overview));
            analysisMailbox->publishOverview(std::move(publication));
        });
    m_analyzer->setCompletionCallback([analysisMailbox](bool completed, auto generation,
                                                  const QString& filePath,
                                                  WaveformAnalyzer::ResultPtr result) {
        analysisMailbox->publish({completed, generation, filePath, std::move(result)});
    });
    clearHotCueState();

    // When the analyzer detects a key, override the (often absent) ID3 key field.
    m_analysisPersistTimer = new QTimer(this);
    m_analysisPersistTimer->setSingleShot(true);
    m_analysisPersistTimer->setInterval(400);
    connect(m_analysisPersistTimer, &QTimer::timeout, this, [this]() {
        persistCurrentAnalysisToLibrary();
    });
    m_externalCacheTimer = new QTimer(this);
    m_externalCacheTimer->setInterval(250);
    m_externalCacheTimer->setTimerType(Qt::CoarseTimer);
    connect(m_externalCacheTimer, &QTimer::timeout, this, &DjEngine::updateExternalCache);

    // Opt-in playback health log. Set BROCKDJ_AUDIO_DIAGNOSTICS=1 to find out
    // whether audible clicks come from the page cache failing to keep up: a
    // rising starvation count means the deck ran out of resident audio and held
    // its last sample. Off by default, so a normal run pays nothing for it.
    if (!qEnvironmentVariableIsEmpty("BROCKDJ_AUDIO_DIAGNOSTICS")) {
        auto* diagnostics = new QTimer(this);
        diagnostics->setInterval(1000);
        diagnostics->setTimerType(Qt::CoarseTimer);
        connect(diagnostics, &QTimer::timeout, this, [this] {
            if (!m_audioPipeline)
                return;
            const auto stats = m_audioPipeline->realtimeStats();
            const auto delta = [](std::uint64_t current, std::uint64_t previous) {
                return current >= previous ? current - previous : current;
            };
            if (stats.playbackStarvationBlocks == m_lastStarvationBlocks
                && stats.playbackDroppedRequests == m_lastDroppedRequests
                && stats.scratchStarvationBlocks == m_lastScratchStarvationBlocks
                && stats.scratchDroppedRequests == m_lastScratchDroppedRequests
                && stats.keylockSeedsOnAudioThread == m_lastKeylockSeedsOnAudioThread)
                return;
            qInfo().nospace()
                << "[AudioDiag] deck=" << m_deckId
                << " playbackStarvation+="
                << delta(stats.playbackStarvationBlocks, m_lastStarvationBlocks)
                << " playbackDropped+="
                << delta(stats.playbackDroppedRequests, m_lastDroppedRequests)
                << " playbackMisses=" << stats.playbackPageMisses
                << " scratchStarvation+="
                << delta(stats.scratchStarvationBlocks, m_lastScratchStarvationBlocks)
                << " scratchDropped+="
                << delta(stats.scratchDroppedRequests, m_lastScratchDroppedRequests)
                << " scratchMisses=" << stats.scratchPageMisses
                << " scratchRecoveries=" << stats.scratchRecoveryEvents
                << " scratchGenerationMismatches=" << stats.scratchGenerationMismatches
                << " diskReadsOnAudioThread=" << stats.diskReadsFromAudioThread
                << " blockingLocks=" << stats.blockingLockAttempts
                << " keylockSeeds=" << stats.keylockSeeds
                << " keylockSeedsInCallback+="
                << delta(stats.keylockSeedsOnAudioThread, m_lastKeylockSeedsOnAudioThread)
                << " keylockBridgeBlocks=" << stats.keylockSeedBridgeBlocks
                << " worstKeylockSeedUs=" << stats.worstKeylockSeedMicros;
            m_lastStarvationBlocks = stats.playbackStarvationBlocks;
            m_lastDroppedRequests = stats.playbackDroppedRequests;
            m_lastScratchStarvationBlocks = stats.scratchStarvationBlocks;
            m_lastScratchDroppedRequests = stats.scratchDroppedRequests;
            m_lastKeylockSeedsOnAudioThread = stats.keylockSeedsOnAudioThread;
        });
        diagnostics->start();
    }

    connect(m_trackData, &TrackData::keyAnalyzed, this, [this]() {
        QString analysedKey = m_trackData->getDetectedKey();
        if (!analysedKey.isEmpty()) {
            m_trackKey = analysedKey;
            emit trackMetadataChanged();
        }

        m_analysisPersistTimer->start();
    });

    // When BPM analysis finishes, re-emit tempoChanged so that currentBpm
    // and tempoRatio Q_PROPERTYs update in QML.
    connect(m_trackData, &TrackData::bpmAnalyzed, this, [this]() {
        emit tempoChanged();
        m_analysisPersistTimer->start();
        publishSyncInputAndApplyActions();
    });

    connect(m_trackData, &TrackData::beatgridChanged, this, [this]() {
        m_analysisPersistTimer->start();
    });

    connect(m_trackData, &TrackData::segmentsAnalyzed, this, [this]() {
        const auto segments = m_trackData->getSegments();

        QVariantList asVariant;
        asVariant.reserve(static_cast<int>(segments.size()));
        for (const auto& s : segments) {
            QVariantMap m;
            m.insert("label", s.label);
            m.insert("startTime", s.startTime);
            m.insert("endTime", s.endTime);
            m.insert("colorHex", s.colorHex);
            m.insert("confidence", s.confidence);
            asVariant.push_back(m);
        }

        m_currentSegments = asVariant;
        emit segmentsChanged();

        if (m_libraryDb && !m_currentTrackId.isEmpty())
            m_libraryDb->updateTrackSegments(m_currentTrackId, segments);
    });

    juce::MessageManager::getInstance();
    formatManager.registerBasicFormats();

    // Audio callback is registered by AudioEngine, not per-deck.

    m_audioPipeline->mixer().setTrim(static_cast<float>(m_trim));
    m_audioPipeline->mixer().setFader(static_cast<float>(m_volume));
    m_keylock = persistedPerformanceToggle("keylock", false);
    m_quantizeEnabled = persistedPerformanceToggle("quantize", false);
    updateSpeedAndPitch();

    // AudioEngine prepares the registered DeckAudioPipeline endpoint.

    refreshHardwareLatency();
    connect(&m_audioDeviceService, &AudioDeviceService::configurationChanged, this, [this]() {
        refreshHardwareLatency();
        if (!m_scratch.scrubbing() && !m_scratch.releaseGlide())
            updateSpeedAndPitch();
    });
    connect(&m_audioDeviceService, &AudioDeviceService::errorChanged,
            this, &DjEngine::audioDeviceErrorChanged);
    connect(&m_audioDeviceService, &AudioDeviceService::fallbackChanged,
            this, &DjEngine::audioDeviceFallbackChanged);
    m_playHistoryClock.start();
    m_vuNotifyClock.start();
    m_progressNotifyClock.start();

    ControlClock::Callbacks clockCallbacks;
    clockCallbacks.fast = [this](const ControlTickContext& context) {
        onFastControlTick(context);
    };
    clockCallbacks.transport = [this](const ControlTickContext& context) {
        onTransportControlTick(context);
    };
    clockCallbacks.syncInput = [this](const ControlTickContext& context) {
        onSyncInputControlTick(context);
    };
    clockCallbacks.syncApply = [this](const ControlTickContext& context) {
        onSyncApplyControlTick(context);
    };
    clockCallbacks.waveform = [this](const ControlTickContext& context) {
        onWaveformControlTick(context);
    };
    clockCallbacks.meters = [this](const ControlTickContext& context) {
        onMeterControlTick(context);
    };
    m_controlClockRegistration = m_controlClock.registerCallbacks(std::move(clockCallbacks));
}

bool DjEngine::persistedPerformanceToggle(const char* name, bool fallback) const
{
    const int normalizedDeckIndex = std::clamp(m_deckIndex, 0, 3);
    const QString key = QStringLiteral("performance/deck%1/%2")
        .arg(QChar::fromLatin1(static_cast<char>('A' + normalizedDeckIndex)),
             QString::fromLatin1(name));
    const QString fallbackValue = fallback ? QStringLiteral("1") : QStringLiteral("0");
    return SettingsManager::getInstance().getUiState(key, fallbackValue) == QLatin1String("1");
}

void DjEngine::persistPerformanceToggle(const char* name, bool enabled)
{
    const int normalizedDeckIndex = std::clamp(m_deckIndex, 0, 3);
    const QString key = QStringLiteral("performance/deck%1/%2")
        .arg(QChar::fromLatin1(static_cast<char>('A' + normalizedDeckIndex)),
             QString::fromLatin1(name));
    SettingsManager::getInstance().setUiState(
        key, enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

void DjEngine::resetExternalCache()
{
    if (m_externalCacheTimer)
        m_externalCacheTimer->stop();
    m_externalCacheHandle = {};
    m_externalCacheNextPage = 0;
    if (m_externalCacheProgress != 0.0 || m_externalCacheReady) {
        m_externalCacheProgress = 0.0;
        m_externalCacheReady = false;
        emit externalCacheProgressChanged();
    }
}

void DjEngine::beginExternalCache(AudioCacheHandle handle)
{
    resetExternalCache();
    if (!handle.isValid())
        return;
    const auto requiredBytes = static_cast<std::uint64_t>(handle.pageCount())
        * static_cast<std::uint64_t>(handle.channelCount())
        * static_cast<std::uint64_t>(AudioPage::kSamplesPerChannel) * sizeof(float);
    if (requiredBytes > m_audioPageCache.budgetBytes() / 2) {
        m_trackLoadError = QStringLiteral(
            "Track is too large for the configured cache; increase BROCKDJ_AUDIO_CACHE_MB before ejecting USB.");
        emit trackLoadErrorChanged();
        return;
    }
    m_externalCacheHandle = handle;
    // Do not run a second decoder against removable media from the shared page
    // cache. Even background requests can begin a non-preemptible USB decode
    // immediately before a playback miss, which starves the audio source.
    // External staging must use a separate low-I/O worker before it can run
    // concurrently with playback.
}

void DjEngine::updateExternalCache()
{
    if (!m_externalCacheHandle.isValid()) {
        if (m_externalCacheTimer)
            m_externalCacheTimer->stop();
        return;
    }
    // A full USB cache is opportunistic. Never decode cache-warming pages once
    // the user has requested playback; the cache worker must serve audio
    // read-ahead without competing USB I/O first.
    if (m_transport->playRequested())
        return;
    const auto stats = m_audioPageCache.handleStats(m_externalCacheHandle);
    if (stats.totalPages <= 0) {
        resetExternalCache();
        return;
    }
    const double progress = static_cast<double>(stats.residentPages)
        / static_cast<double>(stats.totalPages);
    if (std::abs(m_externalCacheProgress - progress) > 0.0001) {
        m_externalCacheProgress = progress;
        emit externalCacheProgressChanged();
    }
    if (stats.residentPages == stats.totalPages) {
        if (m_audioPageCache.sealTrack(m_externalCacheHandle)) {
            m_externalCacheProgress = 1.0;
            m_externalCacheReady = true;
            if (m_externalCacheTimer)
                m_externalCacheTimer->stop();
            emit externalCacheProgressChanged();
        }
        return;
    }
    const auto last = std::min(m_externalCacheNextPage, stats.totalPages - 1);
    if (m_audioPageCache.requestRange(
            m_externalCacheHandle, m_externalCacheNextPage, last, AudioCachePriority::Background)) {
        m_externalCacheNextPage = last + 1;
        if (m_externalCacheNextPage >= stats.totalPages)
            m_externalCacheNextPage = 0;
    }
}


DjEngine::~DjEngine()
{
    m_controlClockRegistration.reset();
    m_trackLoader.shutdownAndJoin();
    m_syncCoordinator.unregisterDeck(m_deckIndex);

    if (m_analyzer) {
        m_analyzer->stopAnalysis();
        m_analyzer.reset();
    }
    releaseTransportReaders();
}

void DjEngine::prepareForShutdown()
{
    m_controlClockRegistration.reset();
    m_trackLoader.shutdownAndJoin();

    if (m_analysisPersistTimer)
        QObject::disconnect(m_analysisPersistTimer, nullptr, this, nullptr);

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->stopAnalysis();
    }

    m_transport->setPlaying(false);
}

void DjEngine::setCoverArtProvider(CoverArtProvider* provider, const QString& deckId)
{
    m_coverProvider = provider;
    m_deckId = deckId;
}


void DjEngine::setLibraryCoverService(LibraryCoverService* service)
{
    m_libraryCoverService = service;
}


void DjEngine::setLibraryDatabase(LibraryDatabase* db)
{
    m_libraryDb = db;
    loadHotCuesForCurrentTrack();
    loadSavedLoopsForCurrentTrack();
    loadMainCueForCurrentTrack();
    emit beatgridLockedChanged();
}


void DjEngine::persistCurrentAnalysisToLibrary()
{
    if (!m_libraryDb || m_currentTrackId.isEmpty() || !m_trackData)
        return;

    auto result = m_trackData->createAnalysisSeed();
    const double bpm = result.bpm;
    const QString key = result.detectedKey.trimmed();
    const auto& beatGrid = result.beats;

    if (bpm <= 0.0 && key.isEmpty() && beatGrid.empty())
        return;

    const QFileInfo info(m_trackFilePath);
    result.complete = true;
    result.validated = true;
    result.analysisVersion = analysis::kCurrentAnalysisVersion;
    result.identity.canonicalFilePath = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    result.identity.trackGeneration = m_trackLoader.currentGeneration();
    result.identity.fileSize = static_cast<std::uint64_t>(std::max<qint64>(0, info.size()));
    result.identity.fileModifiedMs = info.lastModified().toMSecsSinceEpoch();
    result.identity.requestGeneration = m_analyzer ? m_analyzer->generation() : 0;
    (void)m_libraryDb->requestAnalysisPersistence(m_currentTrackId, result);
}


void DjEngine::onFastControlTick(const ControlTickContext& context)
{
    (void)context;
    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        cancelQuantizedCueJump();
        tickScratchPhysics();
        return;
    }

    decayJogNudge();
}

void DjEngine::onTransportControlTick(const ControlTickContext& context)
{
    (void)context;
    // The search cursor is the single position authority until release. Letting
    // updateControlState() sample the stopped/old audio reader here caused the
    // waveform to flicker back to the grab position between MIDI frames.
    if (fastSearchActive()) {
        m_playHistoryClock.restart();
        return;
    }
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return;
    if (m_audioPipeline->renderModeRouterPtr()
        && m_audioPipeline->renderModeRouter().normalPlaybackHandoffPending())
        return;

    if (m_transport->audioRunning())
        serviceQuantizedCueJump();

    const auto transportUpdate = m_transport->updateControlState(
        {m_cueLoopController.activeLoop().active,
         m_cueLoopController.activeLoop().inSec,
         m_cueLoopController.activeLoop().outSec},
        false,
        m_cueLoopController.mainCue().previewActive);
    if (transportUpdate.positionChanged)
        notifyProgressIfNeeded();

    if (transportUpdate.audioRunning) {
        // Accumulate real audible playback time for play-count logging. Use wall
        // time instead of assuming the control timer fires exactly every 4 ms.
        if (!m_playLogged && !m_currentTrackId.isEmpty() && m_transport->playRequested()) {
            const double elapsedSec = m_playHistoryClock.isValid()
                ? static_cast<double>(m_playHistoryClock.restart()) / 1000.0
                : 0.0;
            m_playedAccumSec += std::clamp(elapsedSec, 0.0, 0.25);

            const double playheadSec = std::max(0.0, static_cast<double>(getVisualPosition()));
            const double trackLength = m_transport->trackLengthSeconds();
            const double nearEndSec = trackLength > 0.0 ? trackLength * 0.80 : 1e9;
            const double thresholdSec = playHistoryThresholdSeconds(trackLength);
            const bool enoughPlayback = m_playedAccumSec >= thresholdSec;
            const bool mixedNearEnd = playheadSec >= nearEndSec
                && m_playedAccumSec >= std::min(6.0, thresholdSec * 0.6);

            if (enoughPlayback || mixedNearEnd) {
                m_playLogged = true;
                if (m_libraryDb)
                    m_libraryDb->logPlay(m_currentTrackId);
            }
        }
    } else {
        m_playHistoryClock.restart();
    }

    updateFxBeatSyncPosition();
}

void DjEngine::onSyncInputControlTick(const ControlTickContext& context)
{
    (void)context;
    m_syncCoordinator.stageDeckInput(m_deckIndex, buildSyncInputSnapshot());
}

void DjEngine::onSyncApplyControlTick(const ControlTickContext& context)
{
    (void)context;
    applyPendingSyncActions();
    refreshSyncFacadeSignals();
}

void DjEngine::onWaveformControlTick(const ControlTickContext& context)
{
    (void)context;
    if (m_analysisMailbox && m_analyzer) {
        if (auto overview = m_analysisMailbox->takeOverview()) {
            if (overview->generation == m_analyzer->generation()
                && overview->samples && !overview->samples->isEmpty()) {
                m_trackData->publishWaveformOverview(
                    overview->totalBins,
                    static_cast<int>(WAVEFORM_POINTS_PER_SECOND),
                    QVector<TrackData::RgbWaveformFrame>(*overview->samples));
            }
        }
        const auto chunks = m_analysisMailbox->takeChunks(
            m_waveformDemand, WAVEFORM_POINTS_PER_SECOND);
        for (const auto& chunk : chunks) {
            if (chunk.generation == m_analyzer->generation()
                && chunk.waveform && chunk.rgb) {
                m_trackData->applyProgressiveWaveformChunk(
                    chunk.firstBin, chunk.totalBins, *chunk.waveform, *chunk.rgb,
                    false, chunk.normalizationState);
            }
        }
        // Coalesce a worker burst: each source chunk receives at most one new
        // immutable snapshot per 60 Hz control tick, while the first chunk is
        // still visible on that same tick.
        if (!chunks.empty())
            m_trackData->flushProgressiveWaveformLines();
        double value = 0.0; bool active = false;
        WaveformAnalyzer::AnalysisGeneration progressGeneration = 0;
        if (m_analysisMailbox->takeProgress(value, active, progressGeneration)
            && progressGeneration == m_analyzer->generation())
            m_trackData->reportAnalysisProgress(value, active);
        if (auto completion = m_analysisMailbox->take()) {
            const auto& result = completion->result;
            if (completion->completed && result
                && completion->generation == m_analyzer->generation()
                && completion->filePath == m_trackFilePath
                && result->identity.trackGeneration == m_trackLoader.currentGeneration()) {
                const QFileInfo current(completion->filePath);
                if (result->identity.fileSize == static_cast<std::uint64_t>(std::max<qint64>(0, current.size()))
                    && result->identity.fileModifiedMs == current.lastModified().toMSecsSinceEpoch()) {
                    m_trackData->applyAnalysisResult(*result);
                    m_trackData->reportAnalysisProgress(1.0, false);
                }
            }
        }
    }
    notifyProgressIfNeeded();
}

void DjEngine::onMeterControlTick(const ControlTickContext& context)
{
    (void)context;
    notifyVuMetersIfNeeded();
}


double DjEngine::getPreRollSeconds() const
{
    return PRE_ROLL_SECONDS;
}


void DjEngine::applyMixerEq()
{
    if (!m_audioPipeline->mixerPtr())
        return;
    m_audioPipeline->mixer().setEq(static_cast<float>(m_eqLow),
                       static_cast<float>(m_eqMid),
                       static_cast<float>(m_eqHigh));
}


void DjEngine::applyMixerFilter()
{
    if (!m_audioPipeline->mixerPtr())
        return;
    m_audioPipeline->mixer().setFilterVal(static_cast<float>(m_filter));
}


void DjEngine::notifyVuMetersIfNeeded()
{
    const float vuL    = vuLevelL();
    const float vuR    = vuLevelR();
    const float preL   = preFaderVuLevelL();
    const float preR   = preFaderVuLevelR();
    const float masterL = masterVuLevelL();
    const float masterR = masterVuLevelR();
    const float gr     = gainReduction();
    const bool routed  = onAir();

    constexpr float kVuEps = 0.015f;
    constexpr float kGrEps = 0.012f;
    const bool vuMoved = std::abs(vuL - m_lastNotifiedVuL) > kVuEps
                      || std::abs(vuR - m_lastNotifiedVuR) > kVuEps
                      || std::abs(preL - m_lastNotifiedPreVuL) > kVuEps
                      || std::abs(preR - m_lastNotifiedPreVuR) > kVuEps
                      || std::abs(masterL - m_lastNotifiedMasterVuL) > kVuEps
                      || std::abs(masterR - m_lastNotifiedMasterVuR) > kVuEps;
    const bool grMoved = std::abs(gr - m_lastNotifiedGr) > kGrEps;
    const bool onAirMoved = routed != m_lastNotifiedOnAir;
    const qint64 msSince = m_vuNotifyClock.isValid() ? m_vuNotifyClock.elapsed() : 1000;

    if (!vuMoved && !grMoved && !onAirMoved && msSince < 50)
        return;

    m_lastNotifiedVuL   = vuL;
    m_lastNotifiedVuR   = vuR;
    m_lastNotifiedPreVuL = preL;
    m_lastNotifiedPreVuR = preR;
    m_lastNotifiedMasterVuL = masterL;
    m_lastNotifiedMasterVuR = masterR;
    m_lastNotifiedGr    = gr;
    m_lastNotifiedOnAir = routed;
    m_vuNotifyClock.restart();

    if (vuMoved || msSince >= 50)
        emit vuLevelChanged();
    if (grMoved || msSince >= 50)
        emit gainReductionChanged();
    if (onAirMoved)
        emit onAirChanged();
}


void DjEngine::notifyProgressIfNeeded()
{
    const double posSec = getVisualPosition();
    constexpr double kProgressSecEps = 0.025;
    const bool posMoved = std::abs(posSec - m_lastNotifiedProgressSec) > kProgressSecEps;
    const qint64 msSince = m_progressNotifyClock.isValid() ? m_progressNotifyClock.elapsed() : 1000;

    if (!posMoved && msSince < 50)
        return;

    m_lastNotifiedProgressSec = posSec;
    m_progressNotifyClock.restart();
    emit progressChanged();
}


void DjEngine::applyVolume(double value)
{
    const double clamped = std::clamp(value, kVolumeMin, kVolumeMax);
    if (metadata::nearlyEqual(m_volume, clamped))
        return;

    m_volume = clamped;
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().setFader(static_cast<float>(m_volume));
}


void DjEngine::setVolume(double value)
{
    applyVolume(value);
    emit volumeChanged();
}


void DjEngine::applyTrim(double value)
{
    const double clamped = std::clamp(value, kTrimMin, kTrimMax);
    if (metadata::nearlyEqual(m_trim, clamped))
        return;

    m_trim = clamped;
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().setTrim(static_cast<float>(m_trim));
}


void DjEngine::setTrim(double value)
{
    applyTrim(value);
    emit trimChanged();
}


void DjEngine::applyEqHigh(double value)
{
    const double clamped = std::clamp(value, kEqMin, kEqMax);
    if (metadata::nearlyEqual(m_eqHigh, clamped))
        return;

    m_eqHigh = clamped;
    applyMixerEq();
}


void DjEngine::setEqHigh(double value)
{
    applyEqHigh(value);
    emit eqHighChanged();
}


void DjEngine::applyEqMid(double value)
{
    const double clamped = std::clamp(value, kEqMin, kEqMax);
    if (metadata::nearlyEqual(m_eqMid, clamped))
        return;

    m_eqMid = clamped;
    applyMixerEq();
}


void DjEngine::setEqMid(double value)
{
    applyEqMid(value);
    emit eqMidChanged();
}


void DjEngine::applyEqLow(double value)
{
    const double clamped = std::clamp(value, kEqMin, kEqMax);
    if (metadata::nearlyEqual(m_eqLow, clamped))
        return;

    m_eqLow = clamped;
    applyMixerEq();
}


void DjEngine::setEqLow(double value)
{
    applyEqLow(value);
    emit eqLowChanged();
}


void DjEngine::applyFilter(double value)
{
    const double clamped = std::clamp(value, kFilterMin, kFilterMax);
    if (metadata::nearlyEqual(m_filter, clamped))
        return;

    m_filter = clamped;
    applyMixerFilter();
}


void DjEngine::setFilter(double value)
{
    applyFilter(value);
    emit filterChanged();
}

void DjEngine::applyPolarityInverted(bool inverted)
{
    if (m_polarityInverted == inverted)
        return;

    m_polarityInverted = inverted;
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().setPolarityInverted(inverted);
}

void DjEngine::setPolarityInverted(bool inverted)
{
    applyPolarityInverted(inverted);
    emit polarityInvertedChanged();
}

bool DjEngine::hydrateLibraryStateForTrack(const QString& rawPath, double durationSec)
{
    if (!m_libraryDb)
        return false;

    const int durationSeconds = static_cast<int>(durationSec);
    int bitrateKbps = 0;
    const juce::File file(rawPath.toStdString());
    if (durationSec > 0.0) {
        const auto bytes = static_cast<double>(file.getSize());
        bitrateKbps = static_cast<int>(std::lround((bytes * 8.0) / durationSec / 1000.0));
    }

    const QString existingId = m_libraryDb->trackIdForFilePath(rawPath);
    m_currentTrackId = existingId.isEmpty()
        ? TrackIdGenerator::generate(m_trackArtist, m_trackTitle, durationSeconds, rawPath)
        : existingId;
    m_trackFilePath = rawPath;
    m_playLogged = false;
    m_playedAccumSec = 0.0;
    m_playHistoryClock.restart();
    m_libraryDb->addTrack(m_currentTrackId,
                          m_trackTitle, m_trackArtist, durationSeconds, rawPath, bitrateKbps,
                          m_trackGenre, m_trackAlbum, m_trackComment);

    bool hasDatabaseAnalysis = false;
    LibraryDatabase::AnalysisSnapshot cachedAnalysis;
    const bool foundDatabaseAnalysis =
        m_libraryDb->tryGetAnalysisData(m_currentTrackId, &cachedAnalysis)
        && cachedAnalysis.isAnalyzed;
    const bool userGridMustBePreserved = foundDatabaseAnalysis
        && (cachedAnalysis.beatGridInfo.userModified
            || cachedAnalysis.beatGridInfo.lockedByUser);
    const bool analysisVersionIsCurrent = foundDatabaseAnalysis
        && cachedAnalysis.analysisVersion
            >= static_cast<int>(analysis::kCurrentAnalysisVersion);
    if (foundDatabaseAnalysis
        && (analysisVersionIsCurrent || userGridMustBePreserved)) {
        hasDatabaseAnalysis = true;
        m_currentSegments = m_libraryDb->trackSegmentsForTrack(m_currentTrackId);
        emit segmentsChanged();

        if (cachedAnalysis.bpm > 0.0) {
            m_trackData->setBpmData(cachedAnalysis.bpm,
                                    cachedAnalysis.firstBeatSample,
                                    cachedAnalysis.sampleRate,
                                    cachedAnalysis.beatGrid,
                                    cachedAnalysis.confidence,
                                    cachedAnalysis.beatGridInfo);
        }

        const QString cachedKey = cachedAnalysis.key.trimmed();
        if (!cachedKey.isEmpty()) {
            m_trackKey = cachedKey;
            m_trackData->setKeyData(cachedKey);
        }

        std::vector<TrackSegment> cachedSegments;
        cachedSegments.reserve(static_cast<size_t>(m_currentSegments.size()));
        for (const QVariant& value : m_currentSegments) {
            const QVariantMap map = value.toMap();
            TrackSegment segment;
            segment.label = map.value(QStringLiteral("label")).toString();
            segment.startTime = static_cast<float>(map.value(QStringLiteral("startTime")).toDouble());
            segment.endTime = static_cast<float>(map.value(QStringLiteral("endTime")).toDouble());
            segment.colorHex = map.value(QStringLiteral("colorHex")).toString();
            segment.confidence = static_cast<float>(map.value(QStringLiteral("confidence")).toDouble());
            if (segment.endTime > segment.startTime + 0.01f)
                cachedSegments.push_back(segment);
        }
        if (!cachedSegments.empty())
            m_trackData->setSegmentsData(std::move(cachedSegments));
    } else {
        if (foundDatabaseAnalysis) {
            qDebug() << "[DjEngine] Ignoring stale automatic analysis"
                     << cachedAnalysis.analysisVersion << "for"
                     << m_currentTrackId.left(12)
                     << "current=" << analysis::kCurrentAnalysisVersion;
        }
        m_currentSegments = QVariantList();
        emit segmentsChanged();
    }

    loadHotCuesForCurrentTrack();
    loadSavedLoopsForCurrentTrack();
    loadMainCueForCurrentTrack();
    emit beatgridLockedChanged();
    return hasDatabaseAnalysis;
}

namespace {

double nearestDownbeatAnchor(const std::vector<TrackData::BeatMarker>& grid, double currentSec)
{
    double best = currentSec;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& marker : grid) {
        if (!marker.isDownbeat)
            continue;
        const double distance = std::abs(marker.positionSec - currentSec);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = marker.positionSec;
        }
    }
    return best;
}

} // namespace

void DjEngine::setDownbeatAtPosition(double anchorSec)
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;

    const double trackLengthSec = static_cast<double>(m_transport->trackLengthSeconds());
    if (trackLengthSec <= 0.0)
        return;

    m_trackData->shiftBeatgridToDownbeat(anchorSec, trackLengthSec);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}

void DjEngine::setDownbeatAtCurrentPosition()
{
    setDownbeatAtPosition(static_cast<double>(getVisualPosition()));
}

void DjEngine::nudgeBeatgridMs(double milliseconds)
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;

    const double trackLengthSec = m_transport->trackLengthSeconds();
    if (trackLengthSec <= 0.0 || std::abs(milliseconds) < 1e-6)
        return;

    m_trackData->nudgeBeatgrid(milliseconds / 1000.0, trackLengthSec);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}

void DjEngine::nudgeBeatgridBeats(double beats)
{
    if (!m_trackData || std::abs(beats) < 1e-6)
        return;

    const double position = static_cast<double>(getVisualPosition());
    const double beatDuration = beatDurationAround(position);
    if (beatDuration <= 1e-4)
        return;

    nudgeBeatgridMs(beats * beatDuration * 1000.0);
}

void DjEngine::doubleBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;
    const double trackLengthSec = static_cast<double>(m_transport->trackLengthSeconds());
    if (trackLengthSec <= 0.0)
        return;

    const double anchor = nearestDownbeatAnchor(
        m_trackData->getBeatGrid(), static_cast<double>(getVisualPosition()));
    m_trackData->setBpm(m_trackData->getBpm() * 2.0);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLengthSec);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();
}

void DjEngine::halveBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;
    const double trackLengthSec = static_cast<double>(m_transport->trackLengthSeconds());
    if (trackLengthSec <= 0.0)
        return;

    const double anchor = nearestDownbeatAnchor(
        m_trackData->getBeatGrid(), static_cast<double>(getVisualPosition()));
    m_trackData->setBpm(m_trackData->getBpm() / 2.0);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLengthSec);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();
}

void DjEngine::updateSpeedAndPitch()
{
    const double phaseNudge = m_syncController
        ? m_syncController->snapshot().phaseNudgePercent : 0.0;
    double baseSpeedMultiplier = 1.0 + ((m_tempoPercent + phaseNudge) / 100.0);
    baseSpeedMultiplier = std::clamp(baseSpeedMultiplier, 0.01, 8.0);
    const double jogNudgeRatio = std::clamp(1.0 + (m_jogNudgePercent / 100.0), 0.94, 1.06);

    const bool scratchVarispeed = m_scratch.scrubbing() || m_scratch.releaseGlide();
    m_transport->setPlaybackRate(baseSpeedMultiplier);
    m_transport->setJogNudgeRatio(jogNudgeRatio);
    m_transport->setKeylockEnabled(!scratchVarispeed && m_keylock);
}

void DjEngine::setKeylock(bool on)
{
    if (m_keylock == on)
        return;
    m_keylock = on;
    persistPerformanceToggle("keylock", on);
    updateSpeedAndPitch();
    emit keylockChanged();
}

void DjEngine::setKeySemitoneOffset(double semitones)
{
    semitones = std::isfinite(semitones) ? std::clamp(semitones, -12.0, 12.0) : 0.0;
    if (m_keySemitoneOffset == semitones)
        return;
    m_keySemitoneOffset = semitones;
    m_transport->setKeySemitoneOffset(semitones);
    emit keySemitoneOffsetChanged();
}

void DjEngine::applyTempoPercent(double percent)
{
    percent = std::clamp(percent, -100.0, 100.0);
    if (m_tempoPercent == percent)
        return;
    m_tempoPercent = percent;

    updateSpeedAndPitch();
    emit tempoChanged();
    if (syncEnabled())
        publishSyncInputAndApplyActions();
}

void DjEngine::setTempoPercent(double percent)
{
    if (syncEnabled() && !isSyncMaster())
        return;
    applyTempoPercent(percent);
}

void DjEngine::setTempoRangePercent(double percent)
{
    const double clamped = std::clamp(percent, 6.0, 100.0);
    if (std::abs(m_tempoRangePercent - clamped) < 0.001)
        return;

    m_tempoRangePercent = clamped;
    applyTempoPercent(std::clamp(m_tempoPercent, -m_tempoRangePercent, m_tempoRangePercent));
    emit tempoRangeChanged();
}

void DjEngine::setManualBpm(double bpm)
{
    if (!m_trackData)
        return;

    const double clamped = std::clamp(bpm, 20.0, 300.0);
    const double trackLengthSec = static_cast<double>(m_transport->trackLengthSeconds());
    const double anchor = nearestDownbeatAnchor(
        m_trackData->getBeatGrid(), static_cast<double>(getVisualPosition()));

    m_trackData->setBpm(clamped);
    if (trackLengthSec > 0.0)
        m_trackData->shiftBeatgridToDownbeat(anchor, trackLengthSec);

    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();
    if (syncEnabled())
        publishSyncInputAndApplyActions();
}

#include "deck/DjEngine.h"

#include "audio/AudioEngine.h"
#include "audio/device/AudioDeviceService.h"

bool DjEngine::applyAudioDeviceSettings(int sampleRate, int bufferSize)
{
    return m_audioDeviceService.applySettings(sampleRate, bufferSize);
}

bool DjEngine::applyAudioDeviceSettings(const QString& type, const QString& output,
                                        int sampleRate, int bufferSize,
                                        int masterFirstChannel, int headphonesFirstChannel,
                                        int boothFirstChannel)
{
    return m_audioDeviceService.applySettings(type, output, sampleRate, bufferSize,
                                              masterFirstChannel, headphonesFirstChannel,
                                              boothFirstChannel);
}

void DjEngine::setOutputFirstChannel(int firstChannel)
{
    m_audioDeviceService.setOutputFirstChannel(firstChannel);
}

void DjEngine::setMasterVolume(float value) { AudioEngine::setMasterVolume(value); }
void DjEngine::setAntiClip(bool enabled) { AudioEngine::setAntiClipEnabled(enabled); }

QStringList DjEngine::getAvailableAudioDeviceTypes() const
{
    return m_audioDeviceService.availableDeviceTypes();
}

QStringList DjEngine::getAvailableAudioOutputDevices(const QString& type) const
{
    return m_audioDeviceService.availableOutputDevices(type);
}

QStringList DjEngine::getAvailableOutputChannelPairs(const QString& type,
                                                     const QString& output) const
{
    return m_audioDeviceService.availableOutputChannelPairs(type, output);
}

QString DjEngine::getCurrentAudioDeviceType() const { return m_audioDeviceService.currentDeviceType(); }
QString DjEngine::getCurrentAudioOutputDevice() const { return m_audioDeviceService.currentOutputDevice(); }
int DjEngine::getCurrentAudioSampleRate() const { return m_audioDeviceService.currentSampleRate(); }
int DjEngine::getCurrentAudioBufferSize() const { return m_audioDeviceService.currentBufferSize(); }
bool DjEngine::isJackServerRunning() const { return m_audioDeviceService.isJackServerRunning(); }
QString DjEngine::jackServerStatus() const { return m_audioDeviceService.jackServerStatus(); }
QString DjEngine::lastAudioDeviceError() const { return m_audioDeviceService.lastError(); }
QString DjEngine::audioDeviceFallbackMessage() const { return m_audioDeviceService.fallbackMessage(); }

#include "deck/DjEngine.h"

#include "audio/AudioEngine.h"
#include "audio/device/AudioDeviceService.h"
#include "deck/DeckTransport.h"
#include "fx/FxProcessor.h"

#include <QDebug>

#include <algorithm>


void DjEngine::refreshHardwareLatency()
{
    if (auto* device = m_audioDeviceService.manager().getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        if (latency.sampleRate > 0.0) {
            m_latencySeconds.store(
                static_cast<float>(static_cast<double>(latency.backendOutputSamples) / latency.sampleRate),
                std::memory_order_relaxed);
            const int visualCompSamples = latency.callbackBufferSamples + latency.backendOutputSamples;
            m_visualLatencyCompensationSeconds.store(
                static_cast<float>(std::clamp(static_cast<double>(visualCompSamples) / latency.sampleRate, 0.0, 0.250)),
                std::memory_order_relaxed);
        }

        const bool changed = (latency.backendOutputSamples != m_lastLoggedEffectiveSamples)
                          || (latency.outputRawSamples != m_lastLoggedOutputRawSamples)
                          || (latency.callbackBufferSamples != m_lastLoggedBufferSamples)
                          || (latency.roundedSampleRate() != m_lastLoggedSampleRateRounded)
                          || m_latencyLoggedNoDevice;

        if (changed) {
            if (qEnvironmentVariableIntValue("BROCKDJ_AUDIO_TRACE") > 0) {
                qInfo() << "[DjEngine] Backend output latency:" << latency.backendOutputSamples
                        << "smp" << "(" << m_latencySeconds.load(std::memory_order_relaxed) << "s)"
                        << "raw:" << latency.outputRawSamples
                        << "buf:" << latency.callbackBufferSamples
                        << "sr:" << latency.roundedSampleRate();
            }
            m_lastLoggedEffectiveSamples  = latency.backendOutputSamples;
            m_lastLoggedOutputRawSamples  = latency.outputRawSamples;
            m_lastLoggedBufferSamples     = latency.callbackBufferSamples;
            m_lastLoggedSampleRateRounded = latency.roundedSampleRate();
            m_latencyLoggedNoDevice = false;
        }
    } else {
        m_visualLatencyCompensationSeconds.store(0.0f, std::memory_order_relaxed);
        if (!m_latencyLoggedNoDevice) {
            if (qEnvironmentVariableIntValue("BROCKDJ_AUDIO_TRACE") > 0)
                qInfo() << "[DjEngine] No audio device yet; keeping last known latency";
            m_latencyLoggedNoDevice = true;
        }
    }
}


DjEngine::LatencySnapshot DjEngine::buildLatencySnapshot() const
{
    LatencySnapshot snapshot;
    if (m_lastLatencySnapshot.sampleRate > 0.0)
        snapshot.sampleRate = m_lastLatencySnapshot.sampleRate;

    if (auto* device = m_audioDeviceService.manager().getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        snapshot.outputRawSamples = latency.outputRawSamples;
        snapshot.bufferSamples = latency.callbackBufferSamples;
        snapshot.backendOutputSamples = latency.backendOutputSamples;
        if (latency.sampleRate > 0.0)
            snapshot.sampleRate = latency.sampleRate;
    } else if (m_lastLatencySnapshot.sampleRate > 0.0) {
        snapshot.outputRawSamples = m_lastLatencySnapshot.outputRawSamples;
        snapshot.bufferSamples = m_lastLatencySnapshot.bufferSamples;
        snapshot.backendOutputSamples = m_lastLatencySnapshot.backendOutputSamples;
    }

    snapshot.keylockSamples = m_transport->keylockLatencySamples();

    snapshot.limiterSamples = std::max(0, AudioEngine::limiterLatencySamples());
    snapshot.resamplerSamples = 0;
    snapshot.mixerFxSamples = 0;
    m_lastLatencySnapshot = snapshot;
    return snapshot;
}


double DjEngine::totalLatencyMs() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return 0.0;

    const int totalSamples = snapshot.bufferSamples
                           + snapshot.backendOutputSamples
                           + snapshot.keylockSamples
                           + snapshot.resamplerSamples
                           + snapshot.limiterSamples
                           + snapshot.mixerFxSamples;
    return (static_cast<double>(totalSamples) / snapshot.sampleRate) * 1000.0;
}


QVariantList DjEngine::latencyBreakdown() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return {};

    const auto toMs = [sampleRate = snapshot.sampleRate](int samples) -> double {
        return (static_cast<double>(samples) / sampleRate) * 1000.0;
    };

    QVariantList rows;
    const int audioDeviceSamples = snapshot.bufferSamples + snapshot.backendOutputSamples;
    const int dspSamples = snapshot.keylockSamples
                         + snapshot.resamplerSamples
                         + snapshot.limiterSamples
                         + snapshot.mixerFxSamples;

    QVariantMap audioDeviceRow;
    audioDeviceRow.insert("name", QStringLiteral("Audio Device Total"));
    audioDeviceRow.insert("samples", audioDeviceSamples);
    audioDeviceRow.insert("ms", toMs(audioDeviceSamples));
    audioDeviceRow.insert("countInTotal", false);
    rows.push_back(audioDeviceRow);

    QVariantMap bufferRow;
    bufferRow.insert("name", QStringLiteral("Device Buffer / Period"));
    bufferRow.insert("samples", snapshot.bufferSamples);
    bufferRow.insert("ms", toMs(snapshot.bufferSamples));
    bufferRow.insert("countInTotal", true);
    rows.push_back(bufferRow);

    QVariantMap driverRow;
    driverRow.insert("name", QStringLiteral("Backend / Hardware"));
    driverRow.insert("samples", snapshot.backendOutputSamples);
    driverRow.insert("ms", toMs(snapshot.backendOutputSamples));
    driverRow.insert("countInTotal", true);
    rows.push_back(driverRow);

    QVariantMap dspRow;
    dspRow.insert("name", QStringLiteral("DSP Latency"));
    dspRow.insert("samples", dspSamples);
    dspRow.insert("ms", toMs(dspSamples));
    dspRow.insert("countInTotal", false);
    rows.push_back(dspRow);

    QVariantMap keylockRow;
    keylockRow.insert("name", QStringLiteral("Keylock / Time-stretch"));
    keylockRow.insert("samples", snapshot.keylockSamples);
    keylockRow.insert("ms", toMs(snapshot.keylockSamples));
    keylockRow.insert("countInTotal", true);
    rows.push_back(keylockRow);

    QVariantMap resamplerRow;
    resamplerRow.insert("name", QStringLiteral("Resampler"));
    resamplerRow.insert("samples", snapshot.resamplerSamples);
    resamplerRow.insert("ms", toMs(snapshot.resamplerSamples));
    resamplerRow.insert("countInTotal", true);
    rows.push_back(resamplerRow);

    QVariantMap limiterRow;
    limiterRow.insert("name", QStringLiteral("Limiter Lookahead"));
    limiterRow.insert("samples", snapshot.limiterSamples);
    limiterRow.insert("ms", toMs(snapshot.limiterSamples));
    limiterRow.insert("countInTotal", true);
    rows.push_back(limiterRow);

    QVariantMap fxRow;
    fxRow.insert("name", QStringLiteral("Mixer / FX Chain"));
    fxRow.insert("samples", snapshot.mixerFxSamples);
    fxRow.insert("ms", toMs(snapshot.mixerFxSamples));
    fxRow.insert("countInTotal", true);
    rows.push_back(fxRow);

    QVariantMap totalRow;
    totalRow.insert("name", QStringLiteral("Total Estimated Latency"));
    totalRow.insert("samples", audioDeviceSamples + dspSamples);
    totalRow.insert("ms", toMs(audioDeviceSamples + dspSamples));
    totalRow.insert("countInTotal", false);
    rows.push_back(totalRow);

    return rows;
}


QVariantMap DjEngine::audioPerformanceStats() const
{
    QVariantMap stats;
    const auto snapshot = buildLatencySnapshot();
    const double callbackBudgetUsec = snapshot.sampleRate > 0.0
        ? (static_cast<double>(snapshot.bufferSamples) / snapshot.sampleRate) * 1000000.0
        : 0.0;

    stats.insert(QStringLiteral("callbackAverageUsec"), AudioEngine::callbackAverageUsec());
    stats.insert(QStringLiteral("callbackWorstUsec"), AudioEngine::callbackWorstUsec());
    stats.insert(QStringLiteral("callbackBudgetUsec"), callbackBudgetUsec);
    stats.insert(QStringLiteral("callbackCount"),
                 QVariant::fromValue<qulonglong>(AudioEngine::callbackCount()));
    stats.insert(QStringLiteral("callbackOverruns"),
                 QVariant::fromValue<qulonglong>(AudioEngine::callbackOverrunCount()));
    stats.insert(QStringLiteral("hardwareXruns"),
                 QVariant::fromValue<qulonglong>(m_audioDeviceService.hardwareXRunCount()));
    const auto scheduling = AudioEngine::realtimeThreadSchedulingStatus();
    stats.insert(QStringLiteral("realtimeScheduling"),
                 QString::fromLatin1(platform::audioThreadSchedulingStateName(
                     scheduling.state)));
    stats.insert(QStringLiteral("realtimeSchedulingPriority"), scheduling.priority);
    stats.insert(QStringLiteral("realtimeSchedulingError"), scheduling.nativeError);
    stats.insert(QStringLiteral("sampleRate"), snapshot.sampleRate);
    stats.insert(QStringLiteral("bufferSamples"), snapshot.bufferSamples);

    QVariantList fxProfiles;
    for (int i = 1; i <= static_cast<int>(EffectType::RollOut); ++i) {
        const auto type = static_cast<EffectType>(i);
        const auto profile = FxProcessor::getCpuProfile(type);
        if (profile.count == 0)
            continue;

        QVariantMap row;
        row.insert(QStringLiteral("name"), QString::fromLatin1(FxProcessor::effectTypeName(type)));
        row.insert(QStringLiteral("averageUsec"),
                   static_cast<double>(profile.totalUsec) / static_cast<double>(profile.count));
        row.insert(QStringLiteral("worstUsec"), static_cast<double>(profile.worstUsec));
        row.insert(QStringLiteral("count"), QVariant::fromValue<qulonglong>(profile.count));
        fxProfiles.push_back(row);
    }
    stats.insert(QStringLiteral("fxProfiles"), fxProfiles);
    return stats;
}
