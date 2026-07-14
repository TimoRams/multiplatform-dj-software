#include "DjEngineCommonIncludes.h"
#include "audio/device/AudioDeviceService.h"
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

DjEngine::DjEngine(AudioDeviceService& audioDeviceService, AudioPageCache& audioPageCache,
                   ControlClock& controlClock, engine::sync::SyncCoordinator& syncCoordinator,
                   int deckIndex, QObject* parent)
    : QObject(parent)
    , m_audioDeviceService(audioDeviceService)
    , m_audioPageCache(audioPageCache)
    , m_audioGraph(std::make_unique<DeckAudioGraph>(audioPageCache))
    , m_transport(std::make_unique<DeckTransport>(*m_audioGraph))
    , m_trackLoader(audioPageCache, static_cast<int>(WAVEFORM_POINTS_PER_SECOND))
    , m_syncCoordinator(syncCoordinator)
    , m_controlClock(controlClock)
    , m_deckIndex(deckIndex)
    , m_syncController(std::make_unique<engine::sync::DeckSyncController>(
          engine::sync::DeckSyncController::Configuration {deckIndex}))
{
    m_syncCoordinator.registerDeck(m_deckIndex, *m_syncController);

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
                                                    auto waveform, auto rgb) {
        AnalyzerResultMailbox::Chunk chunk;
        chunk.generation = generation;
        chunk.firstBin = firstBin;
        chunk.totalBins = totalBins;
        chunk.waveform = std::make_shared<const QVector<TrackData::WaveformBin>>(std::move(waveform));
        chunk.rgb = std::make_shared<const QVector<TrackData::RgbWaveformFrame>>(std::move(rgb));
        analysisMailbox->publishChunk(std::move(chunk));
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

    // Audio callback is registered by DjMasterBus, not per-deck.

    m_audioGraph->mixer().setTrim(static_cast<float>(m_trim));
    m_audioGraph->mixer().setFader(static_cast<float>(m_volume));

    // DjMasterBus prepares the registered DeckAudioGraph endpoint.

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
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return;

    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().setScratchTimbre(0.0f);

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
        for (const auto& chunk : m_analysisMailbox->takeChunks()) {
            if (chunk.generation == m_analyzer->generation()
                && chunk.waveform && chunk.rgb) {
                m_trackData->applyProgressiveWaveformChunk(
                    chunk.firstBin, chunk.totalBins, *chunk.waveform, *chunk.rgb);
            }
        }
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
    if (!m_audioGraph->mixerPtr())
        return;
    m_audioGraph->mixer().setEq(static_cast<float>(m_eqLow),
                       static_cast<float>(m_eqMid),
                       static_cast<float>(m_eqHigh));
}


void DjEngine::applyMixerFilter()
{
    if (!m_audioGraph->mixerPtr())
        return;
    m_audioGraph->mixer().setFilterVal(static_cast<float>(m_filter));
}


void DjEngine::notifyVuMetersIfNeeded()
{
    const float vuL    = vuLevelL();
    const float vuR    = vuLevelR();
    const float preL   = preFaderVuLevelL();
    const float preR   = preFaderVuLevelR();
    const float gr     = gainReduction();

    constexpr float kVuEps = 0.015f;
    constexpr float kGrEps = 0.012f;
    const bool vuMoved = std::abs(vuL - m_lastNotifiedVuL) > kVuEps
                      || std::abs(vuR - m_lastNotifiedVuR) > kVuEps
                      || std::abs(preL - m_lastNotifiedPreVuL) > kVuEps
                      || std::abs(preR - m_lastNotifiedPreVuR) > kVuEps;
    const bool grMoved = std::abs(gr - m_lastNotifiedGr) > kGrEps;
    const qint64 msSince = m_vuNotifyClock.isValid() ? m_vuNotifyClock.elapsed() : 1000;

    if (!vuMoved && !grMoved && msSince < 50)
        return;

    m_lastNotifiedVuL   = vuL;
    m_lastNotifiedVuR   = vuR;
    m_lastNotifiedPreVuL = preL;
    m_lastNotifiedPreVuR = preR;
    m_lastNotifiedGr    = gr;
    m_vuNotifyClock.restart();

    if (vuMoved || msSince >= 50)
        emit vuLevelChanged();
    if (grMoved || msSince >= 50)
        emit gainReductionChanged();
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
    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().setFader(static_cast<float>(m_volume));
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
    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().setTrim(static_cast<float>(m_trim));
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
    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().setPolarityInverted(inverted);
}

void DjEngine::setPolarityInverted(bool inverted)
{
    applyPolarityInverted(inverted);
    emit polarityInvertedChanged();
}
