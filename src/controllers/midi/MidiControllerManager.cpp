#include "MidiControllerManager.h"

#include "ParameterStore.h"
#include "app/SettingsManager.h"
#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "deck/DjEngine.h"
#include "fx/FxManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QtGlobal>

#include <algorithm>
#include <iterator>

const juce::String MidiControllerManager::kAllMidiInputsIdentifier { "__all_midi_inputs__" };

int MidiControllerManager::indexOfIdentifier(const std::vector<juce::String>& identifiers,
                                             const juce::String& needle) noexcept
{
    const auto it = std::ranges::find(identifiers, needle);
    return it == identifiers.end() ? -1 : static_cast<int>(std::distance(identifiers.begin(), it));
}


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

    m_14BitFallbackTimer.setSingleShot(true);
    m_14BitFallbackTimer.setInterval(4);
    connect(&m_14BitFallbackTimer, &QTimer::timeout, this, [this]
    {
        auto pending = std::move(m_pending14BitMsbFallbacks);
        m_pending14BitMsbFallbacks.clear();
        for (const auto& [paramId, value] : pending)
            dispatchToStore(paramId, value, ParameterStoreDispatch::Standard);
    });

    m_midiDeviceListConnection = juce::MidiDeviceListConnection::make([this]
    {
        QMetaObject::invokeMethod(this, [this]()
        {
            if (m_shutdownComplete.load(std::memory_order_acquire))
                return;
            const bool devicesChanged = refreshMidiDevices(false);
            if (devicesChanged || !hasActiveMidiInput())
                restoreSavedDeviceSelections();
        }, Qt::QueuedConnection);
    });

    m_midiFeedback.setMidiSender([this](uint8_t status, uint8_t data1, uint8_t data2, const QString& type)
    {
        return sendMidiShort(status, data1, data2, type);
    });
    ControlClock::Callbacks clockCallbacks;
    clockCallbacks.feedback = [this](const ControlTickContext& context) {
        m_midiFeedback.onControlClockFeedbackTick(context.deltaSeconds);
        updateKeyShiftModeBlink(context.monotonicSeconds);
    };
    clockCallbacks.housekeeping = [this](const ControlTickContext& context) {
        runControllerHousekeeping(context.monotonicSeconds);
    };
    m_controlClockRegistration = controlClock.registerCallbacks(std::move(clockCallbacks));

    m_selectedController = SettingsManager::getInstance().getSelectedController();
    m_selectedMappingFile = SettingsManager::getInstance().getSelectedMappingFile();

    refreshMidiDevices(false);
    if (!m_selectedMappingFile.isEmpty())
        loadBrockDjXmlMapping(m_selectedMappingFile);
    else
        loadNativeMappingIfExists();
    setNativeFlx10ScratchEnabled(flx10::isBuiltInMapping(m_selectedMappingFile));
    // Build the dispatch table before starting an input. Some controllers send
    // their current high-resolution fader state immediately on subscription.
    restoreSavedDeviceSelections();

    autoOpenFlx10MidiOutputIfNeeded();

    // The FX ON lamp is held steady while the effect is engaged instead of
    // being pulsed. A pulse meant a lamp write every 180 ms for as long as FX
    // was on, and every one of those writes is a chance for the controller to
    // loop it back in as a phantom press. Steady also reads unambiguously: the
    // button is lit exactly while the unit is engaged.
    m_beatFxBlinkTimer.stop();

    m_startupRefreshTimer.setSingleShot(true);
    connect(&m_startupRefreshTimer, &QTimer::timeout, this, [this]()
    {
        if (m_shutdownComplete.load(std::memory_order_acquire))
            return;
        const bool devicesChanged = refreshMidiDevices(false);
        if (devicesChanged || !hasActiveMidiInput())
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

    cancelBeatJumpSearch();
    setNativeFlx10ScratchEnabled(false);

    // Stop feedback + Qt signal delivery before JUCE/CoreMIDI teardown — device
    // close can pump the Cocoa run loop and re-enter timer/MIDI handlers.
    m_midiFeedback.prepareForShutdown();
    m_controlClockRegistration.reset();
    QCoreApplication::removePostedEvents(this);
    QCoreApplication::removePostedEvents(&m_midiFeedback);

    QObject::disconnect(&m_startupRefreshTimer, nullptr, this, nullptr);
    m_beatFxBlinkTimer.stop();
    QObject::disconnect(&m_beatFxBlinkTimer, nullptr, this, nullptr);
    m_14BitFallbackTimer.stop();
    QObject::disconnect(&m_14BitFallbackTimer, nullptr, this, nullptr);
    resetHighResolutionControlState();

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
    if (m_deckC && m_deckC != m_deckA && m_deckC != m_deckB)
        QObject::disconnect(m_deckC, nullptr, this, nullptr);
    if (m_deckD && m_deckD != m_deckA && m_deckD != m_deckB && m_deckD != m_deckC)
        QObject::disconnect(m_deckD, nullptr, this, nullptr);
    m_deckA = nullptr;
    m_deckB = nullptr;
    m_deckC = nullptr;
    m_deckD = nullptr;

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
    refreshMidiDevices(true);
}

bool MidiControllerManager::refreshMidiDevices(bool notifyMappingList)
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return false;

    const bool devicesChanged = refreshMidiDeviceCache();

    if (m_shutdownComplete.load(std::memory_order_acquire))
        return false;

    if (devicesChanged) {
        emit midiDevicesUpdated();
        emit controllerListUpdated();
    }
    if (notifyMappingList)
        emit mappingListUpdated();
    return devicesChanged;
}

void MidiControllerManager::resetHighResolutionControlState()
{
    m_14BitFallbackTimer.stop();
    m_pending14BitMsbFallbacks.clear();
    m_14BitAccumulators.clear();
    m_channelFaderMsbGates.clear();
}

void MidiControllerManager::setNativeFlx10ScratchEnabled(bool enabled) noexcept
{
    // Close the producer gate while replacing/resetting a mapping so an event
    // cannot land between the two deck resets and bind a half-old session.
    m_nativeFlx10ScratchEnabled.store(false, std::memory_order_release);
    const double now = engine::scratch::RealtimeScratchInput::clockSeconds();
    for (std::size_t index = 0; index < m_realtimeScratchIngress.size(); ++index) {
        m_beatJumpModifierHeld[index].store(false, std::memory_order_release);
        m_realtimeScratchIngress[index].reset(now);
    }
    m_nativeFlx10ScratchEnabled.store(enabled, std::memory_order_release);
}

flx10::RealtimeIngressResult MidiControllerManager::ingestNativeFlx10ScratchMessage(
    std::uint8_t status,
    std::uint8_t data1,
    std::uint8_t data2,
    double timestampSeconds,
    std::uint64_t sourceId) noexcept
{
    if (!m_nativeFlx10ScratchEnabled.load(std::memory_order_acquire))
        return flx10::RealtimeIngressResult::Ignored;

    const int messageType = status & 0xF0;
    const int channel = status & 0x0F;
    if (channel < 0 || channel >= static_cast<int>(m_realtimeScratchIngress.size()))
        return flx10::RealtimeIngressResult::Ignored;

    auto& ingress = m_realtimeScratchIngress[static_cast<std::size_t>(channel)];
    const double timestamp = std::isfinite(timestampSeconds) && timestampSeconds > 0.0
        ? timestampSeconds
        : engine::scratch::RealtimeScratchInput::clockSeconds();

    // The four dedicated BEAT JUMP notes are momentary modifiers for platter
    // search as well as click actions. Publish their state immediately on the
    // producer thread so a following jog packet can never enter scratch before
    // the queued owner-thread button event has been dispatched.
    const bool beatJumpButton = messageType == 0x90 || messageType == 0x80;
    const bool beatJumpNote = data1 == 0x5e || data1 == 0x5f
        || data1 == 0x70 || data1 == 0x71;
    if (beatJumpButton && beatJumpNote) {
        const bool pressed = messageType == 0x90 && data2 != 0;
        // Only the press sets the gate here; the owner thread may keep a
        // search session (and this gate) alive past the release until the
        // platter itself is let go, so releases must not clear it early.
        if (pressed) {
            m_beatJumpModifierHeld[static_cast<std::size_t>(channel)].store(
                true, std::memory_order_release);
        }
        ingress.reset(timestamp);
        return flx10::RealtimeIngressResult::Ignored;
    }

    if (m_beatJumpModifierHeld[static_cast<std::size_t>(channel)].load(
            std::memory_order_acquire)) {
        return flx10::RealtimeIngressResult::Ignored;
    }

    if ((messageType == 0x90 || messageType == 0x80) && data1 == 0x36) {
        const bool pressed = messageType == 0x90 && data2 != 0;
        return pressed ? ingress.touchDown(timestamp, sourceId)
                       : ingress.touchUp(timestamp, sourceId);
    }

    if (messageType != 0xB0)
        return flx10::RealtimeIngressResult::Ignored;

    const double ticks = static_cast<double>(flx10::relativeTicksFromRaw(data2));
    if (data1 == 0x22)
        return ingress.platter(ticks, timestamp, sourceId);
    if (data1 == 0x21)
        return ingress.rim(ticks, timestamp, sourceId);
    return flx10::RealtimeIngressResult::Ignored;
}

void MidiControllerManager::runControllerHousekeeping(double monotonicSeconds)
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    if (monotonicSeconds >= m_nextControllerConnectionCheckSeconds) {
        // JUCE normally reports hotplug immediately, but the direct ALSA path
        // can retain a locally open sequencer handle after its remote device
        // disappeared. Always compare the real port list periodically so both
        // late insertion and removal are observed without restarting BrockDJ.
        m_nextControllerConnectionCheckSeconds = monotonicSeconds + 5.0;

        const bool inputOpen = hasActiveMidiInput();
        if (!inputOpen)
            cancelBeatJumpSearch();
        const bool outputOpen =
            (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
            || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
            ;

        const bool devicesChanged = refreshMidiDevices(false);
        if (devicesChanged || !inputOpen || !outputOpen) {
            if (devicesChanged || !hasActiveMidiInput())
                restoreSavedDeviceSelections();
            else if (!outputOpen)
                autoOpenFlx10MidiOutputIfNeeded();
            if (!hasActiveMidiInput())
                cancelBeatJumpSearch();
        }
    }

    if (monotonicSeconds >= m_nextControllerFeedbackResyncSeconds) {
        m_nextControllerFeedbackResyncSeconds = monotonicSeconds + 5.0;
        forceFlx10FeedbackResync();
    }
}

void MidiControllerManager::forceFlx10FeedbackResync()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;
    if (!outputOpen || !shouldUseFlx10Feedback())
        return;

    // FLX10 has no documented request for passive analog positions. Re-send
    // all software-owned LED/button state instead, while analog state remains
    // synchronized from coherent incoming 14-bit pairs in ParameterStore.
    m_lastMidiShortValues.clear();
    m_midiFeedback.refreshAll();
    refreshAllDeckLeds();
}
