#include "MidiControllerManager.h"
#include "ParameterStore.h"
#include <QDebug>

MidiControllerManager::MidiControllerManager(ParameterStore* store, QObject* parent)
    : QObject(parent), m_parameterStore(store)
{
    if (m_parameterStore) {
        connect(m_parameterStore, &ParameterStore::parameterChanged,
                this, &MidiControllerManager::onParameterChanged);
    }
}

MidiControllerManager::~MidiControllerManager()
{
    if (m_midiInput) m_midiInput->stop();
}

QStringList MidiControllerManager::getAvailableMidiDevices()
{
    QStringList list;
    auto devices = juce::MidiInput::getAvailableDevices();
    
    qDebug() << "JUCE fand" << devices.size() << "MIDI-Geräte (Input)";
    
    m_availableInputDeviceIdentifiers.clear();
    
    for (const auto& dev : devices) {
        qDebug() << "Input Device:" << QString::fromStdString(dev.name.toStdString());
        list << QString::fromStdString(dev.name.toStdString());
        m_availableInputDeviceIdentifiers.push_back(dev.identifier);
    }
    
    auto outDevices = juce::MidiOutput::getAvailableDevices();
    qDebug() << "JUCE fand" << outDevices.size() << "MIDI-Geräte (Output)";
    m_availableOutputDeviceIdentifiers.clear();
    for (const auto& dev : outDevices) {
        qDebug() << "Output Device:" << QString::fromStdString(dev.name.toStdString());
        m_availableOutputDeviceIdentifiers.push_back(dev.identifier);
    }
    
    return list;
}

void MidiControllerManager::selectMidiDevice(int index)
{
    // Close existing handles first
    if (m_midiInput) {
        m_midiInput->stop();
        m_midiInput.reset();
    }
    m_midiOutput.reset();

    if (index >= 0 && index < m_availableInputDeviceIdentifiers.size()) {
        auto identifier = m_availableInputDeviceIdentifiers[index];
        m_midiInput = juce::MidiInput::openDevice(identifier, this);
        if (m_midiInput) {
            m_midiInput->start();
            qDebug() << "Opened and started MIDI Input:" << QString::fromStdString(identifier.toStdString());
        } else {
            qWarning() << "Failed to open MIDI Input:" << QString::fromStdString(identifier.toStdString());
        }
    }

    if (index >= 0 && index < m_availableOutputDeviceIdentifiers.size()) {
        auto identifier = m_availableOutputDeviceIdentifiers[index];
        m_midiOutput = juce::MidiOutput::openDevice(identifier);
        if (m_midiOutput) {
            qDebug() << "Opened MIDI Output:" << QString::fromStdString(identifier.toStdString());
        } else {
            qWarning() << "Failed to open MIDI Output:" << QString::fromStdString(identifier.toStdString());
        }
    }
}

void MidiControllerManager::startMidiLearn(const QString& parameterId)
{
    m_learnParameterId = parameterId;
    m_isLearning = true;
    qDebug() << "Started MIDI learn for" << parameterId;
}

void MidiControllerManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    // Extract a unique integer identifier for the message type
    // We'll use the note number for NoteOn/NoteOff, or CC number for ControlChange
    int msgId = -1;
    float value = 0.0f;

    if (message.isNoteOnOrOff()) {
        msgId = message.getNoteNumber();
        value = message.getFloatVelocity();
    } else if (message.isController()) {
        // Offset CCs by 1000 so they don't collide with Note numbers
        msgId = message.getControllerNumber() + 1000; 
        value = message.getControllerValue() / 127.0f;
    } else {
        return; // Ignore other message types for now
    }

    if (m_isLearning && !message.isNoteOff()) { // Don't learn on note off
        // Wait, what if it's a CC? They stream rapidly. Just take the first one.
        m_midiToParam[msgId] = m_learnParameterId;
        m_paramToMidi[m_learnParameterId] = msgId;
        
        m_isLearning = false;
        qDebug() << "Learned! Mapped ID" << msgId << "to" << m_learnParameterId;
        
        // We must emit the signal on the main thread, as this is the JUCE audio/midi thread
        QMetaObject::invokeMethod(this, "mappingUpdated", Qt::QueuedConnection);
        return;
    }

    // Normal processing
    auto it = m_midiToParam.find(msgId);
    if (it != m_midiToParam.end()) {
        const QString& paramId = it->second;
        if (m_parameterStore) {
            // Again, ensure thread safety when calling ParameterStore which might update QML
            QMetaObject::invokeMethod(m_parameterStore, "setParameter", Qt::QueuedConnection,
                                      Q_ARG(QString, paramId),
                                      Q_ARG(float, value));
        }
    }
}

void MidiControllerManager::onParameterChanged(const QString& id, float value)
{
    if (!m_midiOutput) return;

    auto it = m_paramToMidi.find(id);
    if (it != m_paramToMidi.end()) {
        int msgId = it->second;
        juce::MidiMessage msg;
        
        if (msgId >= 1000) {
            // It's a CC
            msg = juce::MidiMessage::controllerEvent(1, msgId - 1000, static_cast<int>(value * 127));
        } else {
            // It's a Note
            if (value > 0.0f) {
                msg = juce::MidiMessage::noteOn(1, msgId, value);
            } else {
                msg = juce::MidiMessage::noteOff(1, msgId, 0.0f);
            }
        }
        
        m_midiOutput->sendMessageNow(msg);
    }
}
