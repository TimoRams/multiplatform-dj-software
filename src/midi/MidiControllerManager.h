#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <juce_audio_devices/juce_audio_devices.h>
#include <map>
#include <memory>
#include <vector>

#if defined(Q_OS_LINUX)
#include <QProcess>
#endif

class DjEngine;
class ParameterStore;

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

    ParameterStore* m_parameterStore = nullptr;
    DjEngine* m_deckA = nullptr;
    DjEngine* m_deckB = nullptr;
    bool m_jogATouched = false;
    bool m_jogBTouched = false;
    
    std::vector<std::unique_ptr<juce::MidiInput>> m_midiInputs;
    std::unique_ptr<juce::MidiOutput> m_midiOutput;

    // Mapping: Midi Note/CC Number -> Parameter ID
    std::map<int, QString> m_midiToParam;
    
    // Reverse Mapping for Output: Parameter ID -> Midi Note/CC Number
    std::map<QString, int> m_paramToMidi;

    // Available device identifiers and names
    std::vector<juce::String> m_availableInputDeviceIdentifiers;
    QStringList m_availableInputDeviceNames;
    std::vector<juce::String> m_availableOutputDeviceIdentifiers;
    QStringList m_availableOutputDeviceNames;

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
    QString m_alsaMonitorBuffer;
#endif

    void refreshMidiDeviceCache();
    void populateFromAlsaFallback();
    bool isPseudoAlsaIdentifier(const juce::String& identifier) const;
    void startAlsaInputMonitor(const juce::String& pseudoIdentifier);
    void stopAlsaInputMonitor();
    void processDecodedMidiEvent(int msgId, float value, bool isNoteOff);
    void learnMapping(int msgId);
    void restoreSavedDeviceSelections();
    void openMidiInputByIdentifier(const juce::String& identifier);
    void openMidiOutputByIdentifier(const juce::String& identifier);

    QString normalizeControllerKeyFromXmlBase(const QString& baseName) const;
    QString normalizeControllerKeyFromJsBase(const QString& baseName) const;
    QStringList getAvailableXmlMappingFilesForController(const QString& controllerName) const;
    bool loadMixxxXmlMapping(const QString& mappingFileName);
    void loadNativeMappingIfExists();
    QString nativeMappingFilePath() const;
    int parseMixxxNumber(const QString& rawValue) const;
    QString mapMixxxControlToInternalParam(const QString& group, const QString& key) const;
};
