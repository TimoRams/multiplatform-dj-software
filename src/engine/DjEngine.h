#pragma once

#include "deck/DeckCueLoopController.h"
#include "deck/DeckTrackLoader.h"
#include "audio/cache/AudioCacheHandle.h"
#include "app/ControlClock.h"
#include "sync/DeckSyncController.h"
#include "scratch/ScratchSession.hpp"
#include "fx/FxProcessor.h" // EffectType is part of the public controller facade contract.

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QElapsedTimer>
#include <atomic>
#include <cstdint>
#include <memory>
#include <juce_audio_formats/juce_audio_formats.h>

#include "TransportLimits.h"

class AudioDeviceService;
class AudioPageCache;
class DeckAudioPipeline;
class DeckTransport;
class LibraryDatabase;
class QTimer;
class TrackData;
class WaveformAnalyzer;
class AnalyzerResultMailbox;
class CoverArtProvider;
class LibraryCoverService;
namespace engine::sync {
class SyncCoordinator;
}

class DjEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float progress READ getProgress NOTIFY progressChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool scrubbing READ isScrubbing NOTIFY scrubbingChanged)
    Q_PROPERTY(bool scratchVisualActive READ isScratchVisualActive NOTIFY scrubbingChanged)
    Q_PROPERTY(bool isReverse READ isReverse NOTIFY reverseChanged)
    Q_PROPERTY(bool keylock READ keylock WRITE setKeylock NOTIFY keylockChanged)
    Q_PROPERTY(double tempoPercent READ getTempoPercent WRITE setTempoPercent NOTIFY tempoChanged)
    Q_PROPERTY(double tempoRangePercent READ tempoRangePercent WRITE setTempoRangePercent NOTIFY tempoRangeChanged)
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
    Q_PROPERTY(double loopPreviewOutPosition READ loopPreviewOutPosition NOTIFY progressChanged)
    Q_PROPERTY(bool slipActive READ slipActive NOTIFY slipChanged)

    Q_PROPERTY(QString trackTitle   READ trackTitle   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackArtist  READ trackArtist  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackAlbum   READ trackAlbum   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackNumber  READ trackNumber  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackKey     READ trackKey     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackDuration READ trackDuration NOTIFY trackMetadataChanged)
    Q_PROPERTY(double trackDurationSec READ trackDurationSec NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasTrack     READ hasTrack     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackFilePath READ trackFilePath NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString coverArtUrl  READ coverArtUrl  NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasCoverArt  READ hasCoverArt  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackLoadError READ trackLoadError NOTIFY trackLoadErrorChanged)
    Q_PROPERTY(QVariantList currentSegments READ currentSegments NOTIFY segmentsChanged)

    Q_PROPERTY(double waveformPointsPerSecond READ waveformPointsPerSecond CONSTANT)
    Q_PROPERTY(double preRollSeconds READ getPreRollSeconds CONSTANT)

    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY pixelsPerSecondChanged)

    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(double trim READ trim WRITE setTrim NOTIFY trimChanged)
    Q_PROPERTY(double eqHigh READ eqHigh WRITE setEqHigh NOTIFY eqHighChanged)
    Q_PROPERTY(double eqMid READ eqMid WRITE setEqMid NOTIFY eqMidChanged)
    Q_PROPERTY(double eqLow READ eqLow WRITE setEqLow NOTIFY eqLowChanged)
    Q_PROPERTY(double filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool polarityInverted READ polarityInverted WRITE setPolarityInverted NOTIFY polarityInvertedChanged)
    Q_PROPERTY(bool cueEnabled READ cueEnabled WRITE setCueEnabled NOTIFY cueEnabledChanged)
    Q_PROPERTY(bool masterCueEnabled READ masterCueEnabled WRITE setMasterCueEnabled NOTIFY masterCueEnabledChanged)
    Q_PROPERTY(double headphoneMix READ headphoneMix WRITE setHeadphoneMix NOTIFY headphoneMixChanged)

    Q_PROPERTY(float vuLevelL READ vuLevelL NOTIFY vuLevelChanged)
    Q_PROPERTY(float vuLevelR READ vuLevelR NOTIFY vuLevelChanged)
    Q_PROPERTY(float preFaderVuLevelL READ preFaderVuLevelL NOTIFY vuLevelChanged)
    Q_PROPERTY(float preFaderVuLevelR READ preFaderVuLevelR NOTIFY vuLevelChanged)
    Q_PROPERTY(bool clipDetected READ clipDetected NOTIFY vuLevelChanged)
    
    Q_PROPERTY(float gainReduction READ gainReduction NOTIFY gainReductionChanged)
    Q_PROPERTY(QVariantList hotCues READ hotCues NOTIFY hotCuesChanged)
    Q_PROPERTY(QVariantList savedLoops READ savedLoops NOTIFY savedLoopsChanged)
    Q_PROPERTY(bool beatgridLocked READ beatgridLocked WRITE setBeatgridLocked NOTIFY beatgridLockedChanged)
    Q_PROPERTY(double mainCueSec READ mainCueSec NOTIFY mainCueChanged)
    Q_PROPERTY(QString lastAudioDeviceError READ lastAudioDeviceError NOTIFY audioDeviceErrorChanged)
    Q_PROPERTY(QString audioDeviceFallbackMessage READ audioDeviceFallbackMessage NOTIFY audioDeviceFallbackChanged)
    Q_PROPERTY(bool vinylBrakeActive READ isVinylBrakeActive NOTIFY vinylBrakeChanged)
    Q_PROPERTY(bool echoOutActive    READ isEchoOutActive    NOTIFY echoOutChanged)
    Q_PROPERTY(bool backspinActive   READ isBackspinActive   NOTIFY backspinChanged)
    Q_PROPERTY(bool rollOutActive    READ isRollOutActive    NOTIFY rollOutChanged)

public:
    static constexpr double WAVEFORM_POINTS_PER_SECOND = 1200.0;
    // Silent pre-roll zone before track start (t=0): waveform/beatgrid extends backward,
    // audio output is silence (JUCE transport stays at 0 during pre-roll).
    static constexpr double PRE_ROLL_SECONDS = TransportLimits::kPreRollSeconds;
    static constexpr double SCRATCH_PRE_ROLL_SECONDS = PRE_ROLL_SECONDS * 4.0;
    /** Stop Qt timers and waveform analysis before QML / MIDI teardown. */
    void prepareForShutdown();
    /** Detach file readers from scratch/transport; call after audio device is closed. */
    void releaseTransportReaders();

    explicit DjEngine(AudioDeviceService& audioDeviceService, AudioPageCache& audioPageCache,
                      DeckAudioPipeline& audioPipeline,
                      ControlClock& controlClock,
                      engine::sync::SyncCoordinator& syncCoordinator, int deckIndex,
                      QObject* parent = nullptr);
    ~DjEngine() override;

    [[nodiscard]] float getProgress() const;
    [[nodiscard]] Q_INVOKABLE float getDuration() const;
    [[nodiscard]] Q_INVOKABLE double getPreRollSeconds() const;
    [[nodiscard]] double getPosition() const;
    // Latency-compensated position in seconds, used by the waveform renderer.
    [[nodiscard]] double getVisualPosition() const;
    // QML-safe access to the interpolated visual playhead.
    [[nodiscard]] Q_INVOKABLE double getVisualPositionQml() const;
    // Lock-free atomic read of the playhead position (seconds).
    // Called from QML FrameAnimation every VSync frame — must be wait-free.
    [[nodiscard]] Q_INVOKABLE double getPlayheadPositionAtomic() const;
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] bool isScrubbing() const { return m_scratch.scrubbing(); }
    [[nodiscard]] bool isScratchReleaseActive() const { return m_scratch.releaseGlide(); }
    [[nodiscard]] Q_INVOKABLE bool isScratchVisualActive() const {
        return m_scratch.scrubbing() || m_scratch.releaseGlide();
    }

    [[nodiscard]] double pixelsPerSecond() const { return m_pixelsPerSecond; }
    [[nodiscard]] double waveformPointsPerSecond() const { return WAVEFORM_POINTS_PER_SECOND; }

    // Scratch: bridge PD physics + Hermite pull. Waveform/turntable → setScrubPosition; MIDI → scratchBySeconds.
    Q_INVOKABLE void pauseForScrub(double anchorPositionSec = -1.0);
    Q_INVOKABLE void scratchBySeconds(double deltaSeconds, bool vinylOneToOnePosition = false);
    void scratchBySecondsTimed(double deltaSeconds, double eventIntervalSeconds);
    Q_INVOKABLE void setScrubPosition(double positionSeconds);
    [[nodiscard]] Q_INVOKABLE double platterAngleDegrees() const;
    Q_INVOKABLE void resumeAfterScrub();
    Q_INVOKABLE void applyScratchReleaseJog(double deltaSeconds);
    Q_INVOKABLE void finishScrubWithoutInertia();
    // Hardware release path: signed normalized speed, where 1.0 is track speed.
    void requestScratchRelease(double normalizedReleaseSpeed, bool allowInertia = true);
    void submitScratchReleaseSpeed(double normalizedReleaseSpeed);
    // Outer-rim jog nudge: temporarily speeds up/slows down playback without entering scratch mode.
    Q_INVOKABLE void applyJogNudge(double signedTicks);

    // Manual beat-grid correction: rebuilds the BeatMarker array so that the
    // current playhead position becomes beat 1 / bar 1.  Emits beatgridChanged
    // via TrackData, which the waveform renderer picks up automatically.
    Q_INVOKABLE void setDownbeatAtCurrentPosition();
    Q_INVOKABLE void setDownbeatAtPosition(double anchorSec);
    Q_INVOKABLE void nudgeBeatgridMs(double milliseconds);
    Q_INVOKABLE void nudgeBeatgridBeats(double beats);

    // Half-time / double-time correction.
    // Finds the nearest existing downbeat to the current playhead as a stable
    // anchor, doubles/halves the stored BPM, then rebuilds the grid from there.
    Q_INVOKABLE void doubleBpm();
    Q_INVOKABLE void halveBpm();

    // Beatgrid-aligned loop controls
    Q_INVOKABLE void setLoopIn();
    Q_INVOKABLE void setLoopOut();
    Q_INVOKABLE void setLoop4Beats();
    Q_INVOKABLE void toggleLoop4Beats();
    Q_INVOKABLE void toggleLoopThreeQuarter();
    Q_INVOKABLE void halveLoopLength();
    Q_INVOKABLE void doubleLoopLength();
    Q_INVOKABLE void clearLoop();
    Q_INVOKABLE void deactivateLoop();
    Q_INVOKABLE void reactivateLoop();
    Q_INVOKABLE void beatJump(double beats);

    Q_INVOKABLE void ejectTrack();

    // Per-deck output channel assignment (1-indexed first channel of the stereo pair).
    // DeckA defaults to masterFirstChannel; DeckB is set to masterFirstChannel+2.
    Q_INVOKABLE void setOutputFirstChannel(int firstChannel);

    // Master volume + anti-clip (global, shared across all decks)
    Q_INVOKABLE void setMasterVolume(float v);
    Q_INVOKABLE void setAntiClip(bool enabled);
    [[nodiscard]] Q_INVOKABLE double totalLatencyMs() const;
    [[nodiscard]] Q_INVOKABLE QVariantList latencyBreakdown() const;
    [[nodiscard]] Q_INVOKABLE QVariantMap audioPerformanceStats() const;
    Q_INVOKABLE void triggerHotCue(int index);
    Q_INVOKABLE void storeHotCue(int index);
    Q_INVOKABLE void clearHotCue(int index);
    // Unified cue pad: hot cue or loop cue on the same 8 pads.
    Q_INVOKABLE void triggerCuePad(int index);
    Q_INVOKABLE void storeCuePad(int index);
    Q_INVOKABLE void clearCuePad(int index);
    [[nodiscard]] Q_INVOKABLE bool hasStorableLoopRegion() const;
    [[nodiscard]] Q_INVOKABLE bool isLoopCuePad(int index) const;
    Q_INVOKABLE void setHotCueColor(int index, const QString& colorHex);
    Q_INVOKABLE void triggerSavedLoop(int index);
    Q_INVOKABLE void storeSavedLoop(int index);
    Q_INVOKABLE void clearSavedLoop(int index);
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
    [[nodiscard]] QString trackNumber()   const { return m_trackNumber; }
    [[nodiscard]] QString trackKey()      const { return m_trackKey; }
    [[nodiscard]] QString trackDuration() const { return m_trackDuration; }
    [[nodiscard]] double  trackDurationSec() const;
    [[nodiscard]] bool    hasTrack()      const { return m_hasTrack; }
    [[nodiscard]] QString trackFilePath() const { return m_trackFilePath; }
    [[nodiscard]] bool polarityInverted() const { return m_polarityInverted; }
    [[nodiscard]] QString coverArtUrl()   const { return m_coverArtUrl; }
    [[nodiscard]] bool    hasCoverArt()   const { return m_hasCoverArt; }
    [[nodiscard]] QImage  currentCoverImage() const;
    [[nodiscard]] QVariantList currentSegments() const { return m_currentSegments; }
    [[nodiscard]] double  getTempoPercent() const { return m_tempoPercent; }
    [[nodiscard]] double  tempoRangePercent() const { return m_tempoRangePercent; }
    // Beat phase: 0.0 = on the beat, 0.5 = halfway between beats, approaches 1.0 just before the next beat.
    [[nodiscard]] Q_INVOKABLE double getBeatPhase() const;
    // Bar phase: 0.0 = on the downbeat ("the 1"), 0.25/0.5/0.75 = beats 2/3/4 of
    // the bar. Anchored to downbeat markers so sync can arrange grids bar-aligned.
    [[nodiscard]] Q_INVOKABLE double getBarPhase() const;
    [[nodiscard]] double getBeatPosition() const;
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
    [[nodiscard]] bool cueEnabled() const;
    [[nodiscard]] bool masterCueEnabled() const;
    [[nodiscard]] double headphoneMix() const;
    [[nodiscard]] bool quantizeEnabled() const { return m_quantizeEnabled; }
    [[nodiscard]] bool syncEnabled() const { return m_syncController->snapshot().syncEnabled; }
    [[nodiscard]] bool isSyncMaster() const { return m_syncController->snapshot().isMaster; }
    [[nodiscard]] bool loopActive() const { return m_cueLoopController.activeLoop().active; }
    [[nodiscard]] bool loopInSet() const { return m_cueLoopController.activeLoop().inSet; }
    [[nodiscard]] double loopLengthBeats() const { return m_cueLoopController.activeLoop().lengthBeats; }
    [[nodiscard]] double loopInPosition() const { return m_cueLoopController.activeLoop().inSec; }
    [[nodiscard]] double loopOutPosition() const { return m_cueLoopController.activeLoop().outSec; }
    [[nodiscard]] double loopPreviewOutPosition() const;

    // VU meter getters — read atomic peaks from the audio thread
    [[nodiscard]] float vuLevelL() const;
    [[nodiscard]] float vuLevelR() const;
    [[nodiscard]] float preFaderVuLevelL() const;
    [[nodiscard]] float preFaderVuLevelR() const;
    [[nodiscard]] bool clipDetected() const;
    [[nodiscard]] float gainReduction() const;

    // Explicit bootstrap boundary; AudioEngine stores only this audio endpoint.
    [[nodiscard]] DeckAudioPipeline& audioEndpoint() const noexcept;
    [[nodiscard]] QVariantList hotCues() const;
    [[nodiscard]] QVariantList savedLoops() const;
    [[nodiscard]] bool beatgridLocked() const;
    void setBeatgridLocked(bool locked);
    [[nodiscard]] double mainCueSec() const { return m_cueLoopController.mainCue().positionSec; }
    [[nodiscard]] QString lastAudioDeviceError() const;
    [[nodiscard]] QString audioDeviceFallbackMessage() const;
    [[nodiscard]] QString trackLoadError() const { return m_trackLoadError; }

    void setCoverArtProvider(CoverArtProvider* provider, const QString& deckId);
    void setLibraryCoverService(LibraryCoverService* service);
    void setLibraryDatabase(LibraryDatabase* db);

public slots:
    void loadTrack(const QString& rawPath);
    void togglePlay();
    void setPosition(float progress);
    void setTempoPercent(double percent);
    Q_INVOKABLE void setTempoRangePercent(double percent);
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
    // Real-time audio path: update DSP without NOTIFY signals (knob drag / fader).
    void applyVolume(double value);
    void applyTrim(double value);
    void applyEqHigh(double value);
    void applyEqMid(double value);
    void applyEqLow(double value);
    void applyFilter(double value);
    void applyPolarityInverted(bool inverted);
    void setPolarityInverted(bool inverted);
    void setCueEnabled(bool value);
    Q_INVOKABLE void setMasterCueEnabled(bool value);
    Q_INVOKABLE void setHeadphoneMix(double value);
    void setQuantizeEnabled(bool enabled);
    void setSyncEnabled(bool enabled);
    // Re-aligns the beat phase with the sync master using a tempo nudge (no seek).
    // Intended for "press SYNC again while already synced" — turns off nothing.
    Q_INVOKABLE void reSync();
    void setKeylock(bool value);

    // FX chain
    // Color FX (Sound Color) slot
    void setFxEffectType(EffectType type);
    void setFxWetDry(float amount);
    void setFxSCKnob(float knob);               // bipolar -1..+1 for Sound Color
    void setFxSCParam(float param);             // 0..1 mode parameter for Sound Color
    void setFxExternalDelayTime(float seconds); // ≥0 = BPM-synced override; <0 = off
    void setFxPrimaryParam(float v);            // effect-specific primary param (0..1)

    // Beat FX chain slots (1-based slot index)
    void setFxSlotEffectType(int slot, EffectType type);
    void setFxSlotWetDry(int slot, float amount);
    void setFxSlotExternalDelayTime(int slot, float seconds);
    void setFxSlotPrimaryParam(int slot, float v);

    [[nodiscard]] bool isReverse() const;
    Q_INVOKABLE void setReverse(bool on);

    [[nodiscard]] bool slipActive() const;
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
    void tempoRangeChanged();
    void trackLoaded();
    void trackEjected();
    void trackMetadataChanged();
    void trackLoadErrorChanged();
    void pixelsPerSecondChanged();
    
    // Mixer Signals
    void volumeChanged();
    void trimChanged();
    void eqHighChanged();
    void eqMidChanged();
    void eqLowChanged();
    void filterChanged();
    void polarityInvertedChanged();
    void cueEnabledChanged();
    void masterCueEnabledChanged();
    void headphoneMixChanged();
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
    void savedLoopsChanged();
    void beatgridLockedChanged();
    void mainCueChanged();
    void audioDeviceErrorChanged();
    void audioDeviceFallbackChanged();
    void vinylBrakeChanged();
    void echoOutChanged();
    void backspinChanged();
    void rollOutChanged();

private slots:

private:
    struct LatencySnapshot {
        int backendOutputSamples = 0;
        int outputRawSamples = 0;
        int bufferSamples = 0;
        int keylockSamples = 0;
        int resamplerSamples = 0;
        int limiterSamples = 0;
        int mixerFxSamples = 0;
        double sampleRate = 44100.0;
    };

    LatencySnapshot buildLatencySnapshot() const;

    void resetTrackLoadState();
    void applyPreparedTrack(TrackLoadResult result);
    void updateTrackDuration(double durationSec);
    bool hydrateLibraryStateForTrack(const QString& rawPath, double durationSec);
    void attachCacheToTransport(AudioCacheHandle cacheHandle, double trackSampleRate,
                                double trackDurationSeconds);
    void returnToSlipPosition();
    bool isSlipDiverted() const;

    void persistCurrentAnalysisToLibrary();
    void clearHotCueState();
    void loadHotCuesForCurrentTrack();
    void persistHotCueSlot(int index);
    bool isValidHotCueIndex(int index) const;
    bool isHotCuePad(int index) const;
    void triggerHotCueJump(int index);
    void clearSavedLoopState();
    void loadSavedLoopsForCurrentTrack();
    void persistSavedLoopSlot(int index);
    bool isValidSavedLoopIndex(int index) const;
    void activateLoopRange(double inSec, double outSec, bool jumpToIn);
    void loadMainCueForCurrentTrack();
    void persistMainCuePoint();
    void resetMainCueButtonState();
    void startMainCueHoldPreview(quint64 pressSerial);

    enum class StopEffect { VinylBrake, Backspin, EchoOut, RollOut };
    void activateStopEffect(StopEffect effect);
    void deactivateStopEffect(StopEffect effect);

    using HotCueSlot = DeckCueLoopController::HotCue;
    using SavedLoopSlot = DeckCueLoopController::SavedLoop;
    HotCueSlot& slotAt(int i) { return m_cueLoopController.hotCues()[static_cast<size_t>(i)]; }
    const HotCueSlot& slotAt(int i) const { return m_cueLoopController.hotCues()[static_cast<size_t>(i)]; }
    SavedLoopSlot& savedLoopAt(int i) { return m_cueLoopController.savedLoops()[static_cast<size_t>(i)]; }
    const SavedLoopSlot& savedLoopAt(int i) const { return m_cueLoopController.savedLoops()[static_cast<size_t>(i)]; }

    AudioDeviceService& m_audioDeviceService;
    AudioPageCache& m_audioPageCache;
    DeckAudioPipeline* m_audioPipeline = nullptr;
    std::unique_ptr<DeckTransport> m_transport;
    DeckCueLoopController m_cueLoopController;
    DeckTrackLoader m_trackLoader;
    juce::AudioFormatManager formatManager;
    QTimer* m_analysisPersistTimer = nullptr;

    TrackData* m_trackData;
    std::unique_ptr<WaveformAnalyzer> m_analyzer;
    std::shared_ptr<AnalyzerResultMailbox> m_analysisMailbox;

    QString m_trackTitle;
    QString m_trackArtist;
    QString m_trackAlbum;
    QString m_trackNumber;
    QString m_trackGenre;
    QString m_trackComment;
    QString m_trackKey;
    QString m_trackDuration;
    bool    m_hasTrack = false;

    CoverArtProvider*     m_coverProvider       = nullptr;
    LibraryCoverService*  m_libraryCoverService = nullptr;
    LibraryDatabase*      m_libraryDb         = nullptr;
    QString m_deckId;
    QString m_currentTrackId;
    QString m_trackFilePath;
    QString m_coverArtUrl;
    QString m_trackLoadError;
    bool    m_hasCoverArt = false;
    QVariantList m_currentSegments;

    // Tempo control: ±6/8/16/32/100% (WIDE) selectable range
    double m_tempoPercent = 0.0;
    double m_tempoRangePercent = 8.0;
    // Jog outer-rim nudge: temporary speed offset from rim turning (no touch press).
    double m_jogNudgePercent = 0.0;
    double m_jogNudgeCommandPercent = 0.0;
    QElapsedTimer m_lastJogNudgeClock;

    bool m_vinylBrakeActive = false;
    bool m_echoOutActive    = false;
    bool m_backspinActive   = false;
    bool m_rollOutActive    = false;

    // Throttle vuLevelChanged / gainReductionChanged so the UI is not repainted
    // at 250 Hz from the 4 ms control timer while dragging mixer controls.
    float m_lastNotifiedVuL     = 0.0f;
    float m_lastNotifiedVuR     = 0.0f;
    float m_lastNotifiedPreVuL   = 0.0f;
    float m_lastNotifiedPreVuR   = 0.0f;
    float m_lastNotifiedGr       = 1.0f;
    QElapsedTimer m_vuNotifyClock;
    double m_lastNotifiedProgressSec = 0.0;
    QElapsedTimer m_progressNotifyClock;

    // Mixer state
    double m_volume = 0.8;
    double m_trim = 1.0;
    double m_eqHigh = 0.0;
    double m_eqMid = 0.0;
    double m_eqLow = 0.0;
    double m_filter = 0.0;
    bool   m_polarityInverted = false;
    bool m_playLogged    = false;   // true once logPlay() has been called for the current track load
    double m_playedAccumSec = 0.0;  // accumulated real playback seconds since last track load
    QElapsedTimer m_playHistoryClock;
    bool m_keylock = false;
    bool m_quantizeEnabled = false;

    // Quantized cue trigger: when quantize is on and the deck is playing, a hot
    // cue / cue press is deferred to the next beat so the jump lands exactly on
    // the grid (CDJ-style). The deferred jump is serviced by the ControlClock.
    void   scheduleQuantizedCueJump(double targetSec);
    void   cancelQuantizedCueJump();
    bool   serviceQuantizedCueJump();
    void   performCueJump(double targetSec);
    double nextBeatBoundaryAfter(double sec) const;

    struct BeatInterval { double prevSec; double lengthSec; };
    BeatInterval beatIntervalAt(double positionSec) const;
    double quantizedBeatAt(double sec) const;
    double beatDurationAround(double sec) const;
    void startLoopAt(double startSec, double lengthBeats);
    void applyLoopRangeToAudioSource();
    void clearLoopRangeOnAudioSource();
    void updateFxBeatSyncPosition();

    void updateSpeedAndPitch();
    [[nodiscard]] double keylockLatencySeconds() const;
    // Bypass for internal/sync use — no follower-lock guard, no master propagation.
    void applyTempoPercent(double percent);
    // Clamped transport seek used by sync to arrange phase (handles pre-roll).
    void applySyncSeekOffset(double seekOffset);
    // When a synced follower starts playing, re-match tempo and arrange bars to
    // the master so it drops in phase-locked (Serato-style).
    void alignToSyncMasterOnPlay();
    [[nodiscard]] engine::sync::DeckSyncInputSnapshot buildSyncInputSnapshot() const;
    void publishSyncInputAndApplyActions();
    void applyPendingSyncActions();
    void refreshSyncFacadeSignals();
    void onFastControlTick(const ControlTickContext& context);
    void onTransportControlTick(const ControlTickContext& context);
    void onSyncInputControlTick(const ControlTickContext& context);
    void onSyncApplyControlTick(const ControlTickContext& context);
    void onWaveformControlTick(const ControlTickContext& context);
    void onMeterControlTick(const ControlTickContext& context);
    void refreshHardwareLatency();
    void setSnapAnchor(double positionSec, bool valid);
    void armSnapFromTransportPosition();
    void armVisualSeekSettle();
    void freezeTransportAt(double positionSec);
    void terminateScratchSession(double positionSec);
    void syncScratchBridgeToTransport();
    void ensureTransportRunningForPlayIntent();
    void applyScratchNeutralRouting();
    void completeScratchRelease(bool allowInertia);
    void restorePostScrubPlaybackState(double finalCursorSeconds);
    [[nodiscard]] engine::scratch::ScratchLoopCtx scratchLoopCtx() const noexcept;
    void updateScrubPlayheadAnchor();
    void tickScratchPhysics();
    void decayJogNudge();
    void emitPlaybackStateChanged() {
        emit progressChanged(); emit vuLevelChanged(); emit gainReductionChanged();
    }

    void applyMixerEq();
    void applyMixerFilter();
    void notifyVuMetersIfNeeded();
    void notifyProgressIfNeeded();

    // m_latencySeconds tracks effective output latency reported by the audio device.
    // getOutputLatencyInSamples() is JUCE's callback->speaker delay and already
    // includes the callback buffer on compliant drivers.
    std::atomic<float> m_latencySeconds  { 0.0f };
    std::atomic<float> m_visualLatencyCompensationSeconds { 0.0f };
    mutable LatencySnapshot m_lastLatencySnapshot;

    // Per-instance latency log state (avoids shared-static bug with multiple decks).
    int  m_lastLoggedEffectiveSamples  = -1;
    int  m_lastLoggedOutputRawSamples  = -1;
    int  m_lastLoggedBufferSamples     = -1;
    int  m_lastLoggedSampleRateRounded = -1;
    bool m_latencyLoggedNoDevice       = false;

    double m_pixelsPerSecond = WAVEFORM_POINTS_PER_SECOND * 1.5;
    engine::scratch::ScratchSession m_scratch;
    std::uint64_t m_pendingScratchReleaseGeneration = 0;
    bool m_scratchSnapReadPending = false;

    // Pre-roll countdown: when play is pressed while visual position is negative,
    // we advance the visual clock ourselves until it reaches 0, then start transport.

    engine::sync::SyncCoordinator& m_syncCoordinator;
    ControlClock& m_controlClock;
    const int m_deckIndex = 0;
    std::unique_ptr<engine::sync::DeckSyncController> m_syncController;
    ControlClock::Registration m_controlClockRegistration;
    bool m_lastPublishedSyncEnabled = false;
    bool m_lastPublishedSyncMaster = false;
};
