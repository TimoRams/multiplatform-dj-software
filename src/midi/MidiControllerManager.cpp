#include "MidiControllerManager.h"
#include "MidiControllerManagerInternal.h"

using namespace midi_internal;

#include "ParameterStore.h"
#include "SettingsManager.h"
#include "fx/FxManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QtGlobal>


MidiControllerManager::MidiControllerManager(ParameterStore* store, ControlClock& controlClock,
                                             QObject* parent)
    : QObject(parent),
      m_parameterStore(store),
      // Not a QObject child — m_midiFeedback is a C++ member; parenting would make
      // ~QObject delete it again after the member destructor (UAF on macOS quit).
      m_midiFeedback(nullptr)
{
    m_midiTraceEnabled = qEnvironmentVariableIntValue("BROCKDJ_MIDI_TRACE") > 0;

    if (m_parameterStore) {
        connect(m_parameterStore, &ParameterStore::parameterChanged,
                this, &MidiControllerManager::onParameterChanged);
    }

    m_midiDeviceListConnection = juce::MidiDeviceListConnection::make([this]
    {
        QMetaObject::invokeMethod(this, [this]()
        {
            if (m_shutdownComplete.load(std::memory_order_acquire))
                return;
            refreshMidiAndMappings();
            if (!hasActiveMidiInput())
                restoreSavedDeviceSelections();
        }, Qt::QueuedConnection);
    });

    m_midiFeedback.setMidiSender([this](uint8_t status, uint8_t data1, uint8_t data2, const QString& type)
    {
        return sendMidiShort(status, data1, data2, type);
    });
    ControlClock::Callbacks clockCallbacks;
    clockCallbacks.feedback = [this](const ControlTickContext&) {
        m_midiFeedback.onControlClockFeedbackTick();
    };
    m_controlClockRegistration = controlClock.registerCallbacks(std::move(clockCallbacks));

    m_selectedController = SettingsManager::getInstance().getSelectedController();
    m_selectedMappingFile = SettingsManager::getInstance().getSelectedMappingFile();

    refreshMidiAndMappings();
    if (!m_selectedMappingFile.isEmpty())
        loadBrockDjXmlMapping(m_selectedMappingFile);
    else
        loadNativeMappingIfExists();
    // Build the dispatch table before starting an input. Some controllers send
    // their current high-resolution fader state immediately on subscription.
    restoreSavedDeviceSelections();

    autoOpenFlx10MidiOutputIfNeeded();

    m_startupRefreshTimer.setSingleShot(true);
    connect(&m_startupRefreshTimer, &QTimer::timeout, this, [this]()
    {
        if (m_shutdownComplete.load(std::memory_order_acquire))
            return;
        refreshMidiAndMappings();
        if (!hasActiveMidiInput())
            restoreSavedDeviceSelections();
        autoOpenFlx10MidiOutputIfNeeded();
    });
    m_startupRefreshTimer.start(750);
}

void MidiControllerManager::shutdown()
{
    // Guard with a process-wide flag first — during macOS quit, DeferredDelete can
    // destroy this object while a stale unique_ptr still holds the same address.
    static std::atomic<bool> s_processShutdownDone { false };
    if (s_processShutdownDone.exchange(true, std::memory_order_acq_rel))
        return;

    if (m_shutdownComplete.exchange(true, std::memory_order_acq_rel))
        return;

    // Stop feedback + Qt signal delivery before JUCE/CoreMIDI teardown — device
    // close can pump the Cocoa run loop and re-enter timer/MIDI handlers.
    m_midiFeedback.prepareForShutdown();
    m_controlClockRegistration.reset();
    QCoreApplication::removePostedEvents(this);
    QCoreApplication::removePostedEvents(&m_midiFeedback);

    QObject::disconnect(&m_startupRefreshTimer, nullptr, this, nullptr);

    if (m_deckActionsConnection)
        QObject::disconnect(m_deckActionsConnection);
    m_deckActionsConnection = {};

    if (m_parameterStore)
        QObject::disconnect(m_parameterStore, nullptr, this, nullptr);

    if (m_fxManager)
        QObject::disconnect(m_fxManager.data(), nullptr, this, nullptr);
    m_fxManager = nullptr;

    if (m_deckA)
        QObject::disconnect(m_deckA, nullptr, this, nullptr);
    if (m_deckB && m_deckB != m_deckA)
        QObject::disconnect(m_deckB, nullptr, this, nullptr);
    m_deckA = nullptr;
    m_deckB = nullptr;

    m_midiDeviceListConnection = juce::MidiDeviceListConnection{};

    for (auto& input : m_midiInputs) {
        if (input)
            input->stop();
    }
    m_midiInputs.clear();

#if defined(Q_OS_LINUX)
    stopAlsaInputMonitor();
    if (m_alsaMidiOutput)
        m_alsaMidiOutput.reset();
#endif

    if (m_midiOutput)
        m_midiOutput.reset();
}

MidiControllerManager::~MidiControllerManager()
{
    if (!m_shutdownComplete.load(std::memory_order_acquire))
        shutdown();
}
void MidiControllerManager::refreshMidiAndMappings()
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    refreshMidiDeviceCache();

    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    emit midiDevicesUpdated();
    emit controllerListUpdated();
    emit mappingListUpdated();
}
