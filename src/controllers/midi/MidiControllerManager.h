#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "AlsaMidiInput.h"
#include "AlsaMidiLineParser.h"
#include "AlsaMidiOutput.h"
#endif

#include "feedback/MidiFeedbackController.h"
#include "app/ControlClock.h"
#include "controllers/flx10/Flx10JogRouter.h"
#include "Midi14BitAccumulator.h"
#include "MidiEchoGuard.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QStringList>
#include <QVariantMap>
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <vector>

#if defined(Q_OS_LINUX)
#include <QProcess>
#endif

class DjEngine;
class FxManager;
class ParameterStore;
class QXmlStreamAttributes;

enum class MidiInteractionType {
    Momentary,
    Toggle,
    EncoderRelative,
    EncoderAbsolute,
    Fader
};

enum class MidiPadMode {
    HotCue,
    PadFx,
    BeatJump,
    Sampler
};

enum class MidiBeatFxTarget {
    DeckA,
    DeckB,
    DeckC,
    DeckD,
    Master,
    Mic,
    Sampler
};

struct MidiMappingEntry {
    QString paramId;
    MidiInteractionType interactionType = MidiInteractionType::EncoderAbsolute;
};

class MidiControllerManager : public QObject, public juce::MidiInputCallback
{
    Q_OBJECT

public:
    MidiControllerManager(ParameterStore* store, ControlClock& controlClock,
                          QObject* parent = nullptr);
    ~MidiControllerManager() override;

    Q_INVOKABLE QStringList getAvailableMidiInputDevices();
    Q_INVOKABLE QStringList getAvailableMidiOutputDevices();
    Q_INVOKABLE QStringList getAvailableMidiDevices();

    Q_INVOKABLE void selectMidiInputDevice(int index);
    Q_INVOKABLE void selectMidiOutputDevice(int index);
    Q_INVOKABLE void selectMidiDevice(int index);

    Q_INVOKABLE int getSelectedMidiInputIndex() const;
    Q_INVOKABLE int getSelectedMidiOutputIndex() const;
    Q_INVOKABLE int getSelectedMidiDeviceIndex() const;

    Q_INVOKABLE QString getMappingsDirectoryPath() const;
    Q_INVOKABLE QStringList getAvailableMappingFiles();
    Q_INVOKABLE QString getSettingsDirectoryPath() const;
    Q_INVOKABLE bool openSettingsDirectory() const;
    Q_INVOKABLE bool openMappingsDirectory() const;
    Q_INVOKABLE QStringList getAvailableControllers();
    Q_INVOKABLE void selectController(const QString& controllerName);
    Q_INVOKABLE QString getSelectedController() const;

    Q_INVOKABLE QStringList getAvailableMappingsForSelectedController();
    Q_INVOKABLE void selectMapping(const QString& mappingFileName);
    Q_INVOKABLE QString getSelectedMapping() const;

    Q_INVOKABLE void refreshMidiAndMappings();

    void connectDecks(DjEngine* deckA, DjEngine* deckB,
                      DjEngine* deckC, DjEngine* deckD);
    void connectFxManager(FxManager* fxManager);

    // Stop MIDI I/O and disconnect callbacks before QML/engine teardown.
    void shutdown();

    // QML Mapping Functions
    Q_INVOKABLE void startMidiLearn(const QString& parameterId);
    Q_INVOKABLE QString getMappingLabel(const QString& paramId) const;
    Q_INVOKABLE void clearLearnedMapping(const QString& paramId);
    Q_INVOKABLE void saveNativeMapping();
    Q_INVOKABLE void sendFlx10HotcuePaletteTest();
    Q_INVOKABLE void testFlx10LedOutput();
    Q_INVOKABLE void selectPerformancePadMode(const QString& deckId, int mode);
    Q_INVOKABLE void setPerformancePadPressed(const QString& deckId, int padIndex, bool pressed);
    Q_INVOKABLE void clearPerformancePad(const QString& deckId, int padIndex);
    Q_INVOKABLE bool consumePerformancePadPlayLatch(const QString& deckId);
    Q_INVOKABLE int performancePadFxMomentary(const QString& deckId) const;
    Q_INVOKABLE int performancePadFxToggle(const QString& deckId) const;

    Q_INVOKABLE bool isMappingInverted(const QString& paramId) const;
    Q_INVOKABLE void setMappingInverted(const QString& paramId, bool inverted);

    // Live MIDI monitor: last received event as a short human-readable string.
    // High-rate wheel traffic is display-throttled so it cannot starve the UI.
    Q_PROPERTY(QString lastMidiEvent READ lastMidiEvent NOTIFY lastMidiEventChanged)
    Q_PROPERTY(int deckAPadMode READ deckAPadMode NOTIFY deckAPadModeChanged)
    Q_PROPERTY(int deckBPadMode READ deckBPadMode NOTIFY deckBPadModeChanged)
    Q_PROPERTY(bool beatFxActive READ beatFxActive NOTIFY beatFxActiveChanged)
    QString lastMidiEvent() const { return m_lastMidiEvent; }
    int deckAPadMode() const noexcept { return static_cast<int>(m_deckAPadMode); }
    int deckBPadMode() const noexcept { return static_cast<int>(m_deckBPadMode); }
    bool beatFxActive() const noexcept { return m_beatFxActive; }

signals:
    void mappingUpdated();
    void mappingInversionUpdated();
    void midiDevicesUpdated();
    void controllerListUpdated();
    void mappingListUpdated();
    void learnStarted(const QString& parameterId);
    void lastMidiEventChanged();
    void deckAPadModeChanged();
    void deckBPadModeChanged();
    void performancePadStateChanged(const QString& deckId);
    void beatFxActiveChanged();
    void libraryViewToggleRequested();

public slots:
    void onParameterChanged(const QString& id, float value);

private:
    // Sentinel identifier for "listen on every input at once" rather than one
    // named device. Lives here because this class owns the device lists it is
    // stored in, and enumeration, mapping and the FLX10 bridge all compare
    // against it.
    static const juce::String kAllMidiInputsIdentifier;
    [[nodiscard]] static int indexOfIdentifier(const std::vector<juce::String>& identifiers,
                                               const juce::String& needle) noexcept;

    // juce::MidiInputCallback overrides
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    enum class ParameterStoreDispatch {
        Standard,
        Midi
    };
    void dispatchToStore(const QString& paramId, float value, ParameterStoreDispatch method);

    std::atomic<bool> m_shutdownComplete { false };

    ParameterStore* m_parameterStore = nullptr;
    DjEngine* m_deckA = nullptr;
    DjEngine* m_deckB = nullptr;
    DjEngine* m_deckC = nullptr;
    DjEngine* m_deckD = nullptr;
    QPointer<FxManager> m_fxManager;
    bool m_cueAHeld = false;
    bool m_cueBHeld = false;
    bool m_jogATouched = false;
    bool m_jogBTouched = false;
    flx10::Flx10JogRouter m_jogARouter;
    flx10::Flx10JogRouter m_jogBRouter;
    QTimer m_startupRefreshTimer;
    QTimer m_14BitFallbackTimer;
    bool m_deckAShiftHeld = false;
    bool m_deckBShiftHeld = false;
    std::array<bool, 2> m_tempoRawInputSeen { false, false };
    std::array<bool, 2> m_tempoInputSeen { false, false };
    bool m_deckASlipReverseHeld = false;
    bool m_deckBSlipReverseHeld = false;
    bool m_deckAReverseBeforeSlip = false;
    bool m_deckBReverseBeforeSlip = false;
    bool m_deckASlipBeforeReverse = false;
    bool m_deckBSlipBeforeReverse = false;
    MidiPadMode m_deckAPadMode = MidiPadMode::HotCue;
    MidiPadMode m_deckBPadMode = MidiPadMode::HotCue;
    int m_deckAPadFxMomentary = -1;
    int m_deckBPadFxMomentary = -1;
    int m_deckAPadFxToggle = -1;
    int m_deckBPadFxToggle = -1;
    struct HotCueHoldState {
        int padIndex = -1;
        double returnPositionSeconds = 0.0;
        bool returnOnRelease = false;
    };
    HotCueHoldState m_deckAHotCueHold;
    HotCueHoldState m_deckBHotCueHold;
    std::array<bool, 3> m_deckAFxSlotsEnabled = { false, false, false };
    std::array<bool, 3> m_deckBFxSlotsEnabled = { false, false, false };
    bool m_beatFxActive = false;
    float m_beatFxLevelDepth = 0.5f;
    int m_beatFxPosition = 1;
    MidiBeatFxTarget m_beatFxTarget = MidiBeatFxTarget::DeckA;
    bool m_applyingBeatFxRouting = false;
    // Whether the physical LEVEL/DEPTH knob has reported a position yet. Until
    // it has, its resting value is unknown and must not be taken as "the user
    // asked for zero mix" — engaging with that would switch the effect on
    // inaudibly, which is indistinguishable from it not switching on at all.
    bool m_beatFxDepthFromKnob = false;
    float m_beatFxDivision = 0.25f;   // 1/4 beat, the usual power-up default
    QTimer m_beatFxBlinkTimer;
    bool m_beatFxBlinkOn = false;

    std::vector<std::unique_ptr<juce::MidiInput>> m_midiInputs;
    std::unique_ptr<juce::MidiOutput> m_midiOutput;

    // Mapping: Midi Note/CC Number -> parameter plus interaction semantics.
    std::map<int, MidiMappingEntry> m_midiToParam;
    std::map<int, bool> m_momentaryHeldByMsgId;
    std::map<int, int> m_scratchAbsoluteLastByMsgId;
    
    // Reverse Mapping for Output: Parameter ID -> Midi Note/CC Number
    std::map<QString, int> m_paramToMidi;
    std::map<int, int> m_lastMidiShortValues;
    // Keeps this application's own lamp writes from being read back in as
    // button presses. Shared by every input path, so it also covers the plain
    // JUCE inputs, not just the ALSA monitors.
    midi_internal::MidiOutputEchoGuard m_outputEchoGuard;

    // Available device identifiers and names
    std::vector<juce::String> m_availableInputDeviceIdentifiers;
    QStringList m_availableInputDeviceNames;
    std::vector<juce::String> m_availableOutputDeviceIdentifiers;
    QStringList m_availableOutputDeviceNames;
    std::vector<juce::String> m_availableControllerDeviceIdentifiers;
    std::vector<int> m_availableControllerInputIndexes;
    QStringList m_availableControllerDeviceNames;
    int m_selectedMidiOutputIndex = -1;
    juce::String m_selectedMidiOutputIdentifier;
    QString m_selectedMidiOutputName;
    MidiFeedbackController m_midiFeedback;
    ControlClock::Registration m_controlClockRegistration;
    bool m_flx10RawLedTestRun = false;

    QString m_selectedController;
    QString m_selectedMappingFile;
    juce::MidiDeviceListConnection m_midiDeviceListConnection;

    std::map<QString, bool> m_paramInverted;

    // Learn State
    bool m_isLearning = false;
    QString m_learnParameterId;
    // 14-bit CC accumulation accepts both MSB-first and FLX10 LSB-first delivery.
    std::map<QString, midi_internal::Midi14BitAccumulator> m_14BitAccumulators;
    std::map<QString, midi_internal::MidiUnpairedMsbGate> m_channelFaderMsbGates;
    std::map<QString, float> m_pending14BitMsbFallbacks;
    QMetaObject::Connection m_deckActionsConnection;

    // Live MIDI monitor
    QString m_lastMidiEvent;
    bool m_midiTraceEnabled = false;
    double m_nextMidiMonitorUpdateSeconds = 0.0;
    double m_nextControllerConnectionCheckSeconds = 0.0;
    double m_nextControllerFeedbackResyncSeconds = 0.0;

    struct PendingMidiEvent {
        int msgId = -1;
        float rawEncodedValue = 0.0f;
        double timestampSeconds = 0.0;
        bool noteOff = false;
    };
    static constexpr std::size_t kPendingMidiCapacity = 16'384;
    static constexpr std::size_t kMidiDrainBatchSize = 256;
    std::array<PendingMidiEvent, kPendingMidiCapacity> m_pendingMidiEvents {};
    std::mutex m_pendingMidiMutex;
    std::size_t m_pendingMidiHead = 0;
    std::size_t m_pendingMidiCount = 0;
    bool m_midiDrainScheduled = false;
    std::atomic<std::uint64_t> m_droppedMidiEvents { 0 };

#if defined(Q_OS_LINUX)
    std::vector<std::unique_ptr<QProcess>> m_alsaInputMonitors;
    std::vector<std::unique_ptr<AlsaMidiInput>> m_alsaDirectInputs;
    std::unique_ptr<AlsaMidiOutput> m_alsaMidiOutput;
    std::map<QProcess*, QString> m_alsaMonitorBuffers;
    std::map<QString, int> m_alsaFaderSourceLogCounts;
    QString m_primaryAlsaInputPort;
    // Drops the second copy of a button event that the controller mirrors onto
    // more than one of its ALSA ports; without it every press counts twice.
    midi_internal::AlsaCrossPortButtonFilter m_alsaButtonDuplicates;
#endif

    bool refreshMidiDeviceCache();
    bool refreshMidiDevices(bool notifyMappingList);
    void populateFromAlsaFallback();
    void rebuildControllerDeviceCache();
    bool isPseudoAlsaIdentifier(const juce::String& identifier) const;
    bool isPseudoAlsaOutputIdentifier(const juce::String& identifier) const;
    void startAlsaInputMonitor(const juce::String& pseudoIdentifier);
    void stopAlsaInputMonitor();
    bool hasActiveMidiInput() const;
    void processDecodedMidiEvent(int msgId, float value, bool isNoteOff,
                                 double eventTimestampSeconds = 0.0);
    void enqueueRawMidiEvent(int msgId, float rawEncodedValue, bool noteOff,
                             double eventTimestampSeconds);
    void drainRawMidiEvents();
    void processRawMidiEvent(int msgId, float rawEncodedValue, bool noteOff,
                             double eventTimestampSeconds);
    bool dispatchFlx10JogAction(const QString& paramId, float value,
                                double eventTimestampSeconds);
    void resetHighResolutionControlState();
    void learnMapping(int msgId);
    void restoreSavedDeviceSelections();
    bool autoOpenFlx10MidiOutputIfNeeded();
    void runControllerHousekeeping(double monotonicSeconds);
    void forceFlx10FeedbackResync();
    void openMidiInputByIdentifier(const juce::String& identifier);
    void openMidiOutputByIdentifier(const juce::String& identifier);
    int findMatchingMidiOutputIndexForInput(int inputIndex) const;
    int findMidiOutputIndexByName(const QString& nameOrIdentifier) const;
    void logAvailableMidiPorts() const;
    bool shouldUseFlx10Feedback() const;
    void startFlx10OutputSession();
    void stopFlx10OutputSession();

    QString normalizeControllerKeyFromXmlBase(const QString& baseName) const;
    QString normalizeControllerKeyFromJsBase(const QString& baseName) const;
    QStringList getAvailableXmlMappingFilesForController(const QString& controllerName) const;
    bool loadBrockDjXmlMapping(const QString& mappingFileName);
    void applyMidiFeedbackMappingElement(const QString& elementName, const QXmlStreamAttributes& attrs, MidiFeedbackMapping& mapping) const;
    void loadNativeMappingIfExists();
    QString nativeMappingFilePath() const;
    int parseMappingNumber(const QString& rawValue) const;
    int midiMessageIdFromStatusAndControl(int statusNo, int controlNo) const;
    MidiPadMode padModeForDeck(QChar deck) const;
    void setPadModeForDeck(QChar deck, MidiPadMode mode);
    void clearPadFxState(QChar deck, DjEngine* engine);
    void stopPadFxToggle(DjEngine* engine, int padIndex);
    void releaseHeldHotCue(QChar deck, DjEngine* engine);
    void handleCuePadHold(QChar deck, DjEngine* engine, int padIndex,
                          bool pressed, bool storeIfEmpty);
    void handlePerformancePad(QChar deck, DjEngine* engine, MidiPadMode mode,
                              int padIndex, bool pressed, bool clearRequest);
    void refreshAllDeckLeds();
    void refreshDeckLeds(QChar deck, DjEngine* engine);
    void refreshTransportAndLoopLeds(QChar deck, DjEngine* engine);
    void refreshHotCueLeds(QChar deck, DjEngine* engine);
    void refreshPadModeLeds(QChar deck);
    void refreshPerformancePadLeds(QChar deck, DjEngine* engine);
    // Monitoring LEDs (MASTER CUE) mirror the engine, which may already have
    // been switched on from the UI before the controller was plugged in.
    void refreshMixerLeds();
    // Whether an FX update may change the unit's engage state. Only the BEAT FX
    // ON button may (Write); everything else — effect selector, routing channel,
    // level knob, beat division — has to leave "is FX on?" exactly as the user
    // set it, wherever they set it (Follow).
    enum class BeatFxEngage { Follow, Write };
    void applyBeatFxState(BeatFxEngage engage = BeatFxEngage::Follow);
    void refreshFxLeds();
    // Beat-synced timing for the hardware FX unit: turns the current beat
    // division into an echo/delay length and pushes it to whatever the unit is
    // routed to (an individual channel or the master bus).
    void pushBeatFxTiming();
    void stepBeatFxDivision(int direction);
    // Professional DJ hardware pulses the FX ON button while the effect is
    // engaged; the LED itself has no blink mode, so we drive it from a timer.
    void updateBeatFxBlink();
    bool sendMidiShort(int statusNo, int controlNo, int value, const QString& messageType = QStringLiteral("raw"));
    bool sendMidiMessageWithDebug(const juce::MidiMessage& message, const QString& messageType);
    void sendMidiNoteLed(int statusNo, int noteNo, int value);
    void sendMappedNoteLed(const QString& paramId, bool on, int onValue = 0x7f);
    int hotCueStatusForDeck(int deck) const;
    int hotCueStatusForDeck(QChar deck) const;
    int hotCueLedValueForColor(const QString& color) const;
};
