#pragma once

#include "midi/AlsaMidiOutput.h"

#include <QObject>
#include <QByteArray>
#include <QMetaObject>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class DjEngine;

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
#include <libusb-1.0/libusb.h>
#endif

class DDJFLX10Controller : public QObject
{
    Q_OBJECT

public:
    explicit DDJFLX10Controller(QObject* parent = nullptr);
    ~DDJFLX10Controller() override;

    bool start();
    void stop();
    void setDecks(DjEngine* deckA, DjEngine* deckB);

    bool isConnected() const { return m_connected; }
    QString status() const { return m_status; }

signals:
    void statusChanged();
    void connectedChanged();

private slots:
    void sendKeepAlive();
    void sendStateTick();
    void sendWaveformTick();
    void sendUploadChunk();

private:
    void setStatus(const QString& status);
    void setConnected(bool connected);

    bool initialiseUsb();
    bool sendVendorUnlock();
    bool claimScreenInterface();
    bool writePacket(const QByteArray& packet);

    QString findMidiPort() const;
    bool openSequencerMidiPort();
    bool sendMidiHex(const QString& hex) const;
    void sendSessionSysEx();
    void updateJogRingWarning(int deck, double elapsedSeconds, double durationSeconds, bool playing);
    bool sendJogRingIllumination(int deck, bool on);

    bool uploadDeck(int deck);
    bool sendXx30(int deck);
    bool sendXx39(int deck);
    bool sendXx33Album(int deck, const QByteArray& jpeg);
    bool sendXx35(int deck, int entryCount);
    bool sendXx36Window(int deck, const QByteArray& waveform, int entry);
    bool sendXx2f(int deck);
    bool sendXx27(int deck, double fileElapsedSeconds, double durationSeconds, double bpm, bool moving);
    double smoothFileElapsedSec(int deck, double fileElapsedSec, double rateRatio, bool playing);
    void resetDisplayInterp(int deck);
    bool clearDeckDisplay(int deck);

    QByteArray generateCoverJpeg(int deck) const;
    bool uploadCoverArt(int deck);
    QByteArray generatePreviewWaveform(int deck) const;
    int currentWaveformEntry(int deck) const;
    double deckDisplayDuration(int deck) const;
    double deckDisplayPosition(int deck) const;
    double deckBpm(int deck) const;
    double deckTempoPercent(int deck) const;
    double deckTempoRangePercent(int deck) const;
    QString deckKey(int deck) const;
    uint8_t deckKeyByte(int deck) const;
    std::vector<double> deckBeatTimesMs(int deck) const;
    DjEngine* deckEngine(int deck) const;
    void connectDeckSignals();
    void refreshDeckFromEngine(int deck);

    QString m_status;
    QString m_midiPort;
#if defined(Q_OS_LINUX)
    std::unique_ptr<AlsaMidiOutput> m_sequencerMidiOut;
#endif
    QTimer m_keepAliveTimer;
    QTimer m_stateTimer;
    QTimer m_waveformTimer;
    QTimer m_uploadTimer;
    std::array<QByteArray, 5> m_waveforms;
    std::array<double, 5> m_waveformDurations = {0.0, 30.0, 30.0, 30.0, 30.0};
    std::array<int, 5> m_uploadEntries = {0, 0, 0, 0, 0};
    std::array<bool, 5> m_uploadActive = {false, false, false, false, false};
    std::array<QMetaObject::Connection, 5> m_trackLoadedConnections;
    std::array<QMetaObject::Connection, 5> m_rgbWaveformConnections;
    std::array<QMetaObject::Connection, 5> m_overviewWaveformConnections;
    std::array<QMetaObject::Connection, 5> m_dataClearedConnections;
    std::array<QMetaObject::Connection, 5> m_metadataConnections;
    std::array<QMetaObject::Connection, 5> m_keyAnalyzedConnections;
    std::array<QMetaObject::Connection, 5> m_beatgridConnections;
    std::array<QMetaObject::Connection, 5> m_hotCueConnections;
    std::array<QMetaObject::Connection, 5> m_tempoConnections;
    std::array<QMetaObject::Connection, 5> m_tempoRangeConnections;
    std::array<QMetaObject::Connection, 5> m_scrubbingConnections;
    std::array<QString, 5> m_lastCoverUrls;
    std::array<qint64, 5> m_lastWaveformRefreshMs = {0, 0, 0, 0, 0};
    std::array<bool, 5> m_jogRingWarningActive = {false, false, false, false, false};
    std::array<bool, 5> m_jogRingLit = {true, true, true, true, true};
    qint64 m_clockStartMs = 0;
    struct DeckDisplayInterp {
        double lastFilePos = -1.0;
        qint64 lastPosTimeMs = 0;
        qint64 lastNewPosTimeMs = 0;
        double lastSmoothFileMs = 0.0;
        bool initialized = false;
    };
    std::array<DeckDisplayInterp, 5> m_displayInterp{};
    std::array<QByteArray, 5> m_lastXx27Packet;
    bool m_connected = false;
    DjEngine* m_deckA = nullptr;
    DjEngine* m_deckB = nullptr;

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    libusb_context* m_usbContext = nullptr;
    libusb_device_handle* m_handle = nullptr;
    uint8_t m_outEndpoint = 0;
    bool m_interfaceClaimed = false;
#endif
};
