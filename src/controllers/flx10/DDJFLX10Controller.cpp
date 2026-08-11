#include "DDJFLX10Controller.h"

#include "controllers/DeckIndex.h"
#include "DjEngine.h"
#include "Flx10ProtocolCommon.h"
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

    connect(&m_uploadTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendUploadChunk);
    connect(&m_keepAliveTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendKeepAlive);
    ControlClock::Callbacks callbacks;
    callbacks.display = [this](const ControlTickContext&) {
        sendStateTick();
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
    setConnected(true);
    m_clockStartMs = QDateTime::currentMSecsSinceEpoch();
    m_hidTrafficClock.restart();
    m_lastXx27SentMs.fill(-kJogStateIntervalMs);
    m_lastXx36SentMs = -kXx36TrickleIntervalMs;
    for (int deck = controllers::kFlx10FirstDeckIndex;
         deck <= controllers::kFlx10LastDeckIndex; ++deck) {
        m_uploadActive[deck] = false;
        m_uploadEntries[deck] = 0;
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
    m_keepAliveEnabled = false;
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
    m_keepAliveEnabled = false;
#if defined(Q_OS_LINUX)
    if (m_keepAliveProcess && m_keepAliveProcess->state() != QProcess::NotRunning) {
        m_keepAliveProcess->kill();
        m_keepAliveProcess->waitForFinished(100);
    }
    m_keepAliveProcess.reset();
#endif
    m_uploadActive.fill(false);
    m_uploadEntries.fill(0);
    for (int deck = controllers::kFlx10FirstDeckIndex; deck <= controllers::kFlx10LastDeckIndex; ++deck) {
        if (m_jogRingWarningActive[deck] || !m_jogRingLit[deck])
            sendJogRingIllumination(deck, true);
    }
    m_jogRingWarningActive.fill(false);
    m_jogRingLit.fill(true);

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
            if (!m_connected || m_waveforms[deck].isEmpty())
                return;

            if (DjEngine* engine = deckEngine(deck)) {
                const QString coverUrl = engine->coverArtUrl();
                if (!coverUrl.isEmpty() && coverUrl != m_lastCoverUrls[deck]) {
                    m_lastCoverUrls[deck] = coverUrl;
                    uploadCoverArt(deck);
                }

                sendXx39(deck);
            }
        });
        // Tempo bytes in 0x27 are refreshed by sendStateTick(); avoid an extra
        // immediate packet here — it used to make the platter cursor jump wildly.

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
    m_uploadEntries[deck] = 0;
    m_lastWaveformRefreshMs[deck] = 0;
    m_lastCoverUrls[deck].clear();
    resetDisplayPacketState(deck);

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    discardQueuedDeckPackets(deck);
#endif
    if (clearDevice && m_connected)
        clearDeckDisplay(deck);
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

    // Analysis publishes RGB updates about every 90 ms. Rebuilding and restarting
    // the complete device waveform on each update made the jog screens visibly
    // reload forever. The first usable overview is a stable per-track snapshot;
    // the moving 0x36 window continues to use it for the lifetime of the track.
    if (!m_waveforms[deck].isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastWaveformRefreshMs[deck] > 0 && now - m_lastWaveformRefreshMs[deck] < 250)
        return;
    m_lastWaveformRefreshMs[deck] = now;
    resetDisplayPacketState(deck);

    QByteArray waveform = generatePreviewWaveform(deck);
    if (waveform.isEmpty())
        return;
    m_waveforms[deck] = std::move(waveform);
    m_waveformDurations[deck] = deckDisplayDuration(deck);

    if (!m_connected)
        return;

    qInfo() << "[DDJ-FLX10] Deck" << deck << "uploading real waveform entries" << (m_waveforms[deck].size() / 2);
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

    bool sent = false;
    for (int deck = 1; deck <= 2; ++deck)
        if (!m_waveforms[deck].isEmpty() && !m_uploadActive[deck])
            sent = sendXx36Window(deck, m_waveforms[deck], currentWaveformEntry(deck)) || sent;
    if (sent)
        m_lastXx36SentMs = nowMs;
}

void DDJFLX10Controller::sendUploadChunk()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected) {
        m_uploadActive.fill(false);
        return;
    }

    bool anyActive = false;
    for (int deck = 1; deck <= 2; ++deck) {
        if (!m_uploadActive[deck] || m_waveforms[deck].isEmpty())
            continue;

        anyActive = true;
        const int entries = m_waveforms[deck].size() / 2;
        int windowsSent = 0;
        while (m_uploadEntries[deck] < entries && windowsSent < kUploadWindowsPerTick) {
            if (!sendXx36Window(deck, m_waveforms[deck], m_uploadEntries[deck]))
                break;
            m_uploadEntries[deck] += 19;
            ++windowsSent;
        }

        if (m_uploadEntries[deck] >= entries) {
            sendXx2f(deck);
            m_uploadActive[deck] = false;
            qInfo() << "[DDJ-FLX10] Deck" << deck << "waveform upload finished entries" << entries;
        }
    }

    if (!anyActive || (!m_uploadActive[1] && !m_uploadActive[2]))
        m_uploadTimer.stop();
}
