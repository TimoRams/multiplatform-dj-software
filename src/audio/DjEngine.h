#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QVariantList>
#include <QElapsedTimer>
#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "TrackData.h"
#include "WaveformAnalyzer.h"
#include "fx/FxProcessor.h"

class CoverArtProvider;
class LibraryDatabase;

class DjEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float progress READ getProgress NOTIFY progressChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool scrubbing READ isScrubbing NOTIFY scrubbingChanged)
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
    Q_PROPERTY(bool slipActive READ slipActive NOTIFY slipChanged)

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

    Q_PROPERTY(double waveformPointsPerSecond READ waveformPointsPerSecond CONSTANT)

    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY pixelsPerSecondChanged)

    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(double trim READ trim WRITE setTrim NOTIFY trimChanged)
    Q_PROPERTY(double eqHigh READ eqHigh WRITE setEqHigh NOTIFY eqHighChanged)
    Q_PROPERTY(double eqMid READ eqMid WRITE setEqMid NOTIFY eqMidChanged)
    Q_PROPERTY(double eqLow READ eqLow WRITE setEqLow NOTIFY eqLowChanged)
    Q_PROPERTY(double filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool cueEnabled READ cueEnabled WRITE setCueEnabled NOTIFY cueEnabledChanged)

    Q_PROPERTY(float vuLevelL READ vuLevelL NOTIFY vuLevelChanged)
    Q_PROPERTY(float vuLevelR READ vuLevelR NOTIFY vuLevelChanged)
    Q_PROPERTY(float preFaderVuLevelL READ preFaderVuLevelL NOTIFY vuLevelChanged)
    Q_PROPERTY(float preFaderVuLevelR READ preFaderVuLevelR NOTIFY vuLevelChanged)
    Q_PROPERTY(bool clipDetected READ clipDetected NOTIFY vuLevelChanged)
    
    Q_PROPERTY(float gainReduction READ gainReduction NOTIFY gainReductionChanged)
    Q_PROPERTY(QVariantList hotCues READ hotCues NOTIFY hotCuesChanged)
    Q_PROPERTY(double mainCueSec READ mainCueSec NOTIFY mainCueChanged)
    Q_PROPERTY(QString lastAudioDeviceError READ lastAudioDeviceError NOTIFY audioDeviceErrorChanged)
    Q_PROPERTY(bool vinylBrakeActive READ isVinylBrakeActive NOTIFY vinylBrakeChanged)
    Q_PROPERTY(bool echoOutActive    READ isEchoOutActive    NOTIFY echoOutChanged)
    Q_PROPERTY(bool backspinActive   READ isBackspinActive   NOTIFY backspinChanged)
    Q_PROPERTY(bool rollOutActive    READ isRollOutActive    NOTIFY rollOutChanged)

public:
    static constexpr double WAVEFORM_POINTS_PER_SECOND = 600.0;
    static void shutdownSharedAudioDeviceManager();

    explicit DjEngine(QObject* parent = nullptr);
    ~DjEngine() override;

    [[nodiscard]] float getProgress() const;
    [[nodiscard]] Q_INVOKABLE float getDuration() const;
    [[nodiscard]] double getPosition() const;
    // Latency-compensated position in seconds, used by the waveform renderer.
    [[nodiscard]] double getVisualPosition() const;
    // QML-safe access to the interpolated visual playhead.
    [[nodiscard]] Q_INVOKABLE double getVisualPositionQml() const;
    // Lock-free atomic read of the playhead position (seconds).
    // Called from QML FrameAnimation every VSync frame — must be wait-free.
    [[nodiscard]] Q_INVOKABLE double getPlayheadPositionAtomic() const;
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] bool isScrubbing() const { return m_isScrubbing; }

    // Pixels-per-second scale mirrored from the waveform renderer so scrubBy()
    // can convert mouse pixels → audio seconds without needing QML math.
    [[nodiscard]] double pixelsPerSecond() const { return m_pixelsPerSecond; }
    [[nodiscard]] double waveformPointsPerSecond() const { return WAVEFORM_POINTS_PER_SECOND; }

    // Universal scratch API used by jogwheel and scrolling waveform.
    // pauseForScrub() captures current play state and enters scratch mode.
    // scrubBy() and scratchBySeconds() move the playhead with audible output.
    // resumeAfterScrub() restores pre-scratch transport state.
    Q_INVOKABLE void pauseForScrub();
    Q_INVOKABLE void scrubBy(double pixelDelta);
    Q_INVOKABLE void scratchBySeconds(double deltaSeconds);
    // Absolute scrub positioning for 1:1 direct manipulation.
    Q_INVOKABLE void setScrubPosition(double positionSeconds);
    // Generic scratch input: signed playback-rate target where 1.0 = normal forward speed.
    // This decouples UI deltas from the audio scratch model (usable for MIDI/HID jog ticks).
    Q_INVOKABLE void pushScratchVelocityTick(double velocityRate);
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

    Q_INVOKABLE void ejectTrack();

    // Master volume + anti-clip (global, shared across all decks)
    Q_INVOKABLE void setMasterVolume(float v);
    Q_INVOKABLE void setAntiClip(bool enabled);
    [[nodiscard]] Q_INVOKABLE double totalLatencyMs() const;
    [[nodiscard]] Q_INVOKABLE QVariantList latencyBreakdown() const;
    Q_INVOKABLE void triggerHotCue(int index);
    Q_INVOKABLE void storeHotCue(int index);
    Q_INVOKABLE void clearHotCue(int index);
    Q_INVOKABLE void setHotCueColor(int index, const QString& colorHex);
    Q_INVOKABLE void cueButtonPress();
    Q_INVOKABLE void cueButtonRelease();

    // ── PAD FX ────────────────────────────────────────────────────────────────
    // Applies a named effect to the PAD FX slot (independent of the FX bar).
    // effectName: "Echo"|"Reverb"|"Roll"|"Flanger"|"Filter"|"Phaser"|"Bitcrusher"|"SlipRoll"|"Trans"
    Q_INVOKABLE void setPadFx(const QString& effectName, float wet = 1.0f);
    Q_INVOKABLE void clearPadFx();

    // ── Vinyl Brake ───────────────────────────────────────────────────────────
    Q_INVOKABLE void startVinylBrake();
    Q_INVOKABLE void stopVinylBrake();
    [[nodiscard]] bool isVinylBrakeActive() const { return m_vinylBrakeActive; }

    // ── Echo Out ──────────────────────────────────────────────────────────────
    Q_INVOKABLE void startEchoOut();
    Q_INVOKABLE void stopEchoOut();
    [[nodiscard]] bool isEchoOutActive() const { return m_echoOutActive; }

    // ── Backspin ──────────────────────────────────────────────────────────────
    Q_INVOKABLE void startBackspin();
    Q_INVOKABLE void stopBackspin();
    [[nodiscard]] bool isBackspinActive() const { return m_backspinActive; }

    // ── Roll Out ──────────────────────────────────────────────────────────────
    Q_INVOKABLE void startRollOut();
    Q_INVOKABLE void stopRollOut();
    [[nodiscard]] bool isRollOutActive() const { return m_rollOutActive; }
    [[nodiscard]] TrackData* getTrackData() const;

    [[nodiscard]] QString trackTitle()    const { return m_trackTitle; }
    [[nodiscard]] QString trackArtist()   const { return m_trackArtist; }
    [[nodiscard]] QString trackAlbum()    const { return m_trackAlbum; }
    [[nodiscard]] QString trackKey()      const { return m_trackKey; }
    [[nodiscard]] QString trackDuration() const { return m_trackDuration; }
    [[nodiscard]] double  trackDurationSec() const { return m_trackDurationSec; }
    [[nodiscard]] bool    hasTrack()      const { return m_hasTrack; }
    [[nodiscard]] QString coverArtUrl()   const { return m_coverArtUrl; }
    [[nodiscard]] bool    hasCoverArt()   const { return m_hasCoverArt; }
    [[nodiscard]] QVariantList currentSegments() const { return m_currentSegments; }
    [[nodiscard]] double  getTempoPercent() const { return m_tempoPercent; }
    // Beat phase: 0.0 = on the beat, 0.5 = halfway between beats, approaches 1.0 just before the next beat.
    [[nodiscard]] Q_INVOKABLE double getBeatPhase() const;
    // Returns the analysed BPM multiplied by the current tempo ratio.
    // Shows 0.0 until BPM analysis is complete.
    [[nodiscard]] double  getCurrentBpm()   const {
        double base = m_trackData ? m_trackData->getBpm() : 0.0;
        return base > 0.0 ? base * (1.0 + m_tempoPercent / 100.0) : 0.0;
    }
    // Speed multiplier for the waveform renderer (e.g. 1.08 at +8%).
    [[nodiscard]] double  getTempoRatio()   const { return 1.0 + m_tempoPercent / 100.0; }

    [[nodiscard]] bool keylock() const { return m_keylock; }

    // Mixer Getters
    [[nodiscard]] double volume() const { return m_volume; }
    [[nodiscard]] double trim() const { return m_trim; }
    [[nodiscard]] double eqHigh() const { return m_eqHigh; }
    [[nodiscard]] double eqMid() const { return m_eqMid; }
    [[nodiscard]] double eqLow() const { return m_eqLow; }
    [[nodiscard]] double filter() const { return m_filter; }
    [[nodiscard]] bool cueEnabled() const { return m_cueEnabled.load(std::memory_order_relaxed); }
    [[nodiscard]] bool quantizeEnabled() const { return m_quantizeEnabled; }
    [[nodiscard]] bool syncEnabled() const { return m_syncEnabled; }
    [[nodiscard]] bool isSyncMaster() const { return m_isSyncMaster; }
    [[nodiscard]] bool loopActive() const { return m_loopActive; }
    [[nodiscard]] bool loopInSet() const { return m_loopInSet; }
    [[nodiscard]] double loopLengthBeats() const { return m_loopLengthBeats; }
    [[nodiscard]] double loopInPosition() const { return m_loopInSec; }
    [[nodiscard]] double loopOutPosition() const { return m_loopOutSec; }

    // VU meter getters — read atomic peaks from the audio thread
    [[nodiscard]] float vuLevelL() const;
    [[nodiscard]] float vuLevelR() const;
    [[nodiscard]] float preFaderVuLevelL() const;
    [[nodiscard]] float preFaderVuLevelR() const;
    [[nodiscard]] bool clipDetected() const;
    [[nodiscard]] float gainReduction() const;
    [[nodiscard]] QVariantList hotCues() const;
    [[nodiscard]] double mainCueSec() const { return m_mainCueSec; }
    [[nodiscard]] QString lastAudioDeviceError() const { return m_lastAudioDeviceError; }

    void setCoverArtProvider(CoverArtProvider* provider, const QString& deckId);
    void setLibraryDatabase(LibraryDatabase* db);

public slots:
    void loadTrack(const QString& rawPath);
    void togglePlay();
    void setPosition(float progress);
    void setTempoPercent(double percent);
    Q_INVOKABLE void setManualBpm(double bpm);
    Q_INVOKABLE bool applyAudioDeviceSettings(int sampleRate, int bufferSize);
    Q_INVOKABLE bool applyAudioDeviceSettings(const QString& deviceType,
                                              const QString& outputDevice,
                                              int sampleRate,
                                              int bufferSize,
                                              int masterFirstChannel = 1,
                                              int headphonesFirstChannel = -1,
                                              int boothFirstChannel = -1);
    Q_INVOKABLE QStringList getAvailableAudioDeviceTypes() const;
    Q_INVOKABLE QStringList getAvailableAudioOutputDevices(const QString& deviceType = QString()) const;
    Q_INVOKABLE QStringList getAvailableOutputChannelPairs(const QString& deviceType = QString(),
                                                           const QString& outputDevice = QString()) const;
    Q_INVOKABLE QString getCurrentAudioDeviceType() const;
    Q_INVOKABLE QString getCurrentAudioOutputDevice() const;
    Q_INVOKABLE int getCurrentAudioSampleRate() const;
    Q_INVOKABLE int getCurrentAudioBufferSize() const;
    Q_INVOKABLE bool isJackServerRunning() const;
    Q_INVOKABLE QString jackServerStatus() const;
    
    // Playback control
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    
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
    // Re-aligns the beat phase with the sync master using a tempo nudge (no seek).
    // Intended for "press SYNC again while already synced" — turns off nothing.
    Q_INVOKABLE void reSync();
    void setKeylock(bool value);

    // FX chain
    void setFxEffectType(EffectType type);
    void setFxWetDry(float amount);
    void setFxSCKnob(float knob);   // bipolar -1..+1 for Sound Color
    void setFxSCParam(float param); // 0..1 mode parameter for Sound Color

    [[nodiscard]] bool isReverse() const { return m_isReverse; }
    Q_INVOKABLE void setReverse(bool on);

    [[nodiscard]] bool slipActive() const { return m_slipActive; }
    Q_INVOKABLE void setSlip(bool on);

    // Keep the engine's pixel-scale in sync with the waveform renderer.
    void setPixelsPerSecond(double pps) {
        if (m_pixelsPerSecond == pps) return;
        m_pixelsPerSecond = pps;
        emit pixelsPerSecondChanged();
    }

signals:
    void progressChanged();
    void playingChanged();
    void scrubbingChanged();
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
    void slipChanged();
    void keylockChanged();
    void vuLevelChanged();
    void gainReductionChanged();
    void segmentsChanged();
    void hotCuesChanged();
    void mainCueChanged();
    void audioDeviceErrorChanged();
    void vinylBrakeChanged();
    void echoOutChanged();
    void backspinChanged();
    void rollOutChanged();

private slots:
    void onTimer();

private:
    struct LatencySnapshot {
        int outputEffectiveSamples = 0;
        int outputRawSamples = 0;
        int bufferSamples = 0;
        int rubberbandSamples = 0;
        int limiterSamples = 0;
        double sampleRate = 44100.0;
    };

    LatencySnapshot buildLatencySnapshot() const;

    void resetTrackLoadState();
    void populateMetadataFromReader(const juce::AudioFormatReader& reader,
                                    const QString& rawPath,
                                    const juce::File& file);
    void updateTrackDuration(double durationSec);
    bool hydrateLibraryStateForTrack(const QString& rawPath, double durationSec);
    void attachReaderToTransport(juce::AudioFormatReader* reader);
    void returnToSlipPosition();
    bool isSlipDiverted() const { return m_slipActive && (m_loopActive || m_isReverse); }

    void persistCurrentAnalysisToLibrary();
    void clearHotCueState();
    void loadHotCuesForCurrentTrack();
    void persistHotCueSlot(int index);
    bool isValidHotCueIndex(int index) const;
    void loadMainCueForCurrentTrack();
    void persistMainCuePoint();
    void setLastAudioDeviceError(const QString& error);

    struct HotCueSlot {
        bool set = false;
        double positionSec = 0.0;
        QString label;
        QString color = "#e04040";
    };

    class MixerDspSource;
    class TimeStretchAudioSource;

    juce::AudioDeviceManager& deviceManager;
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
    std::atomic<quint64> m_loadGen{0};
    QVariantList m_currentSegments;
    std::array<HotCueSlot, 8> m_hotCueSlots;
    double m_mainCueSec = -1.0;
    bool m_mainCuePreviewActive = false;
    QString m_lastAudioDeviceError;

    // Tempo control: ±6/8/16/32/100% (WIDE) selectable range
    double m_tempoPercent = 0.0;
    // Phase correction nudge added to m_tempoPercent inside updateSpeedAndPitch().
    // Cleared on scratch and when sync is disabled. Normally capped at ±4%; ±8% during reSync.
    double m_phaseNudge = 0.0;
    bool   m_resyncBoost = false;

    bool m_vinylBrakeActive = false;
    bool m_echoOutActive    = false;
    bool m_backspinActive   = false;
    bool m_rollOutActive    = false;

    // Mixer state
    double m_volume = 0.8;
    double m_trim = 1.0;
    double m_eqHigh = 0.0;
    double m_eqMid = 0.0;
    double m_eqLow = 0.0;
    double m_filter = 0.0;
    bool   m_isReverse = false;
    bool   m_slipActive   = false;
    double m_slipPosition = 0.0;
    std::atomic<bool> m_cueEnabled { false };
    bool m_playRequested = false;
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
    void applyLoopRangeToAudioSource();
    void clearLoopRangeOnAudioSource();

    void updateSpeedAndPitch();
    void updatePhaseCorrection();
    // Bypass for internal/sync use — no follower-lock guard, no master propagation.
    void applyTempoPercent(double percent);
    // Seek this deck so its beat phase matches master's. Called when sync is first enabled.
    void snapPhaseToMaster(DjEngine* master);
    void refreshHardwareLatency();
    void setSnapAnchor(double positionSec, bool valid);
    void armSnapFromTransportPosition();
    void freezeTransportAt(double positionSec);
    void ensureTransportRunningForPlayIntent();
    void applyScratchNeutralRouting();
    void restorePostScrubPlaybackState();
    void advanceAbsoluteScrubFollower(double dtSec);
    void updateScrubPlayheadAnchor();

    void updateGain();
    void applyMixerEq();
    void applyMixerFilter();

    // m_latencySeconds tracks effective output latency reported by the audio device.
    // getOutputLatencyInSamples() is JUCE's callback->speaker delay and already
    // includes the callback buffer on compliant drivers.
    // m_snapPosition + m_snapClock enable sub-frame interpolation in getVisualPosition().
    float          m_latencySeconds  = 0.0f;
    mutable LatencySnapshot m_lastLatencySnapshot;
    double         m_snapPosition    = 0.0;
    double         m_snapTempoRatio  = 1.0;
    QElapsedTimer  m_snapClock;
    bool           m_snapValid       = false;

    // Atomic playhead position (seconds). Written on every onTimer() tick,
    // read lock-free by getPlayheadPositionAtomic() from the QML FrameAnimation.
    std::atomic<double> m_atomicPlayheadPos{0.0};

    struct ScratchConfig {
        static constexpr double kRateAttackTauSec = 0.014;
        static constexpr double kRateReleaseTauSec = 0.055;
        static constexpr double kIdleTimeoutSec = 0.120;
        static constexpr double kMaxRate = 12.0;
        static constexpr double kReleaseToPlayTauSec = 0.36;
        static constexpr double kReleaseToStopTauSec = 0.20;
        static constexpr double kReleaseSettleThreshold = 0.015;
        static constexpr double kControlResumeThresholdRate = 0.0012;
        static constexpr double kControlStopThresholdRate = 0.0006;
        static constexpr double kDirectStepLimitSec = 0.02;
        static constexpr double kFineMoveThresholdSec = 0.003;
        static constexpr double kEventSpikeClampSec = 0.08;
        static constexpr double kInertiaMoveThresholdSec = 0.002;
        static constexpr double kInputRateFilterAlpha = 0.34;
        static constexpr double kInputRateSlewPerSec = 28.0;
        static constexpr double kDirectionFlipThresholdRate = 0.08;
        static constexpr double kAbsoluteFollowStiffness = 150.0;
        static constexpr double kAbsoluteFollowDamping = 24.0;
        static constexpr double kAbsoluteMaxFollowRate = 16.0;
        static constexpr double kAbsoluteSnapDistanceSec = 0.00035;
        static constexpr double kAbsoluteSnapVelocitySecPerSec = 0.015;
    };

    // Scrub state.
    // m_pixelsPerSecond is mirrored from the waveform renderer (WAVEFORM_POINTS_PER_SECOND × ppp).
    double m_pixelsPerSecond = WAVEFORM_POINTS_PER_SECOND * 1.5;  // default with 1.5 ppp
    bool   m_isScrubbing     = false;
    bool   m_scrubWasPlaying = false;
    double m_scrubHoldPosition = 0.0;
    QElapsedTimer m_lastScrubInputClock;
    QElapsedTimer m_scrubPhysicsClock;
    double m_scratchTargetRate = 0.0;
    double m_scratchSmoothedRate = 0.0;
    double m_scratchRateAttackTauSec = ScratchConfig::kRateAttackTauSec;
    double m_scratchRateReleaseTauSec = ScratchConfig::kRateReleaseTauSec;
    double m_scratchIdleTimeoutSec = ScratchConfig::kIdleTimeoutSec;
    double m_scratchMaxRate = ScratchConfig::kMaxRate;
    bool   m_scratchReleaseActive = false;
    double m_scratchReleaseTargetRate = 0.0;
    double m_scratchReleaseToPlayTauSec = ScratchConfig::kReleaseToPlayTauSec;
    double m_scratchReleaseToStopTauSec = ScratchConfig::kReleaseToStopTauSec;
    double m_scratchReleaseSettleThreshold = ScratchConfig::kReleaseSettleThreshold;
    double m_scratchControlResumeThresholdRate = ScratchConfig::kControlResumeThresholdRate;
    double m_scratchControlStopThresholdRate = ScratchConfig::kControlStopThresholdRate;
    double m_scratchDirectStepLimitSec = ScratchConfig::kDirectStepLimitSec;
    double m_scratchFineMoveThresholdSec = ScratchConfig::kFineMoveThresholdSec;
    double m_scratchEventSpikeClampSec = ScratchConfig::kEventSpikeClampSec;
    double m_scratchAccumulatedMoveSec = 0.0;
    double m_scratchInertiaMoveThresholdSec = ScratchConfig::kInertiaMoveThresholdSec;
    double m_scratchBaseRate = 1.0;
    double m_scratchInputFilteredRate = 0.0;
    double m_scratchInputRateFilterAlpha = ScratchConfig::kInputRateFilterAlpha;
    double m_scratchInputRateSlewPerSec = ScratchConfig::kInputRateSlewPerSec;
    double m_scratchDirectionSign = 1.0;
    double m_scratchDirectionFlipThresholdRate = ScratchConfig::kDirectionFlipThresholdRate;
    bool   m_scratchAbsolutePositionControl = false;
    double m_scratchAbsoluteTargetPosition = 0.0;
    double m_scratchAbsoluteFollowVelocity = 0.0;
    double m_scratchAbsoluteFollowStiffness = ScratchConfig::kAbsoluteFollowStiffness;
    double m_scratchAbsoluteFollowDamping = ScratchConfig::kAbsoluteFollowDamping;
    double m_scratchAbsoluteMaxFollowRate = ScratchConfig::kAbsoluteMaxFollowRate;
    double m_scratchAbsoluteSnapDistanceSec = ScratchConfig::kAbsoluteSnapDistanceSec;
    double m_scratchAbsoluteSnapVelocitySecPerSec = ScratchConfig::kAbsoluteSnapVelocitySecPerSec;
    bool   m_scrubSavedReverseState = false;
    double m_loadedTrackSampleRate = 44100.0;

    static std::mutex s_syncMutex;
    static std::vector<DjEngine*> s_syncDecks;
    static DjEngine* s_syncMasterDeck;
    static void updateSyncMasterLocked();
    static void propagateMasterTempoLocked(DjEngine* master);
};
