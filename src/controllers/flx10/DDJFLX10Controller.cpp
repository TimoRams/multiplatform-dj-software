#include "DDJFLX10Controller.h"

#include "controllers/DeckIndex.h"
#include "deck/DjEngine.h"
#include "Flx10Protocol.h"
#include "domain/TrackData.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace flx10_protocol;

DDJFLX10Controller::DDJFLX10Controller(ControlClock& controlClock, QObject* parent)
    : QObject(parent)
{
    m_uploadTimer.setTimerType(Qt::PreciseTimer);
    m_keepAliveTimer.setTimerType(Qt::PreciseTimer);
    m_keepAliveTimer.setInterval(kKeepAliveIntervalMs);
    m_stateTimer.setTimerType(Qt::PreciseTimer);
    m_stateTimer.setInterval(kJogStateIntervalMs);
    m_tempoWaveformRefreshTimer.setSingleShot(true);
    m_tempoWaveformRefreshTimer.setTimerType(Qt::PreciseTimer);
    m_tempoWaveformRefreshTimer.setInterval(140);

    connect(&m_uploadTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendUploadChunk);
    connect(&m_keepAliveTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendKeepAlive);
    connect(&m_stateTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendStateTick);
    connect(&m_tempoWaveformRefreshTimer, &QTimer::timeout,
            this, &DDJFLX10Controller::flushTempoWaveformRefresh);
    ControlClock::Callbacks callbacks;
    callbacks.display = [this](const ControlTickContext&) {
        sendWaveformTick();
    };
    m_clockRegistration = controlClock.registerCallbacks(std::move(callbacks));

    setStatus(QStringLiteral("DDJ-FLX10 support disabled"));
}

DDJFLX10Controller::~DDJFLX10Controller()
{
    stop();
}

bool DDJFLX10Controller::start()
{
    stop();
    m_shuttingDown.store(false, std::memory_order_release);

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    setStatus(QStringLiteral("DDJ-FLX10: starting Linux HID activation"));

    if (!initialiseUsb())
        return false;

    sendVendorUnlock();

    const bool sequencerMidiReady = openSequencerMidiPort();
    if (!sequencerMidiReady)
        m_midiPort = findMidiPort();

    if (sequencerMidiReady || !m_midiPort.isEmpty())
        sendSessionSysEx();
    else
        qWarning() << "[DDJ-FLX10] MIDI port not found; SysEx gate skipped";

    if (!claimScreenInterface()) {
        stop();
        setStatus(QStringLiteral("DDJ-FLX10: could not claim HID interface 5 (run with udev permission or root)"));
        return false;
    }

    startHidWriter();
    startWaveformWorker();
    setConnected(true);
    m_clockStartMs = QDateTime::currentMSecsSinceEpoch();
    m_hidTrafficClock.restart();
    m_lastXx27SentMs.fill(-kJogStateIntervalMs);
    m_lastXx36SentMs = -kXx36TrickleIntervalMs;
    for (int deck = controllers::kFlx10FirstDeckIndex;
         deck <= controllers::kFlx10LastDeckIndex; ++deck) {
        m_uploadActive[deck] = false;
        m_uploadWindowsSent[deck] = 0;
        resetDisplayPacketState(deck);

        DjEngine* engine = deckEngine(deck);
        const QString trackPath = engine && engine->hasTrack()
            ? engine->trackFilePath() : QString {};
        if (trackPath != m_waveformTrackPaths[deck])
            invalidateDeckSnapshot(deck, trackPath, false);

        if (!m_waveforms[deck].isEmpty()) {
            m_waveformDurations[deck] = deckDisplayDuration(deck);
            uploadDeck(deck);
        } else {
            refreshDeckFromEngine(deck);
        }
    }
    // xx27 owns the time/playhead and spinning marker. It deliberately has a
    // dedicated 200 Hz clock instead of sharing the 60 Hz UI display callback;
    // writePacket() coalesces these updates per deck if USB is temporarily busy.
    sendStateTick();
    m_stateTimer.start();
    m_keepAliveEnabled = sequencerMidiReady || !m_midiPort.isEmpty();
    if (m_keepAliveEnabled) {
        sendKeepAlive();
        m_keepAliveTimer.start();
    }

    setStatus(QStringLiteral("DDJ-FLX10: HID display active"));
    return true;
#else
    setConnected(false);
#if defined(Q_OS_LINUX)
    setStatus(QStringLiteral("DDJ-FLX10: libusb not available at build time"));
#else
    setStatus(QStringLiteral("DDJ-FLX10: HID activation is implemented for Linux only"));
#endif
    return false;
#endif
}

void DDJFLX10Controller::prepareForShutdown() noexcept
{
    m_shuttingDown.store(true, std::memory_order_release);
    disconnectDeckSignals();
    m_uploadTimer.stop();
    m_keepAliveTimer.stop();
    m_stateTimer.stop();
    m_tempoWaveformRefreshTimer.stop();
    m_pendingTempoWaveformDeckMask = 0;
    m_keepAliveEnabled = false;
    stopWaveformWorker();
#if defined(Q_OS_LINUX)
    if (m_keepAliveProcess && m_keepAliveProcess->state() != QProcess::NotRunning)
        m_keepAliveProcess->kill();
#endif
    QCoreApplication::removePostedEvents(this);
    m_deckA = nullptr;
    m_deckB = nullptr;
}

void DDJFLX10Controller::stop()
{
    m_shuttingDown.store(true, std::memory_order_release);
    disconnectDeckSignals();
    m_uploadTimer.stop();
    m_keepAliveTimer.stop();
    m_stateTimer.stop();
    m_tempoWaveformRefreshTimer.stop();
    m_pendingTempoWaveformDeckMask = 0;
    m_keepAliveEnabled = false;
#if defined(Q_OS_LINUX)
    if (m_keepAliveProcess && m_keepAliveProcess->state() != QProcess::NotRunning) {
        m_keepAliveProcess->kill();
        m_keepAliveProcess->waitForFinished(100);
    }
    m_keepAliveProcess.reset();
#endif
    m_uploadActive.fill(false);
    m_uploadWindowsSent.fill(0);
    m_uploadStartWindows.fill(0);
    m_uploadResweepPending.fill(false);
    // Hand the device back in a defined state. Closing the endpoint without
    // this leaves the last pushed waveform, cover art and platter position
    // frozen on the jog screens until something else drives them, which looks
    // like the controller is still running a track that is long gone.
    if (m_connected) {
        for (int deck = controllers::kFlx10FirstDeckIndex;
             deck <= controllers::kFlx10LastDeckIndex; ++deck) {
            clearDeckDisplay(deck);
            resetDisplayPacketState(deck);
        }
    }
    for (int deck = controllers::kFlx10FirstDeckIndex; deck <= controllers::kFlx10LastDeckIndex; ++deck) {
        if (m_jogRingWarningActive[deck] || !m_jogRingLit[deck])
            sendJogRingIllumination(deck, true);
    }
    m_jogRingWarningActive.fill(false);
    m_jogRingLit.fill(true);

    stopWaveformWorker();

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    stopHidWriter();

    if (m_handle && m_interfaceClaimed) {
        libusb_release_interface(m_handle, kScreenInterface);
        m_interfaceClaimed = false;
    }

    if (m_handle) {
        libusb_close(m_handle);
        m_handle = nullptr;
    }

    if (m_usbContext) {
        libusb_exit(m_usbContext);
        m_usbContext = nullptr;
    }

    m_outEndpoint = 0;
#endif

    m_midiPort.clear();
#if defined(Q_OS_LINUX)
    m_sequencerMidiOut.reset();
#endif

    setConnected(false);
    if (!m_status.contains(QStringLiteral("disabled")))
        setStatus(QStringLiteral("DDJ-FLX10 support disabled"));
}

void DDJFLX10Controller::setDecks(DjEngine* deckA, DjEngine* deckB)
{
    disconnectDeckSignals();
    m_deckA = deckA;
    m_deckB = deckB;
    if (m_connected && !m_shuttingDown.load(std::memory_order_acquire))
        connectDeckSignals();
}

void DDJFLX10Controller::disconnectDeckSignals()
{
    for (auto& connection : m_trackLoadedConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_trackEjectedConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_rgbWaveformConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_overviewWaveformConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_dataClearedConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_metadataConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_keyAnalyzedConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_beatgridConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_hotCueConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_tempoConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_tempoRangeConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_scrubbingConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_progressConnections)
        QObject::disconnect(connection);

    m_trackLoadedConnections.fill({});
    m_trackEjectedConnections.fill({});
    m_rgbWaveformConnections.fill({});
    m_overviewWaveformConnections.fill({});
    m_dataClearedConnections.fill({});
    m_metadataConnections.fill({});
    m_keyAnalyzedConnections.fill({});
    m_beatgridConnections.fill({});
    m_hotCueConnections.fill({});
    m_tempoConnections.fill({});
    m_tempoRangeConnections.fill({});
    m_scrubbingConnections.fill({});
    m_progressConnections.fill({});
}

DjEngine* DDJFLX10Controller::deckEngine(int deck) const
{
    if (deck == 1)
        return m_deckA;
    if (deck == 2)
        return m_deckB;
    return nullptr;
}

void DDJFLX10Controller::connectDeckSignals()
{
    disconnectDeckSignals();

    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    for (int deck = 1; deck <= 2; ++deck) {
        DjEngine* engine = deckEngine(deck);
        if (!engine)
            continue;
        m_cachedDeckKeyBytes[deck] = deckKeyByte(deck);

        m_trackLoadedConnections[deck] = connect(engine, &DjEngine::trackLoaded, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            if (DjEngine* eng = deckEngine(deck); !eng || !eng->hasTrack())
                return;
            refreshDeckFromEngine(deck);
        });
        m_trackEjectedConnections[deck] = connect(engine, &DjEngine::trackEjected, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            resetDeckWaveformOutput(deck);
        });
        m_metadataConnections[deck] = connect(engine, &DjEngine::trackMetadataChanged, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            if (!m_connected)
                return;

            DjEngine* engine = deckEngine(deck);
            if (!engine)
                return;
            m_cachedDeckKeyBytes[deck] = deckKeyByte(deck);

            // Cover art does not depend on the waveform, but this handler used
            // to bail out when the deck had no FLX10 preview yet. Metadata
            // arrives at load time, long before the preview worker has produced
            // anything, so the one notification that carries a fresh picture was
            // always skipped. Marking the URL as done before the upload had
            // actually produced bytes then prevented any later retry.
            const QString coverUrl = engine->coverArtUrl();
            if (coverUrl != m_lastCoverUrls[deck]) {
                if (coverUrl.isEmpty()) {
                    m_lastCoverUrls[deck].clear();
                } else if (const QByteArray jpeg = generateCoverJpeg(deck);
                           !jpeg.isEmpty() && sendXx33Album(deck, jpeg)) {
                    qInfo() << "[DDJ-FLX10] Deck" << deck
                            << "uploading cover art bytes" << jpeg.size();
                    m_lastCoverUrls[deck] = coverUrl;
                }
            }

            // Pad labels carry track text, which is only meaningful once the
            // deck actually has a waveform on screen.
            if (!m_waveforms[deck].isEmpty())
                sendXx39(deck);
        });
        // xx27 changes the FLX10's waveform time scale. The firmware can drop
        // the not-yet-played side of its xx36 buffer while that scale moves, so
        // refresh the waveform once the physical tempo fader has settled.
        m_tempoConnections[deck] = connect(
            engine, &DjEngine::tempoChanged, this,
            [this, deck] { scheduleTempoWaveformRefresh(deck); });
        m_tempoRangeConnections[deck] = connect(
            engine, &DjEngine::tempoRangeChanged, this,
            [this, deck] { scheduleTempoWaveformRefresh(deck); });

        if (TrackData* trackData = engine->getTrackData()) {
            m_rgbWaveformConnections[deck] = connect(trackData, &TrackData::rgbWaveformUpdated, this, [this, deck] {
                if (m_shuttingDown.load(std::memory_order_acquire))
                    return;
                if (DjEngine* eng = deckEngine(deck); !eng || !eng->hasTrack())
                    return;
                refreshDeckFromEngine(deck);
            });
            m_overviewWaveformConnections[deck] = connect(trackData, &TrackData::overviewRgbUpdated, this, [this, deck] {
                if (m_shuttingDown.load(std::memory_order_acquire))
                    return;
                if (DjEngine* eng = deckEngine(deck); !eng || !eng->hasTrack())
                    return;
                refreshDeckFromEngine(deck);
            });
            m_dataClearedConnections[deck] = connect(trackData, &TrackData::dataCleared, this, [this, deck] {
                if (m_shuttingDown.load(std::memory_order_acquire))
                    return;
                if (DjEngine* eng = deckEngine(deck); !eng || !eng->hasTrack())
                    return;
                // Analysis clears full-res RGB but keeps the instant overview — do not
                // wipe the controller display while a preview waveform is still available.
                if (DjEngine* engine = deckEngine(deck)) {
                    if (TrackData* td = engine->getTrackData()) {
                        if (!td->getOverviewRgbData().isEmpty()
                            || !td->getProgressiveOvrData().isEmpty()) {
                            refreshDeckFromEngine(deck);
                            return;
                        }
                    }
                }

                resetDeckWaveformOutput(deck);
            });
            m_keyAnalyzedConnections[deck] = connect(trackData, &TrackData::keyAnalyzed, this, [this, deck] {
                if (m_shuttingDown.load(std::memory_order_acquire))
                    return;
                m_cachedDeckKeyBytes[deck] = deckKeyByte(deck);
                if (!m_connected || m_waveforms[deck].isEmpty())
                    return;
                sendXx39(deck);
            });
            m_beatgridConnections[deck] = connect(trackData, &TrackData::beatgridChanged, this, [this, deck] {
                if (m_shuttingDown.load(std::memory_order_acquire))
                    return;
                if (!m_connected || m_waveforms[deck].isEmpty() || m_uploadActive[deck])
                    return;
                sendXx2f(deck);
            });
        }

        m_hotCueConnections[deck] = connect(engine, &DjEngine::hotCuesChanged, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            if (!m_connected || m_waveforms[deck].isEmpty())
                return;
            sendXx39(deck);
        });
        m_scrubbingConnections[deck] = connect(engine, &DjEngine::scrubbingChanged, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            resetDisplayPacketState(deck);
            pushDeckJogDisplay(deck);
        });
        m_progressConnections[deck] = connect(engine, &DjEngine::progressChanged, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
                return;
            const DjEngine* eng = deckEngine(deck);
            if (!eng || !eng->isScratchVisualActive())
                return;
            pushDeckJogDisplay(deck);
        });
    }
}

void DDJFLX10Controller::resetDeckWaveformOutput(int deck)
{
    if (deck < 1 || deck > 2)
        return;

    invalidateDeckSnapshot(deck, {}, false);
    m_jogRingWarningActive[deck] = false;
    sendJogRingIllumination(deck, true);
    qInfo() << "[DDJ-FLX10] Deck" << deck << "has no track waveform; HID deck output stopped";
    if (m_connected)
        clearDeckDisplay(deck);
}

void DDJFLX10Controller::invalidateDeckSnapshot(int deck, const QString& trackPath,
                                                bool clearDevice)
{
    if (deck < 1 || deck > 2)
        return;

    m_waveforms[deck].clear();
    m_waveformTrackPaths[deck] = trackPath;
    m_waveformDurations[deck] = kPreviewDurationSeconds;
    m_uploadActive[deck] = false;
    m_uploadWindowsSent[deck] = 0;
    m_uploadStartWindows[deck] = 0;
    m_uploadResweepPending[deck] = false;
    m_pendingTempoWaveformDeckMask &= static_cast<std::uint8_t>(~(1u << deck));
    m_lastWaveformRefreshMs[deck] = 0;
    m_lastWaveformUploadMs[deck] = 0;
    m_waveformUploadTrackGenerations[deck] = 0;
    m_waveformUploadQualityPermille[deck] = 0;
    m_cachedDeckKeyBytes[deck] = 0x80;
    m_lastCoverUrls[deck].clear();
    resetDisplayPacketState(deck);

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    discardQueuedDeckPackets(deck);
#endif
    if (clearDevice && m_connected)
        clearDeckDisplay(deck);
}

void DDJFLX10Controller::startWaveformWorker()
{
    stopWaveformWorker();
    m_waveformWorkerStopping = false;
    m_waveformWorker = std::thread([this] { waveformWorkerLoop(); });
}

void DDJFLX10Controller::stopWaveformWorker() noexcept
{
    {
        std::lock_guard lock(m_waveformJobMutex);
        m_waveformWorkerStopping = true;
        for (auto& job : m_pendingWaveformJobs)
            job.reset();
    }
    m_waveformJobCondition.notify_all();
    if (m_waveformWorker.joinable())
        m_waveformWorker.join();
}

void DDJFLX10Controller::waveformWorkerLoop()
{
    for (;;) {
        PendingWaveformJob job;
        {
            std::unique_lock lock(m_waveformJobMutex);
            m_waveformJobCondition.wait(lock, [this] {
                if (m_waveformWorkerStopping)
                    return true;
                for (const auto& pending : m_pendingWaveformJobs) {
                    if (pending)
                        return true;
                }
                return false;
            });
            if (m_waveformWorkerStopping)
                return;
            for (auto& pending : m_pendingWaveformJobs) {
                if (pending) {
                    job = std::move(*pending);
                    pending.reset();
                    break;
                }
            }
        }
        if (!job.snapshot || job.targetEntries <= 0)
            continue;

        // The expensive part, now off the owner thread entirely.
        QByteArray out;
        out.reserve(job.targetEntries * 2);
        std::uint32_t columnsWithData = 0;
        std::uint32_t completeColumns = 0;
        for (int i = 0; i < job.targetEntries; ++i) {
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            const auto range = waveform::sourceLineRangeForColumn(
                job.snapshot->totalLineCount, i, job.targetEntries);
            const auto column = waveform::aggregateWaveformColumn(
                *job.snapshot, range);
            if (!column.hasData) {
                out += encodePwv5Entry(1, 0, 0, 0);
                continue;
            }
            ++columnsWithData;
            if (column.complete)
                ++completeColumns;
            out += encodePwv5Column(column);
        }

        const int deck = job.deck;
        const auto generation = job.trackGeneration;
        const auto generated = static_cast<std::uint32_t>(job.targetEntries);
        QMetaObject::invokeMethod(
            this,
            [this, deck, out, generation, generated, completeColumns] {
                onPreviewWaveformReady(deck, out, generation, generated,
                                       completeColumns);
            },
            Qt::QueuedConnection);
    }
}

void DDJFLX10Controller::refreshDeckFromEngine(int deck)
{
    if (m_shuttingDown.load(std::memory_order_acquire))
        return;

    DjEngine* engine = deckEngine(deck);
    if (!engine || !engine->hasTrack())
        return;

    const QString trackPath = engine->trackFilePath();
    if (trackPath != m_waveformTrackPaths[deck])
        invalidateDeckSnapshot(deck, trackPath, true);
    m_cachedDeckKeyBytes[deck] = deckKeyByte(deck);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastWaveformRefreshMs[deck] > 0 && now - m_lastWaveformRefreshMs[deck] < 250)
        return;
    m_lastWaveformRefreshMs[deck] = now;
    requestPreviewWaveform(deck);
}

void DDJFLX10Controller::scheduleTempoWaveformRefresh(int deck)
{
    if (deck < 1 || deck > 2 || m_shuttingDown.load(std::memory_order_acquire)
        || !m_connected || m_waveforms[deck].isEmpty()) {
        return;
    }

    m_pendingTempoWaveformDeckMask |= static_cast<std::uint8_t>(1u << deck);
    // Tempo faders publish many 14-bit values while moving. Restarting here
    // coalesces that gesture into one repair instead of continuously rewinding
    // the same waveform transfer.
    m_tempoWaveformRefreshTimer.start();
}

void DDJFLX10Controller::flushTempoWaveformRefresh()
{
    const std::uint8_t pending = std::exchange(m_pendingTempoWaveformDeckMask, 0);
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    for (int deck = 1; deck <= 2; ++deck) {
        if ((pending & static_cast<std::uint8_t>(1u << deck)) == 0
            || m_waveforms[deck].isEmpty()) {
            continue;
        }

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
        // Drop old-scale xx36 packets that have not reached the device yet.
        // The priority xx27 slot is republished immediately below.
        discardQueuedDeckPackets(deck);
#endif
        m_uploadResweepPending[deck] = false;
        beginWaveformSweep(deck);
        resetDisplayPacketState(deck);
        pushDeckJogDisplay(deck);
        if (m_uploadActive[deck] && !m_uploadTimer.isActive())
            m_uploadTimer.start(kUploadTickIntervalMs);
    }
}

void DDJFLX10Controller::requestPreviewWaveform(int deck)
{
    DjEngine* engine = deckEngine(deck);
    TrackData* trackData = engine ? engine->getTrackData() : nullptr;
    if (!trackData)
        return;
    // Cheap on the owner thread: a snapshot is one shared_ptr copy.
    auto snapshot = trackData->getWaveformLineStoreSnapshot();
    if (!snapshot || snapshot->trackGeneration == 0
        || snapshot->totalLineCount == 0 || !snapshot->chunks) {
        return;
    }
    const int targetEntries = std::clamp(
        static_cast<int>(std::ceil(deckDisplayDuration(deck)
                                   * kJogWaveformEntriesPerSecond)),
        150, kMaxWaveformEntries);

    PendingWaveformJob job;
    job.deck = deck;
    job.snapshot = std::move(snapshot);
    job.targetEntries = targetEntries;
    job.trackGeneration = job.snapshot->trackGeneration;
    {
        std::lock_guard lock(m_waveformJobMutex);
        m_pendingWaveformJobs[deck] = std::move(job);
    }
    m_waveformJobCondition.notify_one();
}

void DDJFLX10Controller::onPreviewWaveformReady(
    int deck, const QByteArray& incoming, std::uint64_t trackGeneration,
    std::uint32_t generatedColumns, std::uint32_t completeColumns)
{
    if (m_shuttingDown.load(std::memory_order_acquire) || incoming.isEmpty())
        return;
    QByteArray waveform = incoming;
    WaveformPreviewRenderInfo renderInfo;
    renderInfo.trackGeneration = trackGeneration;
    renderInfo.generatedColumns = generatedColumns;
    renderInfo.completeColumns = completeColumns;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const std::uint32_t qualityPermille = renderInfo.generatedColumns == 0
        ? 0
        : static_cast<std::uint32_t>(
              (static_cast<std::uint64_t>(renderInfo.completeColumns) * 1000ULL)
              / static_cast<std::uint64_t>(renderInfo.generatedColumns));
    const bool trackChanged = m_waveformUploadTrackGenerations[deck]
        != renderInfo.trackGeneration;
    const bool firstUploadForTrack = m_waveforms[deck].isEmpty() || trackChanged;
    const std::uint32_t previousQuality = m_waveformUploadQualityPermille[deck];
    const bool qualityImprovedEnough = qualityPermille
        >= previousQuality + 25;
    const bool reachedComplete = qualityPermille == 1000
        && previousQuality < 1000;
    const bool staleUpload = m_lastWaveformUploadMs[deck] == 0
        || now - m_lastWaveformUploadMs[deck] > 3500;
    if (!firstUploadForTrack
        && !qualityImprovedEnough
        && !reachedComplete
        && !staleUpload) {
        return;
    }

    // A quality refresh for the track that is already streaming must not
    // restart the transfer. The log showed eleven full re-uploads of the same
    // 63487-entry waveform (quality 0 -> 26 -> 106 -> ... -> 393 permille);
    // each one reset the window cursor to zero, so a 3342-window transfer that
    // needs ~33 s never got anywhere near finishing before the next analysis
    // chunk restarted it. Windows that have not been sent yet automatically
    // pick up the improved samples, because they are read out of
    // m_waveforms[deck] at send time.
    const bool sameTrackRefresh = !firstUploadForTrack
        && m_waveforms[deck].size() == waveform.size();

    m_waveforms[deck] = std::move(waveform);
    m_waveformDurations[deck] = deckDisplayDuration(deck);
    m_waveformUploadTrackGenerations[deck] = renderInfo.trackGeneration;
    m_waveformUploadQualityPermille[deck] = std::max(
        previousQuality, qualityPermille);
    m_lastWaveformUploadMs[deck] = now;
    resetDisplayPacketState(deck);

    if (!m_connected)
        return;

    // A sweep of a waveform that carries nothing yet costs the full transfer
    // (a five-minute track is ~3300 windows) and puts an empty picture on the
    // screen, while saturating the endpoint that also carries the platter
    // position. Hold the sweep back until the analysis has produced something
    // — but still send the init, otherwise the screen sits on "not loaded"
    // until the analysis finishes.
    const bool waveformHasContent = qualityPermille > 0;
    if (!waveformHasContent && !m_uploadActive[deck]) {
        qInfo() << "[DDJ-FLX10] Deck" << deck
                << "waveform has no analysed columns yet; sending init only";
        uploadDeck(deck, false);
        return;
    }

    if (sameTrackRefresh) {
        // Same track, better data: never re-send the init sequence
        // (xx30/xx39/cover art/xx35) and never rewind an in-flight transfer.
        if (m_uploadActive[deck]) {
            // Windows already on the wire carry whatever the waveform held when
            // they were sent. A transfer that starts while analysis is still
            // running therefore leaves permanently blank or coarse regions on
            // the screen, which is the "no waveform / stops loading" case.
            // Repeat the sweep once this one reaches the end; each repetition
            // reads the current, better samples, so it converges on the
            // finished analysis instead of freezing an early snapshot.
            m_uploadResweepPending[deck] = true;
        } else {
            beginWaveformSweep(deck);
            if (!m_uploadTimer.isActive())
                m_uploadTimer.start(kUploadTickIntervalMs);
        }
        return;
    }

    qInfo() << "[DDJ-FLX10] Deck" << deck
            << "uploading waveform entries" << (m_waveforms[deck].size() / 2)
            << "quality" << qualityPermille << "permille"
            << "source chunk" << renderInfo.playheadChunkIndex
            << "state" << renderInfo.playheadChunkState;
    uploadDeck(deck);
}

void DDJFLX10Controller::setStatus(const QString& status)
{
    if (m_status == status)
        return;

    m_status = status;
    qInfo() << "[DDJ-FLX10]" << m_status;
    emit statusChanged();
}

void DDJFLX10Controller::setConnected(bool connected)
{
    if (m_connected == connected)
        return;

    m_connected = connected;
    if (m_connected && !m_shuttingDown.load(std::memory_order_acquire))
        connectDeckSignals();
    else
        disconnectDeckSignals();
    emit connectedChanged();
}


QString DDJFLX10Controller::findMidiPort() const
{
    QProcess process;
    process.start(QStringLiteral("amidi"), {QStringLiteral("-l")});
    if (!process.waitForFinished(1000))
        return {};

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QRegularExpression portPattern(QStringLiteral("(hw:[0-9,]+)"));
    QString fallbackPort;
    for (const QString& line : output.split('\n')) {
        const QString lower = line.toLower();
        if (!lower.contains(QStringLiteral("flx10"))
            && !lower.contains(QStringLiteral("ddj-flx10"))
            && !lower.contains(QStringLiteral("ddj"))) {
            continue;
        }

        const auto match = portPattern.match(line);
        if (!match.hasMatch())
            continue;

        const QString port = match.captured(1);
        if (fallbackPort.isEmpty())
            fallbackPort = port;

        const QString direction = line.left(line.indexOf(port)).trimmed().toUpper();
        if (direction.contains(QLatin1Char('O')))
            return port;
    }
    return fallbackPort;
}

bool DDJFLX10Controller::openSequencerMidiPort()
{
#if defined(Q_OS_LINUX)
    m_sequencerMidiOut.reset();

    snd_seq_t* seq = nullptr;
    const int openResult = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (openResult < 0) {
        qWarning() << "[DDJ-FLX10] ALSA sequencer unavailable for SysEx:"
                   << QString::fromUtf8(snd_strerror(openResult));
        return false;
    }

    snd_seq_client_info_t* clientInfo = nullptr;
    snd_seq_port_info_t* portInfo = nullptr;
    snd_seq_client_info_alloca(&clientInfo);
    snd_seq_port_info_alloca(&portInfo);

    QStringList candidates;
    snd_seq_client_info_set_client(clientInfo, -1);
    while (snd_seq_query_next_client(seq, clientInfo) >= 0) {
        const int client = snd_seq_client_info_get_client(clientInfo);
        const QString clientName = QString::fromUtf8(snd_seq_client_info_get_name(clientInfo)).trimmed();
        const QString clientKey = clientName.toLower();
        const bool clientLooksRelevant = clientKey.contains(QStringLiteral("flx10"))
            || clientKey.contains(QStringLiteral("ddj"))
            || clientKey.contains(QStringLiteral("pioneer"));

        snd_seq_port_info_set_client(portInfo, client);
        snd_seq_port_info_set_port(portInfo, -1);
        while (snd_seq_query_next_port(seq, portInfo) >= 0) {
            const int port = snd_seq_port_info_get_port(portInfo);
            const QString portName = QString::fromUtf8(snd_seq_port_info_get_name(portInfo)).trimmed();
            const QString portKey = portName.toLower();
            const bool portLooksRelevant = portKey.contains(QStringLiteral("flx10"))
                || portKey.contains(QStringLiteral("ddj"))
                || portKey.contains(QStringLiteral("pioneer"));
            const unsigned int caps = snd_seq_port_info_get_capability(portInfo);
            const bool writable = (caps & SND_SEQ_PORT_CAP_WRITE) != 0
                && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE) != 0;
            if (!writable || (!clientLooksRelevant && !portLooksRelevant))
                continue;

            candidates.push_back(QStringLiteral("alsa-out:%1:%2").arg(client).arg(port));
        }
    }
    snd_seq_close(seq);

    for (const QString& candidate : candidates) {
        auto output = std::make_unique<AlsaMidiOutput>();
        QString errorMessage;
        if (output->open(candidate, &errorMessage)) {
            m_midiPort = candidate;
            m_sequencerMidiOut = std::move(output);
            qInfo() << "[DDJ-FLX10] using ALSA sequencer MIDI port for HID SysEx:" << candidate;
            return true;
        }

        qWarning() << "[DDJ-FLX10] ALSA sequencer SysEx candidate failed:"
                   << candidate << errorMessage;
    }
#endif
    return false;
}

bool DDJFLX10Controller::sendMidiHex(const QString& hex) const
{
#if defined(Q_OS_LINUX)
    if (m_sequencerMidiOut && m_sequencerMidiOut->isOpen()) {
        QString errorMessage;
        const QByteArray bytes = bytesFromHexString(hex);
        const bool ok = m_sequencerMidiOut->sendMessage(bytes, &errorMessage);
        if (!ok)
            qWarning() << "[DDJ-FLX10] ALSA sequencer MIDI send failed:" << errorMessage;
        return ok;
    }
#endif

    if (m_midiPort.isEmpty())
        return false;

    QProcess process;
    process.start(QStringLiteral("amidi"), {QStringLiteral("-p"), m_midiPort, QStringLiteral("-S"), hex});
    if (!process.waitForFinished(1500))
        return false;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool DDJFLX10Controller::sendJogRingIllumination(int deck, bool on)
{
    if (deck < 0 || deck >= static_cast<int>(m_jogRingLit.size()))
        return false;

    if (m_midiPort.isEmpty()
#if defined(Q_OS_LINUX)
        && !(m_sequencerMidiOut && m_sequencerMidiOut->isOpen())
#endif
        )
        return false;

    const int controller = 0x08 + std::clamp(deck, 1, 4);
    const QString hex = QStringLiteral("BF %1 %2")
                            .arg(controller, 2, 16, QLatin1Char('0'))
                            .arg(on ? kJogRingOnValue : 0, 2, 16, QLatin1Char('0'))
                            .toUpper();
    const bool ok = sendMidiHex(hex);
    if (ok)
        m_jogRingLit[deck] = on;
    return ok;
}

void DDJFLX10Controller::updateJogRingWarning(int deck, double elapsedSeconds, double durationSeconds, bool playing)
{
    const double remaining = durationSeconds - elapsedSeconds;
    const bool shouldBlink = playing && durationSeconds > kJogRingWarningSeconds && remaining > 0.0 && remaining <= kJogRingWarningSeconds;

    if (!shouldBlink) {
        if (m_jogRingWarningActive[deck] || !m_jogRingLit[deck])
            sendJogRingIllumination(deck, true);
        m_jogRingWarningActive[deck] = false;
        return;
    }

    if (!m_jogRingWarningActive[deck])
        qInfo() << "[DDJ-FLX10] Deck" << deck << "jog ring end warning active; remaining seconds" << remaining;
    m_jogRingWarningActive[deck] = true;
    const bool blinkOn = ((QDateTime::currentMSecsSinceEpoch() / kJogRingBlinkIntervalMs) % 2) == 0;
    if (blinkOn != m_jogRingLit[deck])
        sendJogRingIllumination(deck, blinkOn);
}

void DDJFLX10Controller::sendSessionSysEx()
{
    sendMidiHex(QString::fromLatin1(kSessionStart));
    sendMidiHex(QString::fromLatin1(kEnterHid));
    for (int deck = 1; deck <= 4; ++deck)
        sendMidiHex(QString::fromLatin1(kDeckInit[deck]));
    sendMidiHex(QString::fromLatin1(kGlobalB));
    sendMidiHex(QString::fromLatin1(kGlobalC));
}

void DDJFLX10Controller::sendKeepAlive()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected
        || !m_keepAliveEnabled)
        return;
#if defined(Q_OS_LINUX)
    if (m_sequencerMidiOut && m_sequencerMidiOut->isOpen()) {
        sendMidiHex(QString::fromLatin1(kKeepAlive));
        return;
    }

    // The raw-MIDI fallback used to run amidi synchronously from the shared
    // control clock every 500 ms. A slow USB response then stalled the whole UI.
    if (m_midiPort.isEmpty())
        return;
    if (!m_keepAliveProcess)
        m_keepAliveProcess = std::make_unique<QProcess>(this);
    if (m_keepAliveProcess->state() == QProcess::NotRunning) {
        m_keepAliveProcess->start(
            QStringLiteral("amidi"),
            {QStringLiteral("-p"), m_midiPort, QStringLiteral("-S"),
             QString::fromLatin1(kKeepAlive)});
    }
#else
    sendMidiHex(QString::fromLatin1(kKeepAlive));
#endif
}

void DDJFLX10Controller::sendStateTick()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    reportHidWriteFailure();
    if (!m_hidWriteHealthy.load(std::memory_order_acquire))
        return;
    if (m_hidStateRefreshPending.exchange(false, std::memory_order_acq_rel)) {
        for (int deck = 1; deck <= 2; ++deck)
            m_lastXx27Packet[deck].clear();
    }
#endif

    for (int deck = 1; deck <= 2; ++deck)
        pushDeckJogDisplay(deck);
}

void DDJFLX10Controller::sendWaveformTick()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    const qint64 nowMs = m_hidTrafficClock.isValid() ? m_hidTrafficClock.elapsed() : 0;
    if (nowMs - m_lastXx36SentMs < kXx36TrickleIntervalMs)
        return;

    // Send at most one playhead window per tick and only while the deck is
    // moving, otherwise endpoint pressure can starve state/jog updates.
    for (int offset = 0; offset < 2; ++offset) {
        const int deck = 1 + ((m_nextWaveformDeck - 1 + offset) % 2);
        if (m_waveforms[deck].isEmpty())
            continue;
        const DjEngine* engine = deckEngine(deck);
        if (!engine || (!engine->isPlaying() && !engine->isScratchVisualActive()))
            continue;
        if (sendXx36Window(deck, m_waveforms[deck], currentWaveformEntry(deck))) {
            m_lastXx36SentMs = nowMs;
            m_nextWaveformDeck = deck == 1 ? 2 : 1;
            break;
        }
    }
}

void DDJFLX10Controller::sendUploadChunk()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected) {
        m_uploadActive.fill(false);
        return;
    }

    bool anyActive = m_uploadActive[1] || m_uploadActive[2];
    if (!anyActive) {
        m_uploadTimer.stop();
        return;
    }

    // The transfer used to be skipped outright while a deck was playing or
    // being scratched, on the theory that the endpoint should stay clear for
    // the platter. In practice a deck spends almost all of its time playing,
    // so the sweep never advanced and the jog screen simply never filled in —
    // which is the "still no waveform" report. The platter state travels in
    // its own priority slot ahead of this queue, so it cannot be starved by
    // the sweep; throttle the burst during playback instead of stopping it.
    for (int offset = 0; offset < 2; ++offset) {
        const int deck = 1 + ((m_nextUploadDeck - 1 + offset) % 2);
        if (!m_uploadActive[deck] || m_waveforms[deck].isEmpty())
            continue;
        const DjEngine* deckEngineForRate = deckEngine(deck);
        const bool deckIsBusy = deckEngineForRate
            && (deckEngineForRate->isPlaying()
                || deckEngineForRate->isScratchVisualActive());
        const int windowsThisTick = deckIsBusy ? 1 : kUploadWindowsPerTick;
        const int entries = m_waveforms[deck].size() / 2;
        const int totalWindows = (entries + kXx36EntriesPerWindow - 1)
            / kXx36EntriesPerWindow;
        // Fill outward from the playhead rather than from entry 0. The xx36
        // position counter lets the firmware accept out-of-order windows, and
        // at the protocol's sustainable rate a full track takes minutes to
        // transfer — so uploading sequentially from the start meant the region
        // actually being played stayed empty for the entire upload, which is
        // why no waveform appeared on the screens.
        const int startWindow = totalWindows > 0
            ? std::clamp(m_uploadStartWindows[deck], 0, totalWindows - 1)
            : 0;
        // kUploadWindowsPerTick existed but was never read: exactly one window
        // went out per 10 ms tick, so a five-minute track needed ~3300 ticks,
        // i.e. over half a minute before the jog screen was complete. That is
        // the "it keeps loading forever" part. The state packets that drive the
        // platter are published into their own priority slot, so they are not
        // queued behind this burst.
        for (int sent = 0;
             sent < windowsThisTick && m_uploadWindowsSent[deck] < totalWindows;
             ++sent) {
            const int window = waveformSweepWindow(
                startWindow, m_uploadWindowsSent[deck], totalWindows);
            if (!sendXx36Window(deck, m_waveforms[deck], window * kXx36EntriesPerWindow))
                break;
            ++m_uploadWindowsSent[deck];
        }

        if (m_uploadWindowsSent[deck] >= totalWindows) {
            if (m_uploadResweepPending[deck]) {
                // Better samples landed while this sweep was running. Send the
                // whole waveform once more so the windows that went out with
                // incomplete data are replaced.
                m_uploadResweepPending[deck] = false;
                beginWaveformSweep(deck);
                qInfo() << "[DDJ-FLX10] Deck" << deck
                        << "repeating waveform sweep with improved analysis data";
            } else {
                sendXx2f(deck);
                m_uploadActive[deck] = false;
                qInfo() << "[DDJ-FLX10] Deck" << deck << "waveform upload finished entries" << entries;
            }
        }
        m_nextUploadDeck = deck == 1 ? 2 : 1;
        break;
    }

    if (!m_uploadActive[1] && !m_uploadActive[2])
        m_uploadTimer.stop();
}
