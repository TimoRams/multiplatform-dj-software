#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <juce_audio_devices/juce_audio_devices.h>
#include <map>
#include <memory>

class ParameterStore;

class MidiControllerManager : public QObject, public juce::MidiInputCallback
{
    Q_OBJECT

public:
    explicit MidiControllerManager(ParameterStore* store, QObject* parent = nullptr);
    ~MidiControllerManager() override;

    Q_INVOKABLE QStringList getAvailableMidiDevices();
    Q_INVOKABLE void selectMidiDevice(int index);

    // QML Mapping Functions
    Q_INVOKABLE void startMidiLearn(const QString& parameterId);
    
signals:
    void mappingUpdated();

public slots:
    void onParameterChanged(const QString& id, float value);

private:
    // juce::MidiInputCallback overrides
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    ParameterStore* m_parameterStore = nullptr;
    
    std::unique_ptr<juce::MidiInput> m_midiInput;
    std::unique_ptr<juce::MidiOutput> m_midiOutput;

    // Mapping: Midi Note/CC Number -> Parameter ID
    std::map<int, QString> m_midiToParam;
    
    // Reverse Mapping for Output: Parameter ID -> Midi Note/CC Number
    std::map<QString, int> m_paramToMidi;

    // Available device identifiers
    std::vector<juce::String> m_availableInputDeviceIdentifiers;
    std::vector<juce::String> m_availableOutputDeviceIdentifiers;

    // Learn State
    bool m_isLearning = false;
    QString m_learnParameterId;
};
