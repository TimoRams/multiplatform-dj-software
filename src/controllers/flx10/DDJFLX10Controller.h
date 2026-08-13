#pragma once

#include "controllers/midi/AlsaMidiOutput.h"
#include "app/ControlClock.h"
#include "Flx10Protocol.h"

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <QPointer>
#include "deck/DjEngine.h"

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
#include <libusb-1.0/libusb.h>
#endif

class DDJFLX10Controller : public QObject
{
    Q_OBJECT

public:
    explicit DDJFLX10Controller(ControlClock& controlClock, QObject* parent = nullptr);
    ~DDJFLX10Controller() override;

    bool start();
    void stop();
    void prepareForShutdown() noexcept;
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
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    void startHidWriter();
    void stopHidWriter() noexcept;
    void hidWriterLoop();
    void reportHidWriteFailure();
    void discardQueuedDeckPackets(int deck);
#endif

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
    bool sendXx35(int deck);
    bool sendXx36Window(int deck, const QByteArray& waveform, int entry);
    bool sendXx2f(int deck);
    bool sendXx27(int deck, const flx10_protocol::DeckDisplaySnapshot& snapshot);
    void resetDisplayPacketState(int deck);
    void pushDeckJogDisplay(int deck);
    bool clearDeckDisplay(int deck);

    QByteArray generateCoverJpeg(int deck) const;
    bool uploadCoverArt(int deck);
    struct WaveformPreviewRenderInfo final {
        std::uint64_t trackGeneration = 0;
        std::uint64_t dataGeneration = 0;
        std::uint32_t sourceLineBegin = 0;
        std::uint32_t sourceLineEnd = 0;
        std::uint32_t outputWidth = 0;
        std::uint32_t generatedColumns = 0;
        std::uint32_t columnsWithData = 0;
        std::uint32_t completeColumns = 0;
        std::uint32_t playheadChunkIndex = 0;
        std::uint8_t playheadChunkState = 0;
        // No LOD fields: the FLX10 path asks waveform::aggregateWaveformColumn
        // for a source-line range and never learns which level answered it.
    };

    QByteArray generatePreviewWaveform(int deck,
                                       WaveformPreviewRenderInfo* outInfo = nullptr) const;
    // Full-track PWV5 generation is O(duration x 150) aggregate calls. Running
    // it on the owner thread every 250 ms while analysis streamed in was what
    // froze the jog handle: it stalled the ControlClock display callback, so
    // xx27 state packets stopped flowing. It now runs on a dedicated worker
    // and the result is posted back.
    void requestPreviewWaveform(int deck);
    void onPreviewWaveformReady(int deck, const QByteArray& waveform,
                                std::uint64_t trackGeneration,
                                std::uint32_t generatedColumns,
                                std::uint32_t completeColumns);
    void startWaveformWorker();
    void stopWaveformWorker() noexcept;
    void waveformWorkerLoop();

    struct PendingWaveformJob final {
        int deck = 0;
        std::shared_ptr<const WaveformLineStoreSnapshot> snapshot;
        int targetEntries = 0;
        std::uint64_t trackGeneration = 0;
    };
    std::mutex m_waveformJobMutex;
    std::condition_variable m_waveformJobCondition;
    // Latest-wins per deck: an newer analysis state supersedes a queued job.
    std::array<std::optional<PendingWaveformJob>, 5> m_pendingWaveformJobs;
    std::thread m_waveformWorker;
    bool m_waveformWorkerStopping = false;
    int currentWaveformEntry(int deck) const;
    flx10_protocol::DeckDisplaySnapshot captureDeckDisplaySnapshot(int deck);
    double deckDisplayDuration(int deck) const;
    double deckDisplayPosition(int deck) const;
    double deckTempoRangePercent(int deck) const;
    QString deckKey(int deck) const;
    uint8_t deckKeyByte(int deck) const;
    std::vector<double> deckBeatTimesMs(int deck) const;
    DjEngine* deckEngine(int deck) const;
    void connectDeckSignals();
    void disconnectDeckSignals();
    void refreshDeckFromEngine(int deck);
    void resetDeckWaveformOutput(int deck);
    void invalidateDeckSnapshot(int deck, const QString& trackPath, bool clearDevice);

    QString m_status;
    QString m_midiPort;
#if defined(Q_OS_LINUX)
    std::unique_ptr<AlsaMidiOutput> m_sequencerMidiOut;
    std::unique_ptr<QProcess> m_keepAliveProcess;
#endif
    ControlClock::Registration m_clockRegistration;
    QTimer m_uploadTimer;
    QTimer m_keepAliveTimer;
    bool m_keepAliveEnabled = false;
    std::array<QByteArray, 5> m_waveforms;
    std::array<double, 5> m_waveformDurations = {0.0, 30.0, 30.0, 30.0, 30.0};
    // Number of 19-entry xx36 windows already pushed for this deck's bulk
    // upload. The window actually sent is derived from this plus the playhead,
    // so filling always starts at what is about to be played.
    std::array<int, 5> m_uploadWindowsSent = {0, 0, 0, 0, 0};
    std::array<bool, 5> m_uploadActive = {false, false, false, false, false};
    std::array<QMetaObject::Connection, 5> m_trackLoadedConnections;
    std::array<QMetaObject::Connection, 5> m_trackEjectedConnections;
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
    std::array<QMetaObject::Connection, 5> m_progressConnections;
    std::array<QString, 5> m_lastCoverUrls;
    std::array<QString, 5> m_waveformTrackPaths;
    std::array<qint64, 5> m_lastWaveformRefreshMs = {0, 0, 0, 0, 0};
    std::array<qint64, 5> m_lastWaveformUploadMs = {0, 0, 0, 0, 0};
    std::array<std::uint64_t, 5> m_waveformUploadTrackGenerations = {0, 0, 0, 0, 0};
    std::array<std::uint32_t, 5> m_waveformUploadQualityPermille = {0, 0, 0, 0, 0};
    std::array<bool, 5> m_jogRingWarningActive = {false, false, false, false, false};
    std::array<bool, 5> m_jogRingLit = {true, true, true, true, true};
    qint64 m_clockStartMs = 0;
    QElapsedTimer m_hidTrafficClock;
    std::array<qint64, 5> m_lastXx27SentMs = {-100, -100, -100, -100, -100};
    qint64 m_lastXx36SentMs = -100;
    std::array<QByteArray, 5> m_lastXx27Packet;
    std::array<std::uint64_t, 5> m_displaySnapshotSequence {0, 0, 0, 0, 0};
    int m_nextWaveformDeck = 1;
    int m_nextUploadDeck = 1;
    std::atomic<bool> m_shuttingDown { false };
    bool m_connected = false;
    QPointer<DjEngine> m_deckA;
    QPointer<DjEngine> m_deckB;

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    libusb_context* m_usbContext = nullptr;
    libusb_device_handle* m_handle = nullptr;
    uint8_t m_outEndpoint = 0;
    bool m_interfaceClaimed = false;
    std::mutex m_hidWriteMutex;
    std::condition_variable m_hidWriteCondition;
    std::deque<QByteArray> m_hidWriteQueue;
    flx10_protocol::LatestDisplayPacketSlots m_latestHidDisplayPackets;
    std::thread m_hidWriter;
    bool m_hidWriterStopping = false;
    bool m_hidWriterRunning = false;
    std::atomic<bool> m_hidWriteHealthy { true };
    std::atomic<bool> m_hidWriteFailurePending { false };
    std::atomic<int> m_hidWriteError { 0 };
    std::atomic<int> m_hidWriteTransferred { 0 };
    std::atomic<bool> m_hidStateRefreshPending { false };
    bool m_hidDiagnosticsEnabled = false;
    std::atomic<std::uint64_t> m_displaySnapshotsPublished { 0 };
    std::atomic<std::uint64_t> m_displayFramesSent { 0 };
    std::atomic<std::uint64_t> m_displayFramesCoalesced { 0 };
    std::atomic<std::uint64_t> m_displayWriteFailures { 0 };
    std::atomic<std::uint64_t> m_displayWriteTimeouts { 0 };
    std::atomic<std::uint64_t> m_maximumDisplayQueueDepth { 0 };
    std::atomic<std::uint64_t> m_lastDisplaySequence { 0 };
    std::atomic<std::uint64_t> m_worstDisplayWriteUsec { 0 };
#endif
};
