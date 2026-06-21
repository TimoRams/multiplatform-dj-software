#include "DDJFLX10Controller.h"

#include "DjEngine.h"
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

namespace {
constexpr uint16_t kVid = 0x2B73;
constexpr uint16_t kPid = 0x0041;
constexpr int kScreenInterface = 5;
constexpr int kHidPacketSize = 128;
constexpr double kPreviewDurationSeconds = 30.0;
constexpr double kJogWaveformEntriesPerSecond = 150.0;
constexpr int kMaxWaveformEntries = 0x7FF00;
constexpr int kUploadWindowsPerTick = 10;
constexpr int kAlbumArtMaxBytes = 119 + 122 * 254;
constexpr double kJogRingWarningSeconds = 30.0;
constexpr qint64 kJogRingBlinkIntervalMs = 500;
constexpr int kJogRingOnValue = 0x7F;
constexpr int kXx2fSampleRate = 22050;
constexpr int kXx2fRecordsPerPacket = 30;
constexpr int kXx36TrickleIntervalMs = 50;
constexpr std::array<uint8_t, 4> kXx2fBeatTypes = {0x03, 0x04, 0x00, 0x02};
constexpr std::array<uint8_t, 4> kXx2fStartMarker = {0x80, 0x02, 0x01, 0x00};

struct VendorUnlockCommand
{
    uint16_t value;
    uint16_t index;
};

constexpr VendorUnlockCommand kVendorUnlockCommands[] = {
    {0x0100, 0xC028},
    {0x0000, 0xC029},
    {0x0200, 0xC013},
    {0x0000, 0xC02B},
    {0x0100, 0xC026},
    {0x0000, 0xC01D},
    {0x0100, 0xC027},
};

constexpr const char* kSessionStart = "F0 00 20 7F 01 02 01 01 22 0F 0C 06 08 04 0A 02 02 05 00 00 0E 0A 0E 03 04 F7";
constexpr const char* kEnterHid = "F0 00 40 05 00 00 04 01 00 03 01 F7";
constexpr const char* kKeepAlive = "F0 00 40 05 00 00 04 01 00 50 31 F7";
constexpr const char* kDeckInit[] = {
    "",
    "F0 00 40 05 00 00 04 01 00 11 00 00 02 0E 0E 05 F7",
    "F0 00 40 05 00 00 04 01 00 12 00 00 02 0E 0E 05 F7",
    "F0 00 40 05 00 00 04 01 00 13 00 00 02 0E 0E 05 F7",
    "F0 00 40 05 00 00 04 01 00 14 00 00 02 0E 0E 05 F7",
};
constexpr const char* kGlobalB = "F0 00 40 05 00 00 04 01 00 0B 31 00 00 00 00 00 F7";
constexpr const char* kGlobalC = "F0 00 40 05 00 00 04 01 00 0C 00 00 02 0E 0E 05 00 01 F7";

const QList<QByteArray> kXx39Packets = {
    QByteArrayLiteral("10390100030000484f54204355450000000000000000000000000000000000000000000000003f000000000000000000000000000000000000000000000000000000000000023f000000000000000000000000000000000000000000000000000000000000023f00000000000000000000000000000000000000000000000000"),
    QByteArrayLiteral("1039020003000000000000023f000000000000000000000000000000000000000000000000000000000000023f000000000000000000000000000000000000000000000000000000000000023f000000000000000000000000000000000000000000000000000000000000023f00000000000000000000000000000000000000"),
    QByteArrayLiteral("1039030003000000000000000000000000023f00000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"),
};

uint8_t deckByte(int deck)
{
    return static_cast<uint8_t>(std::clamp(deck, 1, 4) * 0x10);
}

uint8_t displayDeckState(uint8_t db)
{
    switch (db) {
    case 0x10: return 0x02;
    case 0x20: return 0x01;
    case 0x30: return 0x04;
    case 0x40: return 0x03;
    default: return 0x02;
    }
}

double validTrackDuration(const DjEngine* engine)
{
    if (!engine || engine->getDuration() <= 0.0f)
        return kPreviewDurationSeconds;

    return std::max(1.0, static_cast<double>(engine->getDuration()));
}

double validTrackPosition(const DjEngine* engine, double duration)
{
    if (!engine)
        return 0.0;

    // Match TurntableIndicator / Waveform: atomic during scratch & release glide,
    // interpolated visual position while playing, frozen atomic when paused.
    const double pos = engine->isScratchVisualActive()
        ? engine->getPlayheadPositionAtomic()
        : (engine->isPlaying() ? engine->getVisualPosition()
                               : engine->getPlayheadPositionAtomic());
    return std::clamp(pos, 0.0, duration);
}

QByteArray packet()
{
    return QByteArray(kHidPacketSize, char(0));
}

uint8_t camelotKeyByte(int number, QChar side)
{
    static constexpr std::array<uint8_t, 13> kCamelotA = {
        0x80, 0x98, 0x8E, 0x84, 0x92, 0x88, 0x96,
        0x8C, 0x82, 0x90, 0x86, 0x94, 0x8A
    };
    static constexpr std::array<uint8_t, 13> kCamelotB = {
        0x99, 0x97, 0x8D, 0x83, 0x91, 0x87, 0x95,
        0x8B, 0x81, 0x8F, 0x85, 0x93, 0x89
    };

    if (number < 1 || number > 12)
        return 0x80;

    return side.toUpper() == QLatin1Char('B') ? kCamelotB[number] : kCamelotA[number];
}

uint8_t musicalKeyByte(QString key)
{
    const QString original = key.trimmed();
    if (original.isEmpty())
        return 0x80;

    static const QRegularExpression camelotRe(
        QStringLiteral("\\b(1[0-2]|[1-9])\\s*([AB])\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch camelotMatch = camelotRe.match(original);
    if (camelotMatch.hasMatch())
        return camelotKeyByte(camelotMatch.captured(1).toInt(), camelotMatch.captured(2).at(0));

    static const QRegularExpression noteRe(
        QStringLiteral("^\\s*([A-GH])\\s*([#♯b♭]?)(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch noteMatch = noteRe.match(original);
    if (!noteMatch.hasMatch())
        return 0x80;

    QString note = noteMatch.captured(1).toUpper();
    QString accidental = noteMatch.captured(2);
    const QString rest = noteMatch.captured(3).trimmed().toLower();
    accidental.replace(QStringLiteral("♯"), QStringLiteral("#"));
    accidental.replace(QStringLiteral("♭"), QStringLiteral("b"));
    accidental = accidental.toLower();

    int semitone = -1;
    if (note == QLatin1String("C")) semitone = 0;
    else if (note == QLatin1String("D")) semitone = 2;
    else if (note == QLatin1String("E")) semitone = 4;
    else if (note == QLatin1String("F")) semitone = 5;
    else if (note == QLatin1String("G")) semitone = 7;
    else if (note == QLatin1String("A")) semitone = 9;
    else if (note == QLatin1String("B") || note == QLatin1String("H")) semitone = 11;

    if (semitone < 0)
        return 0x80;
    if (accidental == QLatin1String("#"))
        semitone = (semitone + 1) % 12;
    else if (accidental == QLatin1String("b"))
        semitone = (semitone + 11) % 12;

    const bool minor = rest.startsWith(QLatin1Char('m')) && !rest.startsWith(QLatin1String("maj"));
    static constexpr std::array<int, 12> kMajorCamelot = {8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
    static constexpr std::array<int, 12> kMinorCamelot = {5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};
    return minor ? camelotKeyByte(kMinorCamelot[semitone], QLatin1Char('A'))
                 : camelotKeyByte(kMajorCamelot[semitone], QLatin1Char('B'));
}

QByteArray bytesFromHexString(QString hex)
{
    hex.remove(QLatin1Char(' '));
    hex.remove(QLatin1Char('\n'));
    hex.remove(QLatin1Char('\r'));
    hex.remove(QLatin1Char('\t'));
    return QByteArray::fromHex(hex.toLatin1());
}

void put8(QByteArray& p, int index, int value)
{
    if (index < 0 || index >= p.size())
        return;
    p[index] = static_cast<char>(value & 0xFF);
}

QByteArray encodePwv5Entry(int height, int red, int green, int blue)
{
    height = std::clamp(height, 0, 31);
    red = std::clamp(red, 0, 7);
    green = std::clamp(green, 0, 7);
    blue = std::clamp(blue, 0, 7);
    const int value = (red << 13) | (green << 10) | (blue << 7) | (height << 2);
    QByteArray out;
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    return out;
}

QByteArray encodeCoverJpeg(const QImage& source, int side, int quality)
{
    if (source.isNull())
        return {};

    QImage canvas(side, side, QImage::Format_RGB888);
    canvas.fill(Qt::black);

    const QImage scaled = source.convertToFormat(QImage::Format_RGB888)
                             .scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&canvas);
    painter.drawImage((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    painter.end();

    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    canvas.save(&buffer, "JPEG", quality);
    return out;
}
}

DDJFLX10Controller::DDJFLX10Controller(QObject* parent)
    : QObject(parent)
{
    m_keepAliveTimer.setTimerType(Qt::PreciseTimer);
    m_stateTimer.setTimerType(Qt::PreciseTimer);
    m_waveformTimer.setTimerType(Qt::PreciseTimer);
    m_uploadTimer.setTimerType(Qt::PreciseTimer);

    connect(&m_keepAliveTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendKeepAlive);
    connect(&m_stateTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendStateTick);
    connect(&m_waveformTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendWaveformTick);
    connect(&m_uploadTimer, &QTimer::timeout, this, &DDJFLX10Controller::sendUploadChunk);

    setStatus(QStringLiteral("DDJ-FLX10 support disabled"));
}

DDJFLX10Controller::~DDJFLX10Controller()
{
    stop();
}

bool DDJFLX10Controller::start()
{
    stop();

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

    setConnected(true);
    m_clockStartMs = QDateTime::currentMSecsSinceEpoch();
    refreshDeckFromEngine(1);
    refreshDeckFromEngine(2);
    m_stateTimer.start(5);
    m_waveformTimer.start(kXx36TrickleIntervalMs);
    if (sequencerMidiReady || !m_midiPort.isEmpty())
        m_keepAliveTimer.start(500);

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
    QObject::disconnect(&m_keepAliveTimer, nullptr, this, nullptr);
    QObject::disconnect(&m_stateTimer, nullptr, this, nullptr);
    QObject::disconnect(&m_waveformTimer, nullptr, this, nullptr);
    QObject::disconnect(&m_uploadTimer, nullptr, this, nullptr);
    QCoreApplication::removePostedEvents(this);
    m_deckA = nullptr;
    m_deckB = nullptr;
}

void DDJFLX10Controller::stop()
{
    prepareForShutdown();
    m_uploadActive.fill(false);
    m_uploadEntries.fill(0);
    for (int deck = 1; deck <= 4; ++deck) {
        if (m_jogRingWarningActive[deck] || !m_jogRingLit[deck])
            sendJogRingIllumination(deck, true);
    }
    m_jogRingWarningActive.fill(false);
    m_jogRingLit.fill(true);

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
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
            m_lastWaveformRefreshMs[deck] = 0;
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
                m_lastWaveformRefreshMs[deck] = 0;
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
            const double seedPos = deckDisplayPosition(deck);
            resetDisplayInterp(deck, seedPos);
            pushDeckJogDisplay(deck);
        });
        m_progressConnections[deck] = connect(engine, &DjEngine::progressChanged, this, [this, deck] {
            if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
                return;
            const DjEngine* eng = deckEngine(deck);
            if (!eng || !eng->isScratchVisualActive() || m_waveforms[deck].isEmpty())
                return;
            pushDeckJogDisplay(deck);
        });
    }
}

void DDJFLX10Controller::resetDeckWaveformOutput(int deck)
{
    if (deck < 1 || deck > 2)
        return;

    m_waveforms[deck].clear();
    m_waveformDurations[deck] = kPreviewDurationSeconds;
    m_uploadActive[deck] = false;
    m_uploadEntries[deck] = 0;
    m_jogRingWarningActive[deck] = false;
    m_lastCoverUrls[deck].clear();
    resetDisplayInterp(deck);
    sendJogRingIllumination(deck, true);
    qInfo() << "[DDJ-FLX10] Deck" << deck << "has no track waveform; HID deck output stopped";
    if (m_connected)
        clearDeckDisplay(deck);
}

void DDJFLX10Controller::refreshDeckFromEngine(int deck)
{
    if (m_shuttingDown.load(std::memory_order_acquire))
        return;

    if (DjEngine* engine = deckEngine(deck); !engine || !engine->hasTrack())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_connected && m_lastWaveformRefreshMs[deck] > 0 && now - m_lastWaveformRefreshMs[deck] < 1500)
        return;
    m_lastWaveformRefreshMs[deck] = now;
    resetDisplayInterp(deck);

    m_waveforms[deck] = generatePreviewWaveform(deck);

    m_waveformDurations[deck] = deckDisplayDuration(deck);

    if (!m_connected)
        return;

    if (m_waveforms[deck].isEmpty()) {
        qInfo() << "[DDJ-FLX10] Deck" << deck << "has no track waveform; leaving jog display inactive";
        clearDeckDisplay(deck);
        return;
    }

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

bool DDJFLX10Controller::initialiseUsb()
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    const int initResult = libusb_init(&m_usbContext);
    if (initResult != 0) {
        setStatus(QStringLiteral("DDJ-FLX10: libusb init failed (%1)").arg(initResult));
        return false;
    }

    libusb_device** devices = nullptr;
    const ssize_t deviceCount = libusb_get_device_list(m_usbContext, &devices);
    libusb_device* flx10Device = nullptr;
    for (ssize_t i = 0; i < deviceCount; ++i) {
        libusb_device_descriptor descriptor {};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0)
            continue;
        if (descriptor.idVendor == kVid && descriptor.idProduct == kPid) {
            flx10Device = devices[i];
            break;
        }
    }

    if (!flx10Device) {
        setStatus(QStringLiteral("DDJ-FLX10: USB device 2B73:0041 not found"));
        if (devices)
            libusb_free_device_list(devices, 1);
        libusb_exit(m_usbContext);
        m_usbContext = nullptr;
        return false;
    }

    const int openResult = libusb_open(flx10Device, &m_handle);
    libusb_free_device_list(devices, 1);
    if (openResult != 0 || !m_handle) {
        setStatus(QStringLiteral("DDJ-FLX10: USB device found, but access was denied (%1). Install the BrockDJ udev rule, then reconnect the controller.")
                      .arg(openResult));
        libusb_exit(m_usbContext);
        m_usbContext = nullptr;
        return false;
    }

    libusb_set_auto_detach_kernel_driver(m_handle, 1);
    libusb_set_configuration(m_handle, 1);
    return true;
#else
    return false;
#endif
}

bool DDJFLX10Controller::sendVendorUnlock()
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    if (!m_handle)
        return false;

    bool anyOk = false;
    for (const auto& command : kVendorUnlockCommands) {
        const int result = libusb_control_transfer(m_handle,
                                                   0x40,
                                                   3,
                                                   command.value,
                                                   command.index,
                                                   nullptr,
                                                   0,
                                                   500);
        anyOk = anyOk || result >= 0;
        QThread::msleep(5);
    }
    QThread::msleep(250);
    return anyOk;
#else
    return false;
#endif
}

bool DDJFLX10Controller::claimScreenInterface()
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    if (!m_handle)
        return false;

    const int claimResult = libusb_claim_interface(m_handle, kScreenInterface);
    if (claimResult != 0) {
        qWarning() << "[DDJ-FLX10] claim interface failed" << claimResult;
        return false;
    }
    m_interfaceClaimed = true;

    libusb_device* device = libusb_get_device(m_handle);
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != 0 || config == nullptr)
        return false;

    bool found = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& usbInterface = config->interface[i];
        for (int j = 0; j < usbInterface.num_altsetting && !found; ++j) {
            const libusb_interface_descriptor& descriptor = usbInterface.altsetting[j];
            if (descriptor.bInterfaceNumber != kScreenInterface)
                continue;

            for (int e = 0; e < descriptor.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[e];
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                    m_outEndpoint = endpoint.bEndpointAddress;
                    found = true;
                    break;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    if (!found)
        qWarning() << "[DDJ-FLX10] no HID OUT endpoint found";
    return found;
#else
    return false;
#endif
}

bool DDJFLX10Controller::writePacket(const QByteArray& packetBytes)
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    if (!m_handle || !m_outEndpoint || packetBytes.size() != kHidPacketSize)
        return false;

    int transferred = 0;
    const int result = libusb_interrupt_transfer(m_handle,
                                                 m_outEndpoint,
                                                 reinterpret_cast<unsigned char*>(const_cast<char*>(packetBytes.constData())),
                                                 packetBytes.size(),
                                                 &transferred,
                                                 1000);
    if (result != 0 || transferred != packetBytes.size()) {
        qWarning() << "[DDJ-FLX10] HID write failed" << result << transferred;
        return false;
    }
    return true;
#else
    Q_UNUSED(packetBytes)
    return false;
#endif
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
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;
    sendMidiHex(QString::fromLatin1(kKeepAlive));
}

void DDJFLX10Controller::resetDisplayInterp(int deck, double seedFileSec)
{
    if (deck < 0 || deck >= static_cast<int>(m_displayInterp.size()))
        return;
    m_displayInterp[deck] = {};
    m_lastXx27Packet[deck].clear();

    if (seedFileSec >= 0.0) {
        auto& state = m_displayInterp[deck];
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        state.initialized = true;
        state.lastFilePos = seedFileSec;
        state.lastPosTimeMs = nowMs;
        state.lastNewPosTimeMs = nowMs;
        state.lastSmoothFileMs = seedFileSec * 1000.0;
    }
}

double DDJFLX10Controller::smoothFileElapsedSec(int deck, double fileElapsedSec, double rateRatio, bool playing)
{
    constexpr qint64 kScrubHoldMs = 200;
    constexpr qint64 kRunawayClampMs = 500;

    if (deck < 0 || deck >= static_cast<int>(m_displayInterp.size()))
        return fileElapsedSec;

    auto& state = m_displayInterp[deck];
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double safeRate = std::max(0.01, rateRatio);
    const double fileMs = fileElapsedSec * 1000.0;

    if (!state.initialized) {
        state.initialized = true;
        state.lastFilePos = fileElapsedSec;
        state.lastPosTimeMs = nowMs;
        state.lastNewPosTimeMs = nowMs;
        state.lastSmoothFileMs = fileMs;
        return fileElapsedSec;
    }

    if (!playing) {
        state.lastFilePos = fileElapsedSec;
        state.lastPosTimeMs = nowMs;
        state.lastNewPosTimeMs = nowMs;
        state.lastSmoothFileMs = fileMs;
        return fileElapsedSec;
    }

    if (std::abs(fileElapsedSec - state.lastFilePos) > 0.005) {
        state.lastFilePos = fileElapsedSec;
        state.lastPosTimeMs = nowMs;
        state.lastNewPosTimeMs = nowMs;
        state.lastSmoothFileMs = fileMs;
    } else if (fileElapsedSec != state.lastFilePos) {
        const double posDelta = fileElapsedSec - state.lastFilePos;
        state.lastFilePos = fileElapsedSec;
        state.lastNewPosTimeMs = nowMs;
        if (posDelta < 0.0) {
            state.lastSmoothFileMs = fileMs;
            state.lastPosTimeMs = nowMs;
        } else {
            const double extrapolatedMs = state.lastSmoothFileMs
                + static_cast<double>(nowMs - state.lastPosTimeMs) * safeRate;
            if (fileMs > extrapolatedMs) {
                state.lastSmoothFileMs = fileMs;
                state.lastPosTimeMs = nowMs;
            }
        }
    }

    const qint64 msSince = nowMs - state.lastPosTimeMs;
    double smoothMs = state.lastSmoothFileMs + static_cast<double>(msSince) * safeRate;

    if (nowMs - state.lastNewPosTimeMs > kScrubHoldMs) {
        state.lastSmoothFileMs = fileMs;
        state.lastPosTimeMs = nowMs;
        smoothMs = fileMs;
    } else {
        const double maxMs = fileMs + static_cast<double>(kRunawayClampMs) * safeRate;
        if (smoothMs > maxMs) {
            state.lastSmoothFileMs = fileMs;
            state.lastPosTimeMs = nowMs;
            smoothMs = fileMs;
        }
    }

    return std::max(0.0, smoothMs / 1000.0);
}

void DDJFLX10Controller::pushDeckJogDisplay(int deck)
{
    if (deck < 1 || deck > 2)
        return;
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;
    if (m_waveforms[deck].isEmpty())
        return;

    const DjEngine* engine = deckEngine(deck);
    const double duration = deckDisplayDuration(deck);
    const bool playIntent = engine ? engine->isPlaying() : true;
    const bool scratchVisual = engine && engine->isScratchVisualActive();
    const bool moving = playIntent || scratchVisual;
    const double rateRatio = engine ? engine->getTempoRatio() : 1.0;
    const double rawFileElapsed = engine
        ? deckDisplayPosition(deck)
        : std::fmod((QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0,
                    duration);
    const double fileClamped = std::clamp(rawFileElapsed, 0.0, duration);
    const double displayElapsed = scratchVisual
        ? fileClamped
        : smoothFileElapsedSec(deck, fileClamped, rateRatio, moving);
    sendXx27(deck, displayElapsed, duration, deckBpm(deck), moving);
    updateJogRingWarning(deck, fileClamped, duration, playIntent);
}

void DDJFLX10Controller::sendStateTick()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    for (int deck = 1; deck <= 2; ++deck)
        pushDeckJogDisplay(deck);
}

void DDJFLX10Controller::sendWaveformTick()
{
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    for (int deck = 1; deck <= 2; ++deck)
        if (!m_waveforms[deck].isEmpty() && !m_uploadActive[deck])
            sendXx36Window(deck, m_waveforms[deck], currentWaveformEntry(deck));
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
            sendXx36Window(deck, m_waveforms[deck], m_uploadEntries[deck]);
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

bool DDJFLX10Controller::uploadDeck(int deck)
{
    if (m_waveforms[deck].isEmpty())
        return true;

    bool ok = true;
    ok = sendXx30(deck) && ok;
    ok = sendXx39(deck) && ok;
    ok = uploadCoverArt(deck) && ok;
    ok = sendXx35(deck, m_waveforms[deck].size() / 2) && ok;

    m_uploadEntries[deck] = 0;
    m_uploadActive[deck] = true;
    if (!m_uploadTimer.isActive())
        m_uploadTimer.start(2);
    return ok;
}

bool DDJFLX10Controller::sendXx30(int deck)
{
    QByteArray p = packet();
    put8(p, 0, deckByte(deck));
    put8(p, 1, 0x30);
    put8(p, 2, 0x01);
    put8(p, 4, 0x01);
    for (int index : {10, 16, 22, 28, 34, 40, 46, 52})
        put8(p, index, 0xFF);
    return writePacket(p);
}

bool DDJFLX10Controller::sendXx39(int deck)
{
    bool ok = true;
    for (int packetIndex = 0; packetIndex < kXx39Packets.size(); ++packetIndex) {
        const QByteArray& hex = kXx39Packets[packetIndex];
        QByteArray p = QByteArray::fromHex(hex);
        if (p.size() < kHidPacketSize)
            p.resize(kHidPacketSize);
        if (p.size() > kHidPacketSize)
            p.truncate(kHidPacketSize);
        put8(p, 0, deckByte(deck));
        ok = writePacket(p) && ok;
    }
    return ok;
}

bool DDJFLX10Controller::sendXx33Album(int deck, const QByteArray& jpeg)
{
    if (jpeg.isEmpty())
        return true;

    const int firstCapacity = 119;
    const int nextCapacity = 122;
    const int maxBytes = kAlbumArtMaxBytes;
    const int jpegSize = std::min(static_cast<int>(jpeg.size()), maxBytes);
    const int totalPackets = jpegSize <= firstCapacity
                                 ? 1
                                 : 1 + ((jpegSize - firstCapacity + nextCapacity - 1) / nextCapacity);

    int offset = 0;
    bool ok = true;
    for (int segment = 1; segment <= totalPackets; ++segment) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, 0x33);
        put8(p, 2, segment);
        put8(p, 4, totalPackets);

        if (segment == 1) {
            put8(p, 6, jpegSize);
            put8(p, 7, jpegSize >> 8);
            const int take = std::min(firstCapacity, jpegSize - offset);
            p.replace(9, take, jpeg.constData() + offset, take);
            offset += take;
        } else {
            const int take = std::min(nextCapacity, jpegSize - offset);
            p.replace(6, take, jpeg.constData() + offset, take);
            offset += take;
        }

        ok = writePacket(p) && ok;
    }
    return ok;
}

bool DDJFLX10Controller::sendXx35(int deck, int entryCount)
{
    QByteArray clear = packet();
    put8(clear, 0, deckByte(deck));
    put8(clear, 1, 0x35);
    bool ok = writePacket(clear);

    for (int i = 0; i < 2; ++i) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, 0x35);
        put8(p, 2, entryCount);
        put8(p, 3, entryCount >> 8);
        put8(p, 4, entryCount >> 16);
        put8(p, 5, entryCount >> 24);
        ok = writePacket(p) && ok;
    }
    return ok;
}

bool DDJFLX10Controller::sendXx36Window(int deck, const QByteArray& waveform, int entry)
{
    const int entryCount = waveform.size() / 2;
    if (entryCount <= 0)
        return false;

    entry = std::clamp(entry, 0, std::max(0, entryCount - 19));
    const int take = std::min(19, entryCount - entry);

    QByteArray p = packet();
    put8(p, 0, deckByte(deck));
    put8(p, 1, 0x36);
    put8(p, 2, 0x01);
    put8(p, 4, 0x01);
    put8(p, 6, 0x13);
    put8(p, 10, entry);
    put8(p, 11, entry >> 8);
    put8(p, 12, entry >> 16);
    put8(p, 13, entry >> 24);
    p.replace(14, take * 2, waveform.mid(entry * 2, take * 2));
    return writePacket(p);
}

bool DDJFLX10Controller::sendXx2f(int deck)
{
    struct Xx2fRecord {
        uint8_t type = 0;
        uint32_t samples = 0;
    };

    std::vector<Xx2fRecord> records;
    records.reserve(1 + 512);
    records.push_back({kXx2fStartMarker[0],
                       static_cast<uint32_t>(kXx2fStartMarker[1]
                                             | (kXx2fStartMarker[2] << 8)
                                             | (kXx2fStartMarker[3] << 16))});

    const std::vector<double> beatTimesMs = deckBeatTimesMs(deck);
    for (size_t i = 0; i < beatTimesMs.size(); ++i) {
        const double clampedMs = std::max(0.0, beatTimesMs[i]);
        const uint32_t samples = static_cast<uint32_t>(
            std::llround(clampedMs * static_cast<double>(kXx2fSampleRate) / 1000.0)) & 0x00FFFFFFu;
        records.push_back({kXx2fBeatTypes[i % kXx2fBeatTypes.size()], samples});
    }

    const int totalPackets = std::max(1, static_cast<int>(
        (records.size() + kXx2fRecordsPerPacket - 1) / kXx2fRecordsPerPacket));
    const int packetsToSend = std::min(totalPackets, 255);

    bool ok = true;
    for (int packetIndex = 0; packetIndex < packetsToSend; ++packetIndex) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, 0x2F);
        put8(p, 2, packetIndex + 1);
        put8(p, 3, 0x00);
        put8(p, 4, 0x15);
        put8(p, 5, 0x00);

        int offset = 6;
        const size_t firstRecord = static_cast<size_t>(packetIndex * kXx2fRecordsPerPacket);
        const size_t endRecord = std::min(records.size(), firstRecord + kXx2fRecordsPerPacket);
        for (size_t recordIndex = firstRecord; recordIndex < endRecord; ++recordIndex) {
            if (offset + 3 >= kHidPacketSize)
                break;
            const Xx2fRecord& record = records[recordIndex];
            put8(p, offset, record.type);
            put8(p, offset + 1, record.samples & 0xFF);
            put8(p, offset + 2, (record.samples >> 8) & 0xFF);
            put8(p, offset + 3, (record.samples >> 16) & 0xFF);
            offset += 4;
        }

        ok = writePacket(p) && ok;
    }

    qInfo() << "[DDJ-FLX10] Deck" << deck
            << "sent xx2F beatgrid records" << static_cast<int>(records.size() - 1)
            << "packets" << packetsToSend;
    return ok;
}

bool DDJFLX10Controller::clearDeckDisplay(int deck)
{
    m_uploadActive[deck] = false;
    m_uploadEntries[deck] = 0;

    bool ok = true;
    for (int command : {0x27, 0x30, 0x33, 0x35, 0x36, 0x2F}) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, command);
        ok = writePacket(p) && ok;
    }
    return ok;
}

bool DDJFLX10Controller::sendXx27(int deck, double fileElapsedSeconds, double durationSeconds, double bpm, bool moving)
{
    Q_UNUSED(moving);

    const uint8_t db = deckByte(deck);
    QByteArray p = packet();
    put8(p, 0, db);
    put8(p, 1, 0x27);
    put8(p, 2, 0xB4);
    put8(p, 3, 0x80);
    put8(p, 4, 0x01);

    const double tempoPercent = std::clamp(deckTempoPercent(deck), -100.0, 100.0);
    const double rateRatio = std::max(0.01, 1.0 + tempoPercent / 100.0);
    fileElapsedSeconds = std::max(0.0, fileElapsedSeconds);
    durationSeconds = std::max(1.0, durationSeconds);
    fileElapsedSeconds = std::clamp(fileElapsedSeconds, 0.0, durationSeconds);

    // Needle/handle position is FILE time (track position). Tempo stretch is
    // communicated via bytes 16–17 and wall-time remaining in bytes 9–12 only.
    // Sub-second field is MILLISECONDS (0..999), not 1024ths.
    const double totalSec = fileElapsedSeconds;
    const int secInt = static_cast<int>(std::floor(totalSec));
    const double sub = totalSec - static_cast<double>(secInt);
    int subMs = static_cast<int>(std::floor(sub * 1000.0));
    if (subMs > 999)
        subMs = 999;

    put8(p, 5, (secInt / 60) & 0xFF);
    put8(p, 6, (secInt % 60) & 0xFF);
    put8(p, 7, subMs & 0xFF);
    put8(p, 8, (subMs >> 8) & 0x03);

    const int durationMs = static_cast<int>(std::floor((durationSeconds / rateRatio) * 1000.0));
    put8(p, 9, durationMs / 60000);
    const int rem2 = durationMs % 60000;
    put8(p, 10, rem2 / 1000);
    const int ms2 = rem2 % 1000;
    put8(p, 11, ms2);
    put8(p, 12, ms2 >> 8);

    const int bpmInt = static_cast<int>(bpm);
    put8(p, 13, bpmInt);
    put8(p, 14, (static_cast<int>(std::round((bpm - bpmInt) * 10.0)) & 0x0F) << 4);
    put8(p, 15, 0x01);
    const int tempoEnc = std::clamp(static_cast<int>(std::llround(tempoPercent * 100.0)), -32768, 32767);
    const uint16_t tempoWire = static_cast<uint16_t>(tempoEnc & 0xFFFF);
    put8(p, 16, tempoWire & 0xFF);
    put8(p, 17, (tempoWire >> 8) & 0xFF);
    put8(p, 20, 0x0E);

    // Platter ring phase at 33⅓ RPM. Bytes 21–22 must share one revolution tick
    // derived from the same subsecTicks as 5–8 — mismatched wraps cause jitter
    // and snap-backs around 12 o'clock on the jog display.
    const int totalMs = secInt * 1000 + subMs;
    constexpr double kVinylRevolutionSeconds = 60.0 / (100.0 / 3.0);
    const int ticksPerRevolution = static_cast<int>(std::lround(kVinylRevolutionSeconds * 1024.0));
    const int sub1024 = std::min(1023, (subMs * 1024) / 1000);
    const int subsecTicks = (totalMs / 1000) * 1024 + sub1024;
    const int revolutionTick = ticksPerRevolution > 0
        ? (subsecTicks % ticksPerRevolution + ticksPerRevolution) % ticksPerRevolution
        : 0;
    put8(p, 21, (revolutionTick * 2) & 0xFF);
    put8(p, 22, ticksPerRevolution > 0
        ? (revolutionTick * 15 / ticksPerRevolution) % 15
        : 0);

    put8(p, 25, 0x80);
    put8(p, 29, deckKeyByte(deck));
    put8(p, 30, 0x0D);
    put8(p, 31, displayDeckState(db));
    put8(p, 32, 0xFF);
    put8(p, 33, 0xFF);
    put8(p, 34, 0xFF);

    if (deck >= 0 && deck < static_cast<int>(m_lastXx27Packet.size())
        && m_lastXx27Packet[deck] == p) {
        return true;
    }

    const bool ok = writePacket(p);
    if (deck >= 0 && deck < static_cast<int>(m_lastXx27Packet.size()))
        m_lastXx27Packet[deck] = p;
    return ok;
}

QByteArray DDJFLX10Controller::generateCoverJpeg(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine || !engine->hasTrack() || !engine->hasCoverArt())
        return {};

    const QImage cover = engine->currentCoverImage();
    if (cover.isNull())
        return {};

    for (const auto [side, quality] : {std::pair{240, 86}, std::pair{240, 74}, std::pair{180, 72}, std::pair{160, 66}}) {
        const QByteArray jpeg = encodeCoverJpeg(cover, side, quality);
        if (!jpeg.isEmpty() && jpeg.size() <= kAlbumArtMaxBytes)
            return jpeg;
    }

    return {};
}

bool DDJFLX10Controller::uploadCoverArt(int deck)
{
    const QByteArray jpeg = generateCoverJpeg(deck);
    if (jpeg.isEmpty())
        return true;

    qInfo() << "[DDJ-FLX10] Deck" << deck << "uploading cover art bytes" << jpeg.size();
    return sendXx33Album(deck, jpeg);
}

QByteArray DDJFLX10Controller::generatePreviewWaveform(int deck) const
{
    const DjEngine* engine = deck == 1 ? m_deckA : m_deckB;
    if (!engine || engine->getDuration() <= 0.0f)
        return {};

    TrackData* trackData = engine->getTrackData();
    if (!trackData)
        return {};

    QVector<TrackData::RgbWaveformFrame> frames = trackData->getRgbWaveformData();
    if (frames.isEmpty())
        frames = trackData->getOverviewRgbData();
    if (frames.isEmpty())
        frames = trackData->getProgressiveOvrData();
    if (frames.isEmpty())
        return {};

    const int targetEntries = std::clamp(
        static_cast<int>(std::ceil(deckDisplayDuration(deck) * kJogWaveformEntriesPerSecond)),
        150,
        kMaxWaveformEntries);
    QByteArray out;
    out.reserve(targetEntries * 2);

    for (int i = 0; i < targetEntries; ++i) {
        const double startFraction = static_cast<double>(i) / static_cast<double>(targetEntries);
        const double endFraction = static_cast<double>(i + 1) / static_cast<double>(targetEntries);
        const int startIndex = std::clamp(static_cast<int>(std::floor(startFraction * frames.size())), 0, static_cast<int>(frames.size() - 1));
        const int endIndex = std::clamp(static_cast<int>(std::ceil(endFraction * frames.size())), startIndex + 1, static_cast<int>(frames.size()));

        float rms = 0.0f;
        float low = 0.0f;
        float lowMid = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
        int fallbackRed = 0;
        int fallbackGreen = 0;
        int fallbackBlue = 0;
        int colorCount = 0;

        for (int src = startIndex; src < endIndex; ++src) {
            const auto& frame = frames[src];
            rms = std::max(rms, std::max(0.0f, frame.rms));
            low = std::max(low, std::max(0.0f, frame.low));
            lowMid = std::max(lowMid, std::max(0.0f, frame.lowMid));
            mid = std::max(mid, std::max(0.0f, frame.mid));
            high = std::max(high, std::max(0.0f, frame.high));
            fallbackRed += frame.color.red();
            fallbackGreen += frame.color.green();
            fallbackBlue += frame.color.blue();
            ++colorCount;
        }

        const int height = std::clamp(static_cast<int>(std::sqrt(rms) * 31.0f), 1, 31);
        const float bandMax = std::max({low, lowMid, mid, high, 0.001f});

        int red = std::clamp(static_cast<int>(std::round(7.0f * (0.90f * low + 0.45f * lowMid) / bandMax)), 0, 7);
        int green = std::clamp(static_cast<int>(std::round(7.0f * (0.75f * mid + 0.35f * lowMid) / bandMax)), 0, 7);
        int blue = std::clamp(static_cast<int>(std::round(7.0f * (0.90f * high + 0.25f * mid) / bandMax)), 0, 7);

        if (red == 0 && green == 0 && blue == 0) {
            const QColor color(
                colorCount > 0 ? fallbackRed / colorCount : 255,
                colorCount > 0 ? fallbackGreen / colorCount : 255,
                colorCount > 0 ? fallbackBlue / colorCount : 255);
            red = std::clamp((color.red() + 15) / 32, 0, 7);
            green = std::clamp((color.green() + 15) / 32, 0, 7);
            blue = std::clamp((color.blue() + 15) / 32, 0, 7);
        }

        out += encodePwv5Entry(height, red, green, blue);
    }

    return out;
}

double DDJFLX10Controller::deckDisplayDuration(int deck) const
{
    return validTrackDuration(deckEngine(deck));
}

double DDJFLX10Controller::deckDisplayPosition(int deck) const
{
    return validTrackPosition(deckEngine(deck), deckDisplayDuration(deck));
}

double DDJFLX10Controller::deckBpm(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine || engine->getCurrentBpm() <= 0.0)
        return 0.0;
    return engine->getCurrentBpm();
}

double DDJFLX10Controller::deckTempoPercent(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    return engine ? engine->getTempoPercent() : 0.0;
}

double DDJFLX10Controller::deckTempoRangePercent(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    return engine ? engine->tempoRangePercent() : 8.0;
}

QString DDJFLX10Controller::deckKey(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine)
        return {};

    if (const TrackData* trackData = engine->getTrackData()) {
        const QString detectedKey = trackData->getDetectedKey().trimmed();
        if (!detectedKey.isEmpty())
            return detectedKey;
    }

    return engine->trackKey().trimmed();
}

uint8_t DDJFLX10Controller::deckKeyByte(int deck) const
{
    return musicalKeyByte(deckKey(deck));
}

std::vector<double> DDJFLX10Controller::deckBeatTimesMs(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    const TrackData* trackData = engine ? engine->getTrackData() : nullptr;
    const double duration = deckDisplayDuration(deck);
    std::vector<double> timesMs;

    if (trackData) {
        const std::vector<TrackData::BeatMarker> grid = trackData->getBeatGrid();
        timesMs.reserve(grid.size());
        for (const TrackData::BeatMarker& marker : grid) {
            if (!marker.isBeat || marker.positionSec < 0.0 || marker.positionSec > duration)
                continue;
            timesMs.push_back(marker.positionSec * 1000.0);
        }

        if (!timesMs.empty())
            return timesMs;

        const double bpm = trackData->getBpm();
        if (bpm > 0.0 && duration > 0.0) {
            const double sampleRate = trackData->getSampleRate();
            const double firstBeatSec = sampleRate > 0.0
                                            ? static_cast<double>(trackData->getFirstBeatSample()) / sampleRate
                                            : 0.0;
            const double beatLengthSec = 60.0 / bpm;
            if (beatLengthSec > 0.001) {
                double firstVisibleBeat = firstBeatSec;
                while (firstVisibleBeat > 0.0)
                    firstVisibleBeat -= beatLengthSec;
                while (firstVisibleBeat < 0.0)
                    firstVisibleBeat += beatLengthSec;

                for (double sec = firstVisibleBeat; sec <= duration; sec += beatLengthSec)
                    timesMs.push_back(sec * 1000.0);
            }
        }
    }

    return timesMs;
}

int DDJFLX10Controller::currentWaveformEntry(int deck) const
{
    const QByteArray& waveform = m_waveforms[deck];
    const int entries = waveform.size() / 2;
    if (entries <= 19 || m_clockStartMs <= 0)
        return 0;

    const DjEngine* engine = deckEngine(deck);
    if (engine && engine->getDuration() > 0.0f) {
        const double duration = validTrackDuration(engine);
        const double position = validTrackPosition(engine, duration);
        const double fraction = duration > 0.0 ? std::clamp(position / duration, 0.0, 1.0) : 0.0;
        return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
    }

    const double elapsed = (QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0;
    const double duration = std::max(1.0, m_waveformDurations[deck]);
    const double fraction = std::fmod(std::max(0.0, elapsed), duration) / duration;
    return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
}
