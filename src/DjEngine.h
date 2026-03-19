#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QVariantList>
#include <QElapsedTimer>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "TrackData.h"
#include "WaveformAnalyzer.h"
#include "FxProcessor.h"

class CoverArtProvider;
class LibraryDatabase;

class DjEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float progress READ getProgress NOTIFY progressChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool isReverse READ isReverse NOTIFY reverseChanged)
    Q_PROPERTY(bool keylock READ keylock WRITE setKeylock NOTIFY keylockChanged)
    Q_PROPERTY(double tempoPercent READ getTempoPercent WRITE setTempoPercent NOTIFY tempoChanged)
    Q_PROPERTY(double currentBpm READ getCurrentBpm NOTIFY tempoChanged)
    Q_PROPERTY(double tempoRatio READ getTempoRatio NOTIFY tempoChanged)
    Q_PROPERTY(TrackData* trackData READ getTrackData CONSTANT)
    Q_PROPERTY(bool quantizeEnabled READ quantizeEnabled WRITE setQuantizeEnabled NOTIFY quantizeEnabledChanged)
    Q_PROPERTY(bool syncEnabled READ syncEnabled WRITE setSyncEnabled NOTIFY syncChanged)
    Q_PROPERTY(bool syncMaster READ isSyncMaster NOTIFY syncMasterChanged)
    Q_PROPERTY(bool loopActive READ loopActive NOTIFY loopChanged)
    Q_PROPERTY(bool loopInSet READ loopInSet NOTIFY loopChanged)
    Q_PROPERTY(double loopLengthBeats READ loopLengthBeats NOTIFY loopChanged)
    Q_PROPERTY(double loopInPosition READ loopInPosition NOTIFY loopChanged)
    Q_PROPERTY(double loopOutPosition READ loopOutPosition NOTIFY loopChanged)

    Q_PROPERTY(QString trackTitle   READ trackTitle   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackArtist  READ trackArtist  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackAlbum   READ trackAlbum   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackKey     READ trackKey     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackDuration READ trackDuration NOTIFY trackMetadataChanged)
    Q_PROPERTY(double trackDurationSec READ trackDurationSec NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasTrack     READ hasTrack     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString coverArtUrl  READ coverArtUrl  NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasCoverArt  READ hasCoverArt  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QVariantList currentSegments READ currentSegments NOTIFY segmentsChanged)

    // Mixer Properties
    // Pixels per second at current zoom — needed by the scrub math in QML.
    // 150 waveform-points/sec × pixelsPerPoint (written from QML via setPixelsPerPoint).
    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY pixelsPerSecondChanged)

    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(double trim READ trim WRITE setTrim NOTIFY trimChanged)
    Q_PROPERTY(double eqHigh READ eqHigh WRITE setEqHigh NOTIFY eqHighChanged)
    Q_PROPERTY(double eqMid READ eqMid WRITE setEqMid NOTIFY eqMidChanged)
    Q_PROPERTY(double eqLow READ eqLow WRITE setEqLow NOTIFY eqLowChanged)
    Q_PROPERTY(double filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool cueEnabled READ cueEnabled WRITE setCueEnabled NOTIFY cueEnabledChanged)

    // VU meter peak levels (0.0-1.0+), read from the audio thread
    Q_PROPERTY(float vuLevelL READ vuLevelL NOTIFY vuLevelChanged)
    Q_PROPERTY(float vuLevelR READ vuLevelR NOTIFY vuLevelChanged)
    Q_PROPERTY(bool clipDetected READ clipDetected NOTIFY vuLevelChanged)
    
    // Global anti-clip gain reduction (0.0-1.0), 1.0 = no reduction
    Q_PROPERTY(float gainReduction READ gainReduction NOTIFY gainReductionChanged)

public:
    explicit DjEngine(QObject* parent = nullptr);
    ~DjEngine() override;

    float getProgress() const;
    Q_INVOKABLE float getDuration() const;
    float getPosition() const;
    // Latency-compensated position in seconds, used by the waveform renderer.
    float getVisualPosition() const;
    // Lock-free atomic read of the playhead position (seconds).
    // Called from QML FrameAnimation every VSync frame — must be wait-free.
    Q_INVOKABLE double getPlayheadPositionAtomic() const;
    bool isPlaying() const;

    // Pixels-per-second scale mirrored from the waveform renderer so scrubBy()
    // can convert mouse pixels → audio seconds without needing QML math.
    double pixelsPerSecond() const { return m_pixelsPerSecond; }

    // Universal scratch API used by jogwheel and scrolling waveform.
    // pauseForScrub() captures current play state and enters scratch mode.
    // scrubBy() and scratchBySeconds() move the playhead with audible output.
    // resumeAfterScrub() restores pre-scratch transport state.
    Q_INVOKABLE void pauseForScrub();
    Q_INVOKABLE void scrubBy(double pixelDelta);
    Q_INVOKABLE void scratchBySeconds(double deltaSeconds);
    Q_INVOKABLE void resumeAfterScrub();

    // Manual beat-grid correction: rebuilds the BeatMarker array so that the
    // current playhead position becomes beat 1 / bar 1.  Emits beatgridChanged
    // via TrackData, which the waveform renderer picks up automatically.
    Q_INVOKABLE void setDownbeatAtCurrentPosition();

    // Half-time / double-time correction.
    // Finds the nearest existing downbeat to the current playhead as a stable
    // anchor, doubles/halves the stored BPM, then rebuilds the grid from there.
    Q_INVOKABLE void doubleBpm();
    Q_INVOKABLE void halveBpm();

    // Beatgrid-aligned loop controls
    Q_INVOKABLE void setLoopIn();
    Q_INVOKABLE void setLoopOut();
    Q_INVOKABLE void toggleLoop4Beats();
    Q_INVOKABLE void toggleLoopThreeQuarter();
    Q_INVOKABLE void halveLoopLength();
    Q_INVOKABLE void doubleLoopLength();
    Q_INVOKABLE void clearLoop();

    // Master volume + anti-clip (global, shared across all decks)
    Q_INVOKABLE void setMasterVolume(float v);
    Q_INVOKABLE void setAntiClip(bool enabled);
    TrackData* getTrackData() const;

    QString trackTitle()    const { return m_trackTitle; }
    QString trackArtist()   const { return m_trackArtist; }
    QString trackAlbum()    const { return m_trackAlbum; }
    QString trackKey()      const { return m_trackKey; }
    QString trackDuration() const { return m_trackDuration; }
    double  trackDurationSec() const { return m_trackDurationSec; }
    bool    hasTrack()      const { return m_hasTrack; }
    QString coverArtUrl()   const { return m_coverArtUrl; }
    bool    hasCoverArt()   const { return m_hasCoverArt; }
    QVariantList currentSegments() const { return m_currentSegments; }
    double  getTempoPercent() const { return m_tempoPercent; }
    // Returns the analysed BPM multiplied by the current tempo ratio.
    // Shows 0.0 until BPM analysis is complete.
    double  getCurrentBpm()   const {
        double base = m_trackData ? m_trackData->getBpm() : 0.0;
        return base > 0.0 ? base * (1.0 + m_tempoPercent / 100.0) : 0.0;
    }
    // Speed multiplier for the waveform renderer (e.g. 1.08 at +8%).
    double  getTempoRatio()   const { return 1.0 + m_tempoPercent / 100.0; }

    bool keylock() const { return m_keylock; }

    // Mixer Getters
    double volume() const { return m_volume; }
    double trim() const { return m_trim; }
    double eqHigh() const { return m_eqHigh; }
    double eqMid() const { return m_eqMid; }
    double eqLow() const { return m_eqLow; }
    double filter() const { return m_filter; }
    bool cueEnabled() const { return m_cueEnabled; }
    bool quantizeEnabled() const { return m_quantizeEnabled; }
    bool syncEnabled() const { return m_syncEnabled; }
    bool isSyncMaster() const { return m_isSyncMaster; }
    bool loopActive() const { return m_loopActive; }
    bool loopInSet() const { return m_loopInSet; }
    double loopLengthBeats() const { return m_loopLengthBeats; }
    double loopInPosition() const { return m_loopInSec; }
    double loopOutPosition() const { return m_loopOutSec; }

    // VU meter getters — read atomic peaks from the audio thread
    float vuLevelL() const;
    float vuLevelR() const;
    bool clipDetected() const;
    float gainReduction() const;

    void setCoverArtProvider(CoverArtProvider* provider, const QString& deckId);
    void setLibraryDatabase(LibraryDatabase* db);

public slots:
    void loadTrack(const QString& rawPath);
    void togglePlay();
    void setPosition(float progress);
    void setTempoPercent(double percent);
    
    // Mixer Setters
    void setVolume(double value);
    void setTrim(double value);
    void setEqHigh(double value);
    void setEqMid(double value);
    void setEqLow(double value);
    void setFilter(double value);
    void setCueEnabled(bool value);
    void setQuantizeEnabled(bool enabled);
    void setSyncEnabled(bool enabled);
    void setKeylock(bool value);

    // FX chain
    void setFxEffectType(EffectType type);
    void setFxWetDry(float amount);
    void setFxSCKnob(float knob);   // bipolar -1..+1 for Sound Color

    bool isReverse() const { return m_isReverse; }
    Q_INVOKABLE void setReverse(bool on);

    // Keep the engine's pixel-scale in sync with the waveform renderer.
    void setPixelsPerSecond(double pps) {
        if (m_pixelsPerSecond == pps) return;
        m_pixelsPerSecond = pps;
        emit pixelsPerSecondChanged();
    }

signals:
    void progressChanged();
    void playingChanged();
    void reverseChanged();
    void tempoChanged();
    void trackLoaded();
    void trackMetadataChanged();
    void pixelsPerSecondChanged();
    
    // Mixer Signals
    void volumeChanged();
    void trimChanged();
    void eqHighChanged();
    void eqMidChanged();
    void eqLowChanged();
    void filterChanged();
    void cueEnabledChanged();
    void quantizeEnabledChanged();
    void syncChanged();
    void syncMasterChanged();
    void loopChanged();
    void keylockChanged();
    void vuLevelChanged();
    void gainReductionChanged();
    void segmentsChanged();

private slots:
    void onTimer();

private:
    void persistCurrentAnalysisToLibrary();

    class MixerDspSource;
    class TimeStretchAudioSource;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<class ReverseStreamAudioSource> reverseWrapSource;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<juce::ResamplingAudioSource> resamplingSource;
    std::unique_ptr<TimeStretchAudioSource> timeStretchSource;
    std::unique_ptr<MixerDspSource> mixerSource;
    juce::AudioSourcePlayer sourcePlayer;

    QTimer timer;

    TrackData* m_trackData;
    WaveformAnalyzer* m_analyzer;

    QString m_trackTitle;
    QString m_trackArtist;
    QString m_trackAlbum;
    QString m_trackKey;
    QString m_trackDuration;
    double  m_trackDurationSec = 0.0;
    bool    m_hasTrack = false;

    CoverArtProvider* m_coverProvider = nullptr;
    LibraryDatabase*   m_libraryDb     = nullptr;
    QString m_deckId;
    QString m_currentTrackId;
    QString m_coverArtUrl;
    bool    m_hasCoverArt = false;
    QVariantList m_currentSegments;

    // Tempo control: ±6/8/16/32/100% (WIDE) selectable range
    double m_tempoPercent = 0.0;

    // Mixer state
    double m_volume = 0.8;
    double m_trim = 1.0;
    double m_eqHigh = 0.0;
    double m_eqMid = 0.0;
    double m_eqLow = 0.0;
    double m_filter = 0.0;
    bool   m_isReverse = false;
    bool m_cueEnabled = false;
    bool m_keylock = false;
    bool m_quantizeEnabled = false;
    bool m_syncEnabled = false;
    bool m_isSyncMaster = false;

    bool m_loopActive = false;
    double m_loopInSec = 0.0;
    double m_loopOutSec = 0.0;
    double m_loopLengthBeats = 0.0;
    bool m_loopInSet = false;

    double quantizedBeatAt(double sec) const;
    double beatDurationAround(double sec) const;
    void startLoopAt(double startSec, double lengthBeats);

    void updateSpeedAndPitch();

    // Updates the JUCE transport source gain based on volume and trim
    void updateGain();

    // m_latencySeconds is computed once after device init (output latency + buffer size).
    // m_snapPosition + m_snapClock enable sub-frame interpolation in getVisualPosition().
    float          m_latencySeconds  = 0.0f;
    double         m_snapPosition    = 0.0;
    QElapsedTimer  m_snapClock;
    bool           m_snapValid       = false;

    // Atomic playhead position (seconds). Written on every onTimer() tick,
    // read lock-free by getPlayheadPositionAtomic() from the QML FrameAnimation.
    std::atomic<double> m_atomicPlayheadPos{0.0};

    // Scrub state.
    // m_pixelsPerSecond is mirrored from the waveform renderer (150 pts/s × ppp).
    double m_pixelsPerSecond = 225.0;  // default: 150 pts/s × 1.5 ppp
    bool   m_isScrubbing     = false;
    bool   m_scrubWasPlaying = false;
    double m_scrubHoldPosition = 0.0;
    QElapsedTimer m_lastScrubInputClock;

    static std::mutex s_syncMutex;
    static std::vector<DjEngine*> s_syncDecks;
    static DjEngine* s_syncMasterDeck;
    static void updateSyncMasterLocked();
    static void propagateMasterTempoLocked(DjEngine* master);
};
