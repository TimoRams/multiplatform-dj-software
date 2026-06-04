#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "AlsaMidiOutput.h"
#endif

#include "Flx10LedController.h"

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <juce_audio_devices/juce_audio_devices.h>
#include <map>
#include <memory>
#include <cstdint>
#include <vector>

#if defined(Q_OS_LINUX)
#include <QProcess>
#endif

class DjEngine;
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
    BeatJump
};

struct MidiMappingEntry {
    QString paramId;
    MidiInteractionType interactionType = MidiInteractionType::EncoderAbsolute;
};

class MidiControllerManager : public QObject, public juce::MidiInputCallback
{
    Q_OBJECT

public:
    explicit MidiControllerManager(ParameterStore* store, QObject* parent = nullptr);
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

    void connectDecks(DjEngine* deckA, DjEngine* deckB);

    // QML Mapping Functions
    Q_INVOKABLE void startMidiLearn(const QString& parameterId);
    Q_INVOKABLE QString getMappingLabel(const QString& paramId) const;
    Q_INVOKABLE void clearLearnedMapping(const QString& paramId);
    Q_INVOKABLE void saveNativeMapping();
    Q_INVOKABLE void sendFlx10HotcuePaletteTest();
    Q_INVOKABLE void testFlx10LedOutput();

    Q_INVOKABLE bool isMappingInverted(const QString& paramId) const;
    Q_INVOKABLE void setMappingInverted(const QString& paramId, bool inverted);

    // Live MIDI monitor: last received event as a short human-readable string.
    // Updated on EVERY incoming message so the UI can show what the controller sends.
    Q_PROPERTY(QString lastMidiEvent READ lastMidiEvent NOTIFY lastMidiEventChanged)
    QString lastMidiEvent() const { return m_lastMidiEvent; }

signals:
    void mappingUpdated();
    void mappingInversionUpdated();
    void midiDevicesUpdated();
    void controllerListUpdated();
    void mappingListUpdated();
    void learnStarted(const QString& parameterId);
    void lastMidiEventChanged();

public slots:
    void onParameterChanged(const QString& id, float value);

private:
    // juce::MidiInputCallback overrides
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void dispatchParameterToStore(const QString& paramId, float value);
    void dispatchMidiParameterToStore(const QString& paramId, float value);

    ParameterStore* m_parameterStore = nullptr;
    DjEngine* m_deckA = nullptr;
    DjEngine* m_deckB = nullptr;
    bool m_cueAHeld = false;
    bool m_cueBHeld = false;
    bool m_jogATouched = false;
    bool m_jogBTouched = false;
    bool m_deckAShiftHeld = false;
    bool m_deckBShiftHeld = false;
    MidiPadMode m_deckAPadMode = MidiPadMode::HotCue;
    MidiPadMode m_deckBPadMode = MidiPadMode::HotCue;
    int m_deckAPadFxMomentary = -1;
    int m_deckBPadFxMomentary = -1;
    int m_deckAPadFxToggle = -1;
    int m_deckBPadFxToggle = -1;
    
    std::vector<std::unique_ptr<juce::MidiInput>> m_midiInputs;
    std::unique_ptr<juce::MidiOutput> m_midiOutput;

    // Mapping: Midi Note/CC Number -> parameter plus interaction semantics.
    std::map<int, MidiMappingEntry> m_midiToParam;
    std::map<int, bool> m_momentaryHeldByMsgId;
    std::map<int, int> m_scratchAbsoluteLastByMsgId;
    
    // Reverse Mapping for Output: Parameter ID -> Midi Note/CC Number
    std::map<QString, int> m_paramToMidi;

    // Available device identifiers and names
    std::vector<juce::String> m_availableInputDeviceIdentifiers;
    QStringList m_availableInputDeviceNames;
    std::vector<juce::String> m_availableOutputDeviceIdentifiers;
    QStringList m_availableOutputDeviceNames;
    int m_selectedMidiOutputIndex = -1;
    juce::String m_selectedMidiOutputIdentifier;
    QString m_selectedMidiOutputName;
    Flx10LedController m_flx10Leds;
    bool m_flx10RawLedTestRun = false;

    QString m_selectedController;
    QString m_selectedMappingFile;
    juce::MidiDeviceListConnection m_midiDeviceListConnection;

    std::map<QString, bool> m_paramInverted;

    // Learn State
    bool m_isLearning = false;
    QString m_learnParameterId;
    // 14-bit CC accumulation: paramId → last 7-bit MSB value
    std::map<QString, int> m_msbAccumulator;

    // Live MIDI monitor
    QString m_lastMidiEvent;

#if defined(Q_OS_LINUX)
    std::unique_ptr<QProcess> m_alsaInputMonitor;
    std::unique_ptr<AlsaMidiOutput> m_alsaMidiOutput;
    QString m_alsaMonitorBuffer;
#endif

    void refreshMidiDeviceCache();
    void populateFromAlsaFallback();
    bool isPseudoAlsaIdentifier(const juce::String& identifier) const;
    bool isPseudoAlsaOutputIdentifier(const juce::String& identifier) const;
    void startAlsaInputMonitor(const juce::String& pseudoIdentifier);
    void stopAlsaInputMonitor();
    void processDecodedMidiEvent(int msgId, float value, bool isNoteOff);
    void learnMapping(int msgId);
    void restoreSavedDeviceSelections();
    bool autoOpenFlx10MidiOutputIfNeeded();
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
    void applyFlx10FeedbackMappingElement(const QString& elementName, const QXmlStreamAttributes& attrs, Flx10LedMapping& mapping) const;
    void loadNativeMappingIfExists();
    QString nativeMappingFilePath() const;
    int parseMappingNumber(const QString& rawValue) const;
    int midiMessageIdFromStatusAndControl(int statusNo, int controlNo) const;
    MidiPadMode padModeForDeck(QChar deck) const;
    void setPadModeForDeck(QChar deck, MidiPadMode mode);
    void clearPadFxState(QChar deck, DjEngine* engine);
    void stopPadFxToggle(DjEngine* engine, int padIndex);
    void handlePerformancePad(QChar deck, DjEngine* engine, int padIndex, bool pressed, bool clearRequest);
    void refreshAllDeckLeds();
    void refreshDeckLeds(QChar deck, DjEngine* engine);
    void refreshTransportAndLoopLeds(QChar deck, DjEngine* engine);
    void refreshHotCueLeds(QChar deck, DjEngine* engine);
    void refreshPadModeLeds(QChar deck);
    bool sendMidiShort(int statusNo, int controlNo, int value, const QString& messageType = QStringLiteral("raw"));
    bool sendMidiMessageWithDebug(const juce::MidiMessage& message, const QString& messageType);
    void sendMidiNoteLed(int statusNo, int noteNo, int value);
    void sendMappedNoteLed(const QString& paramId, bool on, int onValue = 0x7f);
    int hotCueStatusForDeck(int deck) const;
    int hotCueStatusForDeck(QChar deck) const;
    int hotCueLedValueForColor(const QString& color) const;
};
