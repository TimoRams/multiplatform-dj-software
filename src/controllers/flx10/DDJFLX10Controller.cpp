#include "DDJFLX10Controller.h"

#include "DjEngine.h"
#include "domain/TrackData.h"

#include <QDateTime>
#include <QDebug>
#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
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
constexpr int kMaxWaveformEntries = 65520;
constexpr int kUploadWindowsPerTick = 10;
constexpr int kAlbumArtMaxBytes = 119 + 122 * 254;

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

QByteArray packet()
{
    return QByteArray(kHidPacketSize, char(0));
}

void put8(QByteArray& p, int index, int value)
{
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

    m_midiPort = findMidiPort();
    if (!m_midiPort.isEmpty())
        sendSessionSysEx();
    else
        qWarning() << "[DDJ-FLX10] amidi port not found; SysEx gate skipped";

    if (!claimScreenInterface()) {
        stop();
        setStatus(QStringLiteral("DDJ-FLX10: could not claim HID interface 5 (run with udev permission or root)"));
        return false;
    }

    setConnected(true);
    m_clockStartMs = QDateTime::currentMSecsSinceEpoch();
    refreshDeckFromEngine(1);
    refreshDeckFromEngine(2);
    m_nextStateDeck = 1;
    m_stateTimer.start(10);
    m_waveformTimer.start(500);
    if (!m_midiPort.isEmpty())
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

void DDJFLX10Controller::stop()
{
    m_keepAliveTimer.stop();
    m_stateTimer.stop();
    m_waveformTimer.stop();
    m_uploadTimer.stop();
    m_uploadActive.fill(false);
    m_uploadEntries.fill(0);

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

    setConnected(false);
    if (!m_status.contains(QStringLiteral("disabled")))
        setStatus(QStringLiteral("DDJ-FLX10 support disabled"));
}

void DDJFLX10Controller::setDecks(DjEngine* deckA, DjEngine* deckB)
{
    m_deckA = deckA;
    m_deckB = deckB;
    connectDeckSignals();
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
    for (auto& connection : m_trackLoadedConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_rgbWaveformConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_overviewWaveformConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_dataClearedConnections)
        QObject::disconnect(connection);
    for (auto& connection : m_metadataConnections)
        QObject::disconnect(connection);

    for (int deck = 1; deck <= 2; ++deck) {
        DjEngine* engine = deckEngine(deck);
        if (!engine)
            continue;

        m_trackLoadedConnections[deck] = connect(engine, &DjEngine::trackLoaded, this, [this, deck] {
            m_lastWaveformRefreshMs[deck] = 0;
            refreshDeckFromEngine(deck);
        });
        m_metadataConnections[deck] = connect(engine, &DjEngine::trackMetadataChanged, this, [this, deck] {
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

        if (TrackData* trackData = engine->getTrackData()) {
            m_rgbWaveformConnections[deck] = connect(trackData, &TrackData::rgbWaveformUpdated, this, [this, deck] {
                refreshDeckFromEngine(deck);
            });
            m_overviewWaveformConnections[deck] = connect(trackData, &TrackData::overviewRgbUpdated, this, [this, deck] {
                m_lastWaveformRefreshMs[deck] = 0;
                refreshDeckFromEngine(deck);
            });
            m_dataClearedConnections[deck] = connect(trackData, &TrackData::dataCleared, this, [this, deck] {
                m_waveforms[deck].clear();
                m_waveformDurations[deck] = kPreviewDurationSeconds;
                m_uploadActive[deck] = false;
                m_uploadEntries[deck] = 0;
                m_lastCoverUrls[deck].clear();
                qInfo() << "[DDJ-FLX10] Deck" << deck << "has no track waveform; HID deck output stopped";
                if (m_connected)
                    clearDeckDisplay(deck);
            });
        }
    }
}

void DDJFLX10Controller::refreshDeckFromEngine(int deck)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_connected && m_lastWaveformRefreshMs[deck] > 0 && now - m_lastWaveformRefreshMs[deck] < 1500)
        return;
    m_lastWaveformRefreshMs[deck] = now;

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
    for (const QString& line : output.split('\n')) {
        const QString lower = line.toLower();
        if (!lower.contains(QStringLiteral("flx10"))
            && !lower.contains(QStringLiteral("ddj-flx10"))
            && !lower.contains(QStringLiteral("ddj"))) {
            continue;
        }

        const auto match = portPattern.match(line);
        if (match.hasMatch())
            return match.captured(1);
    }
    return {};
}

bool DDJFLX10Controller::sendAmidiSysEx(const QString& hex) const
{
    if (m_midiPort.isEmpty())
        return false;

    QProcess process;
    process.start(QStringLiteral("amidi"), {QStringLiteral("-p"), m_midiPort, QStringLiteral("-S"), hex});
    if (!process.waitForFinished(1500))
        return false;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void DDJFLX10Controller::sendSessionSysEx()
{
    sendAmidiSysEx(QString::fromLatin1(kSessionStart));
    sendAmidiSysEx(QString::fromLatin1(kEnterHid));
    for (int deck = 1; deck <= 4; ++deck)
        sendAmidiSysEx(QString::fromLatin1(kDeckInit[deck]));
    sendAmidiSysEx(QString::fromLatin1(kGlobalB));
    sendAmidiSysEx(QString::fromLatin1(kGlobalC));
}

void DDJFLX10Controller::sendKeepAlive()
{
    sendAmidiSysEx(QString::fromLatin1(kKeepAlive));
}

void DDJFLX10Controller::sendStateTick()
{
    if (!m_connected)
        return;

    if (m_waveforms[m_nextStateDeck].isEmpty()) {
        m_nextStateDeck = (m_nextStateDeck == 1) ? 2 : 1;
        return;
    }

    const DjEngine* engine = deckEngine(m_nextStateDeck);
    const double duration = std::max(1.0, m_waveformDurations[m_nextStateDeck]);
    const double elapsed = engine ? deckDisplayPosition(m_nextStateDeck)
                                  : std::fmod((QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0, duration);
    sendXx27(m_nextStateDeck,
             std::clamp(elapsed, 0.0, duration),
             duration,
             deckBpm(m_nextStateDeck),
             engine ? engine->isPlaying() : true);
    m_nextStateDeck = (m_nextStateDeck == 1) ? 2 : 1;
}

void DDJFLX10Controller::sendWaveformTick()
{
    if (!m_connected)
        return;

    for (int deck = 1; deck <= 2; ++deck)
        if (!m_waveforms[deck].isEmpty() && !m_uploadActive[deck])
            sendXx36Window(deck, m_waveforms[deck], currentWaveformEntry(deck));
}

void DDJFLX10Controller::sendUploadChunk()
{
    if (!m_connected) {
        m_uploadTimer.stop();
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
        if (packetIndex == 0) {
            const QString key = deckKey(deck).isEmpty() ? QStringLiteral("--") : deckKey(deck);
            const QByteArray label = QStringLiteral("KEY %2  BPM %1")
                .arg(deckBpm(deck), 0, 'f', 1)
                .arg(key)
                .left(28)
                .toLatin1();
            for (int i = 0; i < 28; ++i)
                put8(p, 7 + i, i < label.size() ? label.at(i) : 0);
        }
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
    QByteArray p = packet();
    put8(p, 0, deckByte(deck));
    put8(p, 1, 0x2F);
    put8(p, 2, 0x01);
    put8(p, 4, 0x01);
    return writePacket(p);
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

bool DDJFLX10Controller::sendXx27(int deck, double elapsedSeconds, double durationSeconds, double bpm, bool moving)
{
    const uint8_t db = deckByte(deck);
    QByteArray p = packet();
    put8(p, 0, db);
    put8(p, 1, 0x27);
    put8(p, 2, 0xB4);
    put8(p, 3, 0x80);
    put8(p, 4, 0x01);

    elapsedSeconds = std::max(0.0, elapsedSeconds);
    const int totalMs = static_cast<int>(elapsedSeconds * 1000.0);
    const int minutes = totalMs / 60000;
    const int rem = totalMs % 60000;
    const int seconds = rem / 1000;
    const int ms = rem % 1000;
    put8(p, 5, minutes);
    put8(p, 6, seconds);
    put8(p, 7, ms);
    put8(p, 8, (ms >> 8) & 0x03);

    const double remaining = std::max(0.0, durationSeconds - elapsedSeconds);
    const int remainingMs = static_cast<int>(remaining * 1000.0);
    put8(p, 9, remainingMs / 60000);
    const int rem2 = remainingMs % 60000;
    put8(p, 10, rem2 / 1000);
    const int ms2 = rem2 % 1000;
    put8(p, 11, ms2);
    put8(p, 12, ms2 >> 8);

    const int bpmInt = static_cast<int>(bpm);
    put8(p, 13, bpmInt);
    put8(p, 14, (static_cast<int>(std::round((bpm - bpmInt) * 10.0)) & 0x0F) << 4);
    put8(p, 15, 0x01);
    put8(p, 20, 0x0E);

    if (moving) {
        const int secInt = totalMs / 1000;
        const int subMs = totalMs % 1000;
        const int sub1024 = std::min(1023, (subMs * 1024) / 1000);
        const int ticks = secInt * 1024 + sub1024;
        put8(p, 21, ticks * 2);
        put8(p, 22, static_cast<int>(ticks / 123.2) % 15);
    } else {
        put8(p, 21, 0x02);
        put8(p, 22, 0x00);
    }

    put8(p, 25, 0x80);
    put8(p, 29, 0x92);
    put8(p, 30, 0x0D);
    put8(p, 31, displayDeckState(db));
    put8(p, 32, 0xFF);
    put8(p, 33, 0xFF);
    put8(p, 34, 0xFF);
    return writePacket(p);
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

    const int targetEntries = std::clamp(static_cast<int>(std::ceil(deckDisplayDuration(deck) * 150.0)), 150, kMaxWaveformEntries);
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
    const DjEngine* engine = deckEngine(deck);
    if (!engine || engine->getDuration() <= 0.0f)
        return kPreviewDurationSeconds;

    return std::max(1.0, static_cast<double>(engine->getDuration()));
}

double DDJFLX10Controller::deckDisplayPosition(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine)
        return 0.0;

    return std::clamp(engine->getPlayheadPositionAtomic(), 0.0, deckDisplayDuration(deck));
}

double DDJFLX10Controller::deckBpm(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine || engine->getCurrentBpm() <= 0.0)
        return 0.0;
    return engine->getCurrentBpm();
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

int DDJFLX10Controller::currentWaveformEntry(int deck) const
{
    const QByteArray& waveform = m_waveforms[deck];
    const int entries = waveform.size() / 2;
    if (entries <= 19 || m_clockStartMs <= 0)
        return 0;

    const DjEngine* engine = deck == 1 ? m_deckA : m_deckB;
    if (engine && engine->getDuration() > 0.0f) {
        const double fraction = std::clamp(engine->getPlayheadPositionAtomic() / static_cast<double>(engine->getDuration()), 0.0, 1.0);
        return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
    }

    const double elapsed = (QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0;
    const double fraction = std::fmod(std::max(0.0, elapsed), m_waveformDurations[deck]) / m_waveformDurations[deck];
    return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
}
