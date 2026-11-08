#include "app/MidiClockController.h"

#include "app/SettingsManager.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <cmath>
#include <chrono>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace brockdj::midiclock;

namespace {

double nowSeconds() noexcept
{
    return juce::Time::getMillisecondCounterHiRes() * 0.001;
}

QString qString(const juce::String& value)
{
    return QString::fromUtf8(value.toRawUTF8());
}

void listDevices(const juce::Array<juce::MidiDeviceInfo>& devices, QStringList& names, QStringList& ids)
{
    names.clear();
    ids.clear();
    for (const auto& device : devices) {
        names.append(qString(device.name));
        ids.append(qString(device.identifier));
    }
}

QString identifierFor(const QString& name, const QStringList& names, const QStringList& ids)
{
    const auto index = names.indexOf(name);
    return index >= 0 ? ids.at(index) : QString();
}

// A small real-time boost keeps pulse jitter low under GUI load. Lack of
// permission is not an error: the clock still works with normal scheduling.
void tryRaiseClockThreadPriority() noexcept
{
#if defined(__linux__)
    sched_param parameters {};
    parameters.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters);
#endif
}

} // namespace

class MidiClockController::InputCallback final : public juce::MidiInputCallback {
public:
    explicit InputCallback(MidiClockController& owner) : m_owner(owner) {}

    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        const auto* raw = message.getRawData();
        const int size = message.getRawDataSize();
        if (size <= 0)
            return;
        const auto status = static_cast<std::uint8_t>(raw[0]);
        if (status != kClock && status != kStart && status != kContinue && status != kStop
            && status != kSongPosition)
            return;
        const auto data1 = size > 1 ? static_cast<std::uint8_t>(raw[1]) : std::uint8_t {0};
        const auto data2 = size > 2 ? static_cast<std::uint8_t>(raw[2]) : std::uint8_t {0};
        const double timestamp = nowSeconds();
        std::lock_guard lock(m_owner.m_inputMutex);
        m_owner.m_follower.handle(status, timestamp, data1, data2);
    }

private:
    MidiClockController& m_owner;
};

MidiClockController::MidiClockController(SettingsManager* settings, QObject* parent)
    : QObject(parent), m_settings(settings)
{
    if (m_settings) {
        m_outputEnabled = m_settings->getUiState("midiClock/outputEnabled", "0") == "1";
        m_outputDevice = m_settings->getUiState("midiClock/output");
        m_sendTransport = m_settings->getUiState("midiClock/transport", "1") == "1";
        m_inputEnabled = m_settings->getUiState("midiClock/inputEnabled", "0") == "1";
        m_inputDevice = m_settings->getUiState("midiClock/input");
    }
    m_sendTransportFlag.store(m_sendTransport);
    m_inputPoll.setInterval(100);
    connect(&m_inputPoll, &QTimer::timeout, this, &MidiClockController::refreshInputState);
    refreshDevices();
}

MidiClockController::~MidiClockController()
{
    shutdown();
}

void MidiClockController::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_inputPoll.stop();
    stopOutputThread();
    m_output.reset();
    if (m_input)
        m_input->stop();
    m_input.reset();
    m_inputCallback.reset();
}

void MidiClockController::refreshDevices()
{
    if (m_shutdown)
        return;
    listDevices(juce::MidiOutput::getAvailableDevices(), m_outputNames, m_outputIds);
    listDevices(juce::MidiInput::getAvailableDevices(), m_inputNames, m_inputIds);
    emit devicesChanged();
    reopenOutput();
    reopenInput();
}

void MidiClockController::setOutputEnabled(bool enabled)
{
    if (enabled == m_outputEnabled)
        return;
    m_outputEnabled = enabled;
    persist();
    emit changed();
    reopenOutput();
}

void MidiClockController::setOutputDevice(const QString& name)
{
    if (name == m_outputDevice)
        return;
    m_outputDevice = name;
    persist();
    emit changed();
    reopenOutput();
}

void MidiClockController::setSendTransport(bool enabled)
{
    if (enabled == m_sendTransport)
        return;
    m_sendTransport = enabled;
    m_sendTransportFlag.store(enabled);
    persist();
    emit changed();
}

void MidiClockController::setInputEnabled(bool enabled)
{
    if (enabled == m_inputEnabled)
        return;
    m_inputEnabled = enabled;
    persist();
    emit changed();
    reopenInput();
}

void MidiClockController::setInputDevice(const QString& name)
{
    if (name == m_inputDevice)
        return;
    m_inputDevice = name;
    persist();
    emit changed();
    reopenInput();
}

void MidiClockController::persist()
{
    if (!m_settings)
        return;
    m_settings->setUiState("midiClock/outputEnabled", m_outputEnabled ? "1" : "0");
    m_settings->setUiState("midiClock/output", m_outputDevice);
    m_settings->setUiState("midiClock/transport", m_sendTransport ? "1" : "0");
    m_settings->setUiState("midiClock/inputEnabled", m_inputEnabled ? "1" : "0");
    m_settings->setUiState("midiClock/input", m_inputDevice);
}

void MidiClockController::reopenOutput()
{
    stopOutputThread();
    m_output.reset();
    if (m_shutdown || !m_outputEnabled) {
        setOutputStatus(QStringLiteral("Off"));
        return;
    }
    if (m_outputDevice.isEmpty()) {
        setOutputStatus(QStringLiteral("Select a MIDI output"));
        return;
    }
    const QString identifier = identifierFor(m_outputDevice, m_outputNames, m_outputIds);
    if (identifier.isEmpty()) {
        setOutputStatus(QStringLiteral("Device not connected: %1").arg(m_outputDevice));
        return;
    }
    m_output = juce::MidiOutput::openDevice(juce::String::fromUTF8(identifier.toUtf8().constData()));
    if (!m_output) {
        setOutputStatus(QStringLiteral("Cannot open %1").arg(m_outputDevice));
        return;
    }
    {
        std::lock_guard lock(m_outputMutex);
        m_outputStopping = false;
    }
    m_outputThread = std::thread([this] { outputLoop(); });
    setOutputStatus(QStringLiteral("Sending clock to %1").arg(m_outputDevice));
}

void MidiClockController::stopOutputThread()
{
    {
        std::lock_guard lock(m_outputMutex);
        m_outputStopping = true;
    }
    m_outputWake.notify_all();
    if (m_outputThread.joinable())
        m_outputThread.join();
}

void MidiClockController::sendStatus(std::uint8_t status)
{
    if (m_output)
        m_output->sendMessageNow(juce::MidiMessage(static_cast<int>(status)));
}

void MidiClockController::outputLoop()
{
    tryRaiseClockThreadPriority();
    MidiClockScheduler scheduler;
    std::array<std::uint8_t, 64> pending {};
    std::unique_lock lock(m_outputMutex);
    while (!m_outputStopping) {
        const ClockSnapshot snapshot = m_masterClock;
        lock.unlock();
        scheduler.setTransportEnabled(m_sendTransportFlag.load(std::memory_order_relaxed));
        std::size_t count = 0;
        const double wake = scheduler.advance(snapshot, nowSeconds(), [&](std::uint8_t status) {
            if (count < pending.size())
                pending[count++] = status;
        });
        for (std::size_t i = 0; i < count; ++i)
            sendStatus(pending[i]);
        lock.lock();
        if (m_outputStopping)
            break;
        const double delay = wake - nowSeconds();
        if (delay > 0.0)
            m_outputWake.wait_for(lock, std::chrono::duration<double>(delay));
    }
    lock.unlock();
    if (scheduler.transportRunning())
        sendStatus(kStop);
}

void MidiClockController::publishMasterClock(const ClockSnapshot& snapshot)
{
    std::lock_guard lock(m_outputMutex);
    m_masterClock = snapshot;
}

void MidiClockController::reopenInput()
{
    if (m_input)
        m_input->stop();
    m_input.reset();
    {
        std::lock_guard lock(m_inputMutex);
        m_follower.reset();
    }
    m_inputPoll.stop();
    if (m_shutdown || !m_inputEnabled) {
        setInputStatus(QStringLiteral("Off"));
        refreshInputState();
        return;
    }
    if (m_inputDevice.isEmpty()) {
        setInputStatus(QStringLiteral("Select a MIDI input"));
        refreshInputState();
        return;
    }
    const QString identifier = identifierFor(m_inputDevice, m_inputNames, m_inputIds);
    if (identifier.isEmpty()) {
        setInputStatus(QStringLiteral("Device not connected: %1").arg(m_inputDevice));
        refreshInputState();
        return;
    }
    if (!m_inputCallback)
        m_inputCallback = std::make_unique<InputCallback>(*this);
    m_input = juce::MidiInput::openDevice(juce::String::fromUTF8(identifier.toUtf8().constData()),
                                          m_inputCallback.get());
    if (!m_input) {
        setInputStatus(QStringLiteral("Cannot open %1").arg(m_inputDevice));
        refreshInputState();
        return;
    }
    m_input->start();
    m_inputPoll.start();
    setInputStatus(QStringLiteral("Waiting for clock from %1").arg(m_inputDevice));
}

ClockSnapshot MidiClockController::externalClock(double now) const
{
    if (!m_inputEnabled || !m_input)
        return {};
    std::lock_guard lock(m_inputMutex);
    return m_follower.snapshot(now);
}

void MidiClockController::refreshInputState()
{
    const auto snapshot = externalClock(nowSeconds());
    const bool active = snapshot.valid;
    const double bpm = active ? std::round(snapshot.bpm * 10.0) / 10.0 : 0.0;
    const bool changedState = active != m_inputActive || bpm != m_inputBpm;
    m_inputActive = active;
    m_inputBpm = bpm;
    if (m_input)
        setInputStatus(active ? QStringLiteral("Locked to %1 · %2 BPM%3")
                                    .arg(m_inputDevice)
                                    .arg(bpm, 0, 'f', 1)
                                    .arg(snapshot.running ? QStringLiteral(" · running") : QString())
                              : QStringLiteral("Waiting for clock from %1").arg(m_inputDevice));
    if (changedState)
        emit statusChanged();
}

void MidiClockController::setOutputStatus(const QString& status)
{
    if (status == m_outputStatus)
        return;
    m_outputStatus = status;
    emit statusChanged();
}

void MidiClockController::setInputStatus(const QString& status)
{
    if (status == m_inputStatus)
        return;
    m_inputStatus = status;
    emit statusChanged();
}
