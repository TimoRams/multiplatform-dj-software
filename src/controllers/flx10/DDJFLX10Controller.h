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
    void flushTempoWaveformRefresh();

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

    bool uploadDeck(int deck, bool startWindowSweep = true);
    bool sendXx30(int deck);
    bool sendXx39(int deck);
    bool sendXx33Album(int deck, const QByteArray& jpeg);
    bool sendXx35(int deck);
    bool sendXx36Window(int deck, const QByteArray& waveform, int entry);
    bool sendXx2f(int deck);
    // Display thread only. nowMs comes from that thread's own clock so the
    // rate limiter never has to read a timer the owner thread restarts.
    bool sendXx27(int deck, const flx10_protocol::DeckDisplaySnapshot& snapshot,
                  qint64 nowMs);
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

    // Slow-changing half of the jog-screen state: everything that only moves on
    // a user action or a track load. The playhead is deliberately absent —
    // the display thread reads that straight out of the deck's atomic, so the
    // screens keep moving even while the owner thread is stalled.
    struct DeckStateFeed final {
        double trackDurationSec = flx10_protocol::kPreviewDurationSeconds;
        double bpm = 0.0;
        double tempoPercent = 0.0;
        double latencyCompensationSec = 0.0;
        bool playing = false;
        bool scratching = false;
        bool reverse = false;
        bool hasEngine = false;
        std::uint8_t keyByte = 0x80;
    };
    // Published by the owner thread, consumed by the display thread. The lock is
    // held only across a copy of the struct above, so neither side can be made
    // to wait on the other doing anything else.
    struct DeckStateCell final {
        mutable std::mutex mutex;
        DeckStateFeed value;
        // Set when something invalidated the device's idea of this deck, so the
        // display thread re-sends even a byte-identical packet.
        std::atomic<bool> forceResend {false};
    };
    std::array<DeckStateCell, 5> m_deckStateCells;
    [[nodiscard]] DeckStateFeed captureDeckStateFeed(int deck) const;
    void publishDeckState(int deck);
    [[nodiscard]] DeckStateFeed readDeckState(int deck) const;

    // xx27 production used to run on the owner thread's 5 ms timer. Any stall
    // there — a QML frame, a library scroll, a waveform repaint — stopped the
    // state stream, the HID writer ran out of new packets and the jog screens
    // froze. Production now runs here instead, on a thread that touches no Qt
    // object and no engine state beyond one atomic load per deck.
    void startDisplayThread();
    void stopDisplayThread() noexcept;
    void displayThreadLoop();
    std::thread m_displayThread;
    std::atomic<bool> m_displayThreadStopping {true};
    std::atomic<bool> m_displayFeedActive {false};
    // Captured from the decks so the display thread never dereferences a
    // QPointer. Cleared only once the thread has been joined, never while it
    // may still be running.
    std::array<std::atomic<const std::atomic<double>*>, 5> m_deckPlayheadSinks;
    void captureDeckPlayheadSinks();
    // Owned exclusively by the display thread.
    std::array<QByteArray, 5> m_displayLastPacket;
    std::array<qint64, 5> m_displayLastSentMs = {-100, -100, -100, -100, -100};
    // Telemetry: ticks the display thread ran, and ticks that arrived late
    // enough to have missed a slot. Counters only; never logged from the loop.
    std::atomic<std::uint64_t> m_displayTicks {0};
    std::atomic<std::uint64_t> m_displayTicksLate {0};

    double deckDisplayDuration(int deck) const;
    double deckDisplayPosition(int deck) const;
    double deckTempoRangePercent(int deck) const;
    QString deckKey(int deck) const;
    uint8_t deckKeyByte(int deck) const;
    DjEngine* deckEngine(int deck) const;
    void connectDeckSignals();
    void disconnectDeckSignals();
    void refreshDeckFromEngine(int deck);
    void scheduleTempoWaveformRefresh(int deck);
    void refreshWaveformMarkersAndUpload(int deck);
    void decorateWaveformMarkers(int deck, QByteArray& waveform) const;
    void beginWaveformSweep(int deck);
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
    QTimer m_stateTimer;
    QTimer m_tempoWaveformRefreshTimer;
    bool m_keepAliveEnabled = false;
    std::array<QByteArray, 5> m_waveforms;
    // Marker-free worker output. Hot-cue/loop changes rebuild the decorated
    // waveform from this copy so deleting a marker cannot leave a stale stripe.
    std::array<QByteArray, 5> m_baseWaveforms;
    std::array<double, 5> m_waveformDurations = {0.0, 30.0, 30.0, 30.0, 30.0};
    // Number of 19-entry xx36 windows already pushed for this deck's bulk
    // upload. The playhead selects the origin once when the sweep begins; that
    // origin then stays fixed until all indexed windows have been transferred.
    std::array<int, 5> m_uploadWindowsSent = {0, 0, 0, 0, 0};
    // A sweep is an indexed transfer. Its origin must not follow the moving
    // playhead between packets or entries are skipped and the firmware shows
    // blank waveform regions after the cursor.
    std::array<int, 5> m_uploadStartWindows = {0, 0, 0, 0, 0};
    std::array<bool, 5> m_uploadActive = {false, false, false, false, false};
    // Set when better waveform data arrived while a transfer was already in
    // flight. Windows sent before that point still carry the old samples, so
    // the sweep is repeated once it reaches the end instead of being rewound
    // mid-flight.
    std::array<bool, 5> m_uploadResweepPending = {false, false, false, false, false};
    std::uint8_t m_pendingTempoWaveformDeckMask = 0;
    std::array<QMetaObject::Connection, 5> m_trackLoadedConnections;
    std::array<QMetaObject::Connection, 5> m_trackEjectedConnections;
    std::array<QMetaObject::Connection, 5> m_rgbWaveformConnections;
    std::array<QMetaObject::Connection, 5> m_overviewWaveformConnections;
    std::array<QMetaObject::Connection, 5> m_dataClearedConnections;
    std::array<QMetaObject::Connection, 5> m_metadataConnections;
    std::array<QMetaObject::Connection, 5> m_keyAnalyzedConnections;
    std::array<QMetaObject::Connection, 5> m_bpmAnalyzedConnections;
    std::array<QMetaObject::Connection, 5> m_beatgridConnections;
    std::array<QMetaObject::Connection, 5> m_hotCueConnections;
    std::array<QMetaObject::Connection, 5> m_loopConnections;
    std::array<QMetaObject::Connection, 5> m_savedLoopConnections;
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
    std::array<bool, 5> m_jogRingLit = {false, false, false, false, false};
    std::array<bool, 5> m_jogRingStateKnown = {false, false, false, false, false};
    qint64 m_clockStartMs = 0;
    QElapsedTimer m_hidTrafficClock;
    std::array<qint64, 5> m_lastXx36SentMs = {-100, -100, -100, -100, -100};
    // Avoid reading TrackData (and taking its locks) on every 5 ms display
    // tick. Key changes are signal-driven and update this cache immediately.
    std::array<std::uint8_t, 5> m_cachedDeckKeyBytes = {
        0x80, 0x80, 0x80, 0x80, 0x80
    };
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
