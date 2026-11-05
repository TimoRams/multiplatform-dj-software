#pragma once
#include "SamplerAudioEngine.h"
#include "SamplerMidiPickup.h"
#include <QTimer>
#include "app/ControlClock.h"
#include <QObject>
#include <QVariantList>
#include <QFutureWatcher>
#include <QString>
#include <array>

class DjEngine;
class LinkManager;
namespace engine::sync { class SyncCoordinator; }

class SamplerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int bank READ bank WRITE setBank NOTIFY changed)
    Q_PROPERTY(int selectedSlot READ selectedSlot WRITE setSelectedSlot NOTIFY changed)
    Q_PROPERTY(QVariantList slots READ slotList NOTIFY changed)
    Q_PROPERTY(double gain READ gain WRITE setGain NOTIFY changed)
    Q_PROPERTY(bool cue READ cue WRITE setCue NOTIFY changed)
    Q_PROPERTY(int quantize READ quantize WRITE setQuantize NOTIFY changed)
    Q_PROPERTY(QString clockSource READ clockSource NOTIFY changed)
    Q_PROPERTY(double bpm READ bpm NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantMap fileInfo READ fileInfo NOTIFY fileInfoChanged)
    Q_PROPERTY(QVariantList playback READ playback NOTIFY playbackChanged)
    Q_PROPERTY(QVariantList bankActivity READ bankActivity NOTIFY playbackChanged)
    Q_PROPERTY(bool midiPickupPending READ midiPickupPending NOTIFY changed)
    Q_PROPERTY(bool previewAvailable READ previewAvailable NOTIFY previewChanged)
    Q_PROPERTY(bool previewPlaying READ previewPlaying NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap previewPlayback READ previewPlayback NOTIFY previewChanged)
    Q_PROPERTY(bool editorBusy READ editorBusy NOTIFY editorChanged)
    Q_PROPERTY(QString editorError READ editorError NOTIFY editorChanged)
public:
    QVariantList playback() const;
    QVariantList bankActivity() const;
    bool midiPickupPending() const { return m_midiGainSeen && m_midiPickup.pending(); }
    void setMidiGainNormalized(double value);
    void resetMidiPickup();
    bool previewAvailable() const;
    bool previewPlaying() const { return m_audio.previewPlaying(); }
    bool previewLoading() const { return m_previewLoading; }
    QVariantMap previewPlayback() const;
    bool editorBusy() const { return m_editorSaving || m_editorLoading; }
    QString editorError() const { return m_editorError; }
    Q_INVOKABLE QVariantMap beginEditor(int pad);
    Q_INVOKABLE void cancelEditor(qulonglong session);
    Q_INVOKABLE void setEditorFile(qulonglong session, const QString& path);
    Q_INVOKABLE void captureEditor(qulonglong session, int deck, int beats);
    Q_INVOKABLE void applyEditor(qulonglong session, const QVariantMap& draft);
    Q_INVOKABLE void clearEditor(qulonglong session);
    Q_INVOKABLE void previewEditor(qulonglong session, const QVariantMap& draft);
    Q_INVOKABLE void stopEditorPreview();
    Q_INVOKABLE void requestEditorWaveform(qulonglong session, double start, double end);
    SamplerController(SamplerAudioEngine& audio, ControlClock& clock,
                      const QString& storageDirectory, QObject* parent = nullptr);
    ~SamplerController() override;
    int bank() const { return m_bank; }
    int selectedSlot() const { return m_selectedSlot; }
    double gain() const { return m_gain; }
    bool cue() const { return m_cue; }
    int quantize() const { return m_quantize; }
    QString clockSource() const { return m_clockSource; }
    double bpm() const { return m_clock.bpm; }
    QString error() const { return m_error; }
    QVariantList slotList() const;
    QVariantMap fileInfo() const { return m_fileInfo; }
    void setBank(int bank);
    void setSelectedSlot(int index);
    void setGain(double gain);
    void setCue(bool enabled);
    void setQuantize(int beats);
    void attachDecks(const std::array<DjEngine*, 4>& decks,
                     engine::sync::SyncCoordinator* sync, LinkManager* link);
    Q_INVOKABLE void trigger(int pad);
    Q_INVOKABLE void stop(int pad);
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void clear(int pad);
    Q_INVOKABLE void loadFile(int pad, const QString& path, double startSec = 0,
                             double endSec = -1, double sourceBpm = 0, bool loop = false, bool sync = false,
                             const QString& name = {}, const QString& color = {}, double gain = 1);
    Q_INVOKABLE void captureDeck(int pad, int deck, int beats = 4);
    Q_INVOKABLE void edit(int pad, const QString& name, const QString& color,
                          double gain, bool loop, bool sync, double bpm,
                          double startSec, double endSec);
    Q_INVOKABLE void probeFile(const QString& path);
    void shutdown();
signals:
    void changed();
    void playbackChanged();
    void previewChanged();
    void editorChanged();
    void editorSourceReady(qulonglong session, const QVariantMap& info);
    void editorWaveformReady(qulonglong session, double start, double end, const QVariantList& peaks);
    void editorFinished(qulonglong session, bool success, const QString& error);
    void fileInfoChanged();
private:
    struct Slot {
        QString asset, name, color = "#e0a344", error;
        std::uint64_t pcmBytes = 0;
        double duration = 0, sampleRate = 44100, bpm = 0, gain = 1, start = 0, end = 0;
        bool loop = false, sync = false, loading = false;
        std::uint64_t generation = 0;
        QVariantList waveform;
    };
    struct Loaded {
        std::unique_ptr<SamplerSample> sample;
        QString asset, error;
        double duration = 0, assetDuration = 0;
        QVariantList waveform;
    };
    void load(int index, const QString& path, double start, double end,
              bool imported, Slot desired, std::uint64_t editorSession = 0);
    bool clearSlot(int index);
    void refreshPlayback();
    void editorProbeFinished(std::uint64_t probeGeneration);
    void finishEditorSave(std::uint64_t session, bool success, const QString& error);
    bool validateEditor(std::uint64_t session, const QVariantMap& draft);
    bool validEditor(std::uint64_t session) const;
    void setEditorError(const QString& error);
    bool save();
    void restore();
    void tick();
    void fail(const QString& message);
    static QString localPath(const QString& path);
    SamplerAudioEngine& m_audio;
    QTimer m_gainSaveTimer;
    SamplerMidiPickup m_midiPickup;
    bool m_applyingMidiGain = false, m_midiGainSeen = false;
    std::array<SamplerAudioEngine::PlaybackSnapshot, 32> m_playbackCache {};
    std::array<int, 32> m_lastStates {};
    double m_lastUiBpm = 0;
    QString m_lastUiClock;
    std::uint64_t m_nextEditorSession = 0, m_editorSession = 0, m_editorGeneration = 0;
    std::uint64_t m_editorProbeGeneration = 0, m_previewGeneration = 0, m_waveGeneration = 0;
    int m_editorSlot = -1;
    QString m_editorSource, m_editorError;
    double m_editorSourceDuration = 0, m_editorSourceRate = 44100;
    bool m_editorSaving = false, m_editorLoading = false, m_previewLoading = false;
    QVariantMap m_editorCapture;
    std::shared_ptr<std::atomic<bool>> m_previewCancel, m_waveCancel;
    std::shared_ptr<std::atomic<std::uint64_t>> m_budget = std::make_shared<std::atomic<std::uint64_t>>(0);
    ControlClock::Registration m_registration;
    QString m_directory, m_error, m_clockSource = "INTERNAL";
    std::array<Slot, 32> m_slots;
    std::array<DjEngine*, 4> m_decks {};
    engine::sync::SyncCoordinator* m_sync = nullptr;
    LinkManager* m_link = nullptr;
    std::vector<QFutureWatcher<std::shared_ptr<Loaded>>*> m_jobs;
    QVariantMap m_fileInfo;
    std::vector<QFutureWatcher<QVariantMap>*> m_probes;
    std::uint64_t m_probeGeneration = 0;
    int m_bank = 0, m_selectedSlot = 0, m_quantize = 1;
    double m_gain = 0.72;
    bool m_cue = false, m_stopped = false;
    SamplerBeatClock m_clock;
};
