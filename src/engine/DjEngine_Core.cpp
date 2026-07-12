#include "DjEngineCommonIncludes.h"
#include "SyncMaintenancePolicy.h"
#include "audio/device/AudioDeviceService.h"


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

DjEngine::DjEngine(AudioDeviceService& audioDeviceService, QObject* parent)
    : QObject(parent)
    , m_audioDeviceService(audioDeviceService)
{
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        s_syncDecks.push_back(this);
    }

    m_trackData = new TrackData(this);
    m_analyzer = std::make_unique<WaveformAnalyzer>(
        m_trackData,
        &formatManager,
        static_cast<int>(WAVEFORM_POINTS_PER_SECOND));
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
        bool propagateFromSelf = false;
        DjEngine* masterToFollow = nullptr;
        if (m_syncEnabled) {
            {
                std::lock_guard<std::mutex> g(s_syncMutex);
                updateSyncMasterLocked();
                propagateFromSelf = m_isSyncMaster;
                masterToFollow = s_syncMasterDeck;
            }
            if (propagateFromSelf)
                propagateMasterTempoLocked(this);
            else if (masterToFollow)
                propagateMasterTempoLocked(masterToFollow);
        }
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
    readAheadThread.startThread();

    // Audio callback is registered by DjMasterBus, not per-deck.

    scratchBridge = std::make_unique<engine::audio::ScratchDeckBridge>(&transportSource, false);
    scratchBridge->setAudioPlayheadSink(&m_atomicPlayheadPos);
    timeStretchSource = std::make_unique<TimeStretchAudioSource>(scratchBridge.get());
    
    // Create the mixer DSP source to apply EQ, Filter, and Gain based on Pioneer DJM A9.
    mixerSource = std::make_unique<MixerDspSource>(timeStretchSource.get());
    mixerSource->setTrim(static_cast<float>(m_trim));
    mixerSource->setFader(static_cast<float>(m_volume));

    // DjMasterBus calls prepareToPlay on this source via addDeck().

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
    m_snapClock.start();
    m_playHistoryClock.start();
    m_vuNotifyClock.start();
    m_progressNotifyClock.start();

    connect(&timer, &QTimer::timeout, this, &DjEngine::onTimer);
    timer.setTimerType(Qt::PreciseTimer);
    // Faster control snapshots reduce audible speed stepping while scratching.
    timer.start(4);
}


DjEngine::~DjEngine()
{
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        s_syncDecks.erase(std::remove(s_syncDecks.begin(), s_syncDecks.end(), this), s_syncDecks.end());
        s_syncEnableOrder.erase(std::remove(s_syncEnableOrder.begin(), s_syncEnableOrder.end(), this),
                                s_syncEnableOrder.end());
        if (s_syncMasterDeck == this)
            s_syncMasterDeck = nullptr;
        updateSyncMasterLocked();
    }

    if (m_analyzer) {
        m_analyzer->stopAnalysis();
        m_analyzer.reset();
    }
    releaseTransportReaders();
    readAheadThread.stopThread(1000);
}


void DjEngine::prepareForShutdown()
{
    // Disconnect only — QTimer::stop() during aboutToQuit has crashed on macOS when
    // a timeout handler is still unwinding on the stack.
    QObject::disconnect(&timer, nullptr, this, nullptr);

    if (m_analysisPersistTimer)
        QObject::disconnect(m_analysisPersistTimer, nullptr, this, nullptr);

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->stopAnalysis();
    }

    m_playRequested = false;
    transportSource.stop();
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

    const double bpm = m_trackData->getBpm();
    const QString key = m_trackData->getDetectedKey().trimmed();
    const auto beatGrid = m_trackData->getBeatGrid();

    if (bpm <= 0.0 && key.isEmpty() && beatGrid.empty())
        return;

    m_libraryDb->updateAnalysisData(
        m_currentTrackId,
        static_cast<float>(bpm),
        key,
        m_trackData->getFirstBeatSample(),
        m_trackData->getSampleRate(),
        beatGrid,
        m_trackData->getConfidenceInfo(),
        m_trackData->getBeatGridInfo());
}


void DjEngine::onTimer()
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        cancelQuantizedCueJump();
        tickScratchPhysics();
        return;
    }

    if (mixerSource)
        mixerSource->setScratchTimbre(0.0f);

    decayJogNudge();

    // Safety: transport must not run when there's no play intent and no cue preview.
    // Catches any leftover transport state that togglePlay/pause missed.
    if (transportSource.isPlaying() && !m_playRequested && !m_mainCuePreviewActive) {
        freezeTransportAt(transportSource.getCurrentPosition());
        notifyVuMetersIfNeeded();
        return;
    }

    // Safety: transport must not run during pre-roll countdown (visual position is
    // negative; transport is clamped at 0 and driven by wall-clock).
    if (transportSource.isPlaying() && m_preRollCountdownActive)
        transportSource.stop();

    if (transportSource.isPlaying()) {
        serviceQuantizedCueJump();
        if (!tickTransportPlaying())
            return;
        // Accumulate real audible playback time for play-count logging. Use wall
        // time instead of assuming the control timer fires exactly every 4 ms.
        if (!m_playLogged && !m_currentTrackId.isEmpty() && m_playRequested) {
            const double elapsedSec = m_playHistoryClock.isValid()
                ? static_cast<double>(m_playHistoryClock.restart()) / 1000.0
                : 0.0;
            m_playedAccumSec += std::clamp(elapsedSec, 0.0, 0.25);

            const double playheadSec = std::max(0.0, static_cast<double>(getVisualPosition()));
            const double nearEndSec = m_trackDurationSec > 0.0 ? m_trackDurationSec * 0.80 : 1e9;
            const double thresholdSec = playHistoryThresholdSeconds(m_trackDurationSec);
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
        tickTransportStopped();
    }

    if (engine::shouldRunFollowerSyncMaintenance(m_syncEnabled,
                                                 m_isSyncMaster,
                                                 m_scratch.scrubbing(),
                                                 m_scratch.releaseGlide())) {
        updatePhaseCorrection();
        updateTightDoubleAlignment();
    }

    updateFxBeatSyncPosition();
    notifyVuMetersIfNeeded();
}


bool DjEngine::tickTransportPlaying()
{
    // Store a fresh snapshot from the transport each control tick.
    // Interpolation happens only between these anchors.
    const double dtSec = m_snapValid
        ? std::clamp(static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
        : 0.004;

    const double measuredPos = transportSource.getCurrentPosition();
    m_snapPosition   = measuredPos;
    m_snapTempoRatio = getTempoRatio();

    // Slip shadow: advance continuously while loop/reverse diverts the transport.
    if (isSlipDiverted()) {
        const double dur = std::max(0.001, static_cast<double>(getDuration()));
        m_slipPosition = std::min(m_slipPosition + dtSec * getTempoRatio(), dur);
    } else {
        m_slipPosition = measuredPos;
    }

    if (m_loopActive && m_loopOutSec > m_loopInSec) {
        if (m_isReverse && m_snapPosition <= m_loopInSec) {
            transportSource.setPosition(m_loopOutSec);
            m_snapPosition = m_loopOutSec;
        } else if (!m_isReverse && m_loopInSec < 0.0
                   && m_snapPosition >= m_loopOutSec) {
            // Loop with pre-roll IN point: the audio source cannot enforce this
            // (no negative samples).  Software-wrap: stop transport and start a
            // pre-roll countdown from the true (negative) loop-in position.
            transportSource.stop();
            m_snapValid = false;
            m_scrubHoldPosition = m_loopInSec;
            m_preRollCountdownActive = true;
            m_preRollVisualStartPos  = m_loopInSec;
            m_preRollClock.restart();
            m_atomicPlayheadPos.store(m_loopInSec, std::memory_order_relaxed);
            emitPlaybackStateChanged();
            return false;
        }
    }

    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(m_snapPosition, std::memory_order_relaxed);
    notifyProgressIfNeeded();
    return true;
}


void DjEngine::tickTransportStopped()
{
    m_snapValid = false;
    if (m_preRollCountdownActive) {
        // Advance visual position from negative toward 0 at playback rate.
        const double elapsed   = static_cast<double>(m_preRollClock.nsecsElapsed()) * 1e-9;
        const double visualPos = m_preRollVisualStartPos + elapsed * getTempoRatio();
        m_scrubHoldPosition = visualPos;
        m_atomicPlayheadPos.store(visualPos, std::memory_order_relaxed);

        // Loop entirely in pre-roll: both endpoints negative, wrap back to loop IN.
        if (m_loopActive && m_loopOutSec <= 0.0 && m_loopOutSec > m_loopInSec
                && visualPos >= m_loopOutSec) {
            m_preRollVisualStartPos = m_loopInSec;
            m_scrubHoldPosition     = m_loopInSec;
            m_preRollClock.restart();
            m_atomicPlayheadPos.store(m_loopInSec, std::memory_order_relaxed);
            emit progressChanged();
            return;
        }

        if (visualPos >= 0.0) {
            m_preRollCountdownActive = false;
            m_scrubHoldPosition = 0.0;
            transportSource.setPosition(0.0);
            armSnapFromTransportPosition();
            transportSource.start();
            // For loops crossing 0: re-apply audio-source loop now that transport
            // is in-track.  applyLoopRangeToAudioSource() was a no-op while
            // loopIn was negative; with loopIn < 0 the audio loop [0, loopOut]
            // is software-enforced via the transport-playing branch above.
        }
        emit progressChanged();
    } else {
        ensureTransportRunningForPlayIntent();
        // In pre-roll the transport is clamped at 0, so syncing from transport
        // erases the negative visual position. Preserve m_scrubHoldPosition when negative.
        const double transportPos = transportSource.getCurrentPosition();
        const double atomicPos    = (m_scrubHoldPosition < 0.0)
            ? m_scrubHoldPosition
            : transportPos;
        m_atomicPlayheadPos.store(atomicPos, std::memory_order_relaxed);
    }
}



double DjEngine::getPreRollSeconds() const
{
    return PRE_ROLL_SECONDS;
}


void DjEngine::applyMixerEq()
{
    if (!mixerSource)
        return;
    mixerSource->setEq(static_cast<float>(m_eqLow),
                       static_cast<float>(m_eqMid),
                       static_cast<float>(m_eqHigh));
}


void DjEngine::applyMixerFilter()
{
    if (!mixerSource)
        return;
    mixerSource->setFilterVal(static_cast<float>(m_filter));
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
    if (mixerSource)
        mixerSource->setFader(static_cast<float>(m_volume));
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
    if (mixerSource)
        mixerSource->setTrim(static_cast<float>(m_trim));
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
    if (mixerSource)
        mixerSource->setPolarityInverted(inverted);
}

void DjEngine::setPolarityInverted(bool inverted)
{
    applyPolarityInverted(inverted);
    emit polarityInvertedChanged();
}
