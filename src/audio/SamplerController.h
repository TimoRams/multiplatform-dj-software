#pragma once
#include "SamplerAudioEngine.h"
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
public:
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
              bool imported, Slot desired);
    bool save();
    void restore();
    void tick();
    void fail(const QString& message);
    static QString localPath(const QString& path);
    SamplerAudioEngine& m_audio;
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
