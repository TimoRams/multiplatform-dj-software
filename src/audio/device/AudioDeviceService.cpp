#include "AudioDeviceService.h"

#include "engine/audio/AudioDeviceUtils.h"

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <vector>

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

AudioDeviceService::AudioDeviceService(QObject* parent)
    : QObject(parent)
{
    clearOutputChannelCountCache();
}

AudioDeviceService::~AudioDeviceService()
{
    closeAudioDevice();
}


bool AudioDeviceService::applySettings(int sampleRate, int bufferSize)
{
    const auto routing = unpackRouting(m_outputRoutingPacked.load(std::memory_order_relaxed));
    setLastError(QString());
    setFallbackMessage(QString());
    const auto result = applySettingsExpected(currentDeviceType(),
                                                         currentOutputDevice(),
                                                         sampleRate,
                                                         bufferSize,
                                                         routing.masterFirstChannel,
                                                         routing.headphonesFirstChannel,
                                                         routing.boothFirstChannel);
    if (!result) {
        setLastError(result.error());
        return false;
    }
    return true;
}


bool AudioDeviceService::applySettings(const QString& deviceType,
                                        const QString& outputDevice,
                                        int sampleRate,
                                        int bufferSize,
                                        int masterFirstChannel,
                                        int headphonesFirstChannel,
                                        int boothFirstChannel)
{
    setLastError(QString());
    setFallbackMessage(QString());
    const auto result = applySettingsExpected(deviceType,
                                                         outputDevice,
                                                         sampleRate,
                                                         bufferSize,
                                                         masterFirstChannel,
                                                         headphonesFirstChannel,
                                                         boothFirstChannel);
    if (!result) {
        setLastError(result.error());
        return false;
    }
    return true;
}


std::expected<void, QString> AudioDeviceService::applySettingsExpected(const QString& deviceType,
                                                                          const QString& outputDevice,
                                                                          int sampleRate,
                                                                          int bufferSize,
                                                                          int masterFirstChannel,
                                                                          int headphonesFirstChannel,
                                                                          int boothFirstChannel)
{
    sampleRate = normalizeSampleRate(sampleRate);

    const auto previousRouting = unpackRouting(m_outputRoutingPacked.load(std::memory_order_relaxed));
    const QString previousDeviceType = currentDeviceType();
    const QString previousOutputDevice = currentOutputDevice();
    const int previousSampleRate = currentSampleRate();
    const int previousBufferSize = currentBufferSize();

    const QString requestedType = !deviceType.isEmpty() ? deviceType : currentDeviceType();
#if JUCE_LINUX || JUCE_BSD
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    bufferSize = normalizeBufferSize(bufferSize);
#else
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    bufferSize = normalizeBufferSize(bufferSize);
#endif

    if (jackBackendRequested) {
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
        QString jackMsg;
        if (!probeJackServer(jackMsg))
            return std::unexpected(jackMsg);
#else
        return std::unexpected(QStringLiteral("JACK backend not built in this binary."));
#endif
    }

    masterFirstChannel = clampFirstChannelForPack(masterFirstChannel);
    headphonesFirstChannel = clampFirstChannelForPack(headphonesFirstChannel);
    boothFirstChannel = clampFirstChannelForPack(boothFirstChannel);
    const OutputRoutingConfig requestedRouting{
        .masterFirstChannel = masterFirstChannel,
        .headphonesFirstChannel = headphonesFirstChannel,
        .boothFirstChannel = boothFirstChannel
    };
    m_outputRoutingPacked.store(packRouting(requestedRouting), std::memory_order_relaxed);
    if (packRouting(previousRouting) != packRouting(requestedRouting))
        emit routingChanged(masterFirstChannel, boothFirstChannel, headphonesFirstChannel);

    auto& manager = m_manager;
    const juce::String previousType = manager.getCurrentAudioDeviceType();
    juce::AudioDeviceManager::AudioDeviceSetup previousSetup;
    manager.getAudioDeviceSetup(previousSetup);
    if (manager.getCurrentAudioDevice() == nullptr) {
        qWarning() << "[AudioDeviceService] No current audio device before setup; trying default initialisation";
        const juce::String initErr = manager.initialiseWithDefaultDevices(0, 2);
        if (initErr.isNotEmpty())
            qWarning() << "[AudioDeviceService] initialiseWithDefaultDevices failed:" << QString::fromStdString(initErr.toStdString());
    }
    
    auto* type = findDeviceType(manager, deviceType);
    if (!deviceType.isEmpty() && type == nullptr) {
        manager.setCurrentAudioDeviceType(toJuceString(deviceType), true);
        type = findDeviceType(manager, deviceType);
    }
    if (!deviceType.isEmpty() && type == nullptr) {
        const QString msg = jackBackendRequested
            ? QStringLiteral("JACK backend not available. Start JACK or install JACK support.")
            : QStringLiteral("Audio backend not available: %1").arg(deviceType);
        qWarning() << "[AudioDeviceService]" << msg;
        return std::unexpected(msg);
    }
    if (!deviceType.isEmpty() && QString::fromUtf8(manager.getCurrentAudioDeviceType().toRawUTF8()) != deviceType) {
        manager.setCurrentAudioDeviceType(toJuceString(deviceType), true);
        type = findDeviceType(manager, deviceType);
    }

    QString sanitizedOutput = outputDevice.trimmed();
    if (sanitizedOutput.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
        sanitizedOutput.clear();

    if (!jackBackendRequested && !sanitizedOutput.isEmpty() && type != nullptr) {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        bool found = false;
        for (const auto& name : names) {
            if (QString::fromUtf8(name.toRawUTF8()).trimmed() == sanitizedOutput) {
                found = true;
                break;
            }
        }
        if (!found) {
            qWarning() << "[AudioDeviceService] Requested output device not found:" << sanitizedOutput
                       << "- falling back to system default";
            setFallbackMessage(
                QStringLiteral("Saved audio device \"%1\" is no longer available. "
                               "Falling back to system default — open Settings → Audio Setup to reconfigure.")
                .arg(sanitizedOutput));
            sanitizedOutput.clear();
        }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    manager.getAudioDeviceSetup(setup);
    if (jackBackendRequested) {
        if (auto* device = manager.getCurrentAudioDevice()) {
            const double currentJackRate = device->getCurrentSampleRate();
            if (currentJackRate > 0.0)
                sampleRate = static_cast<int>(std::lround(currentJackRate));
        }

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
        QString jackBufferMsg;
        int effectiveJackBuffer = bufferSize;
        int effectiveJackSampleRate = 0;
        if (!requestJackBufferSize(bufferSize, effectiveJackBuffer, effectiveJackSampleRate, jackBufferMsg)) {
            qWarning() << "[AudioDeviceService]" << jackBufferMsg;
            setFallbackMessage(jackBufferMsg);
        } else if (effectiveJackBuffer != bufferSize) {
            qWarning() << "[AudioDeviceService]" << jackBufferMsg;
            setFallbackMessage(jackBufferMsg);
        }
        bufferSize = effectiveJackBuffer;
        if (effectiveJackSampleRate > 0)
            sampleRate = effectiveJackSampleRate;
#endif
    }
    setup.sampleRate = static_cast<double>(sampleRate);
    setup.bufferSize = bufferSize;
    setup.useDefaultInputChannels = true;
    setup.inputDeviceName.clear();
    setup.useDefaultOutputChannels = true;
    
    if (jackBackendRequested) {
        if (sanitizedOutput.isEmpty()) {
            if (type != nullptr) {
                type->scanForDevices();
                const auto names = type->getDeviceNames(false);
                if (names.size() > 0)
                    setup.outputDeviceName = names[0];
            }

            if (setup.outputDeviceName.isEmpty()) {
                const QString msg = QStringLiteral("No JACK output ports available. Start PipeWire-JACK or jackd.");
                qWarning() << "[AudioDeviceService]" << msg;
                return std::unexpected(msg);
            }
        } else {
            setup.outputDeviceName = toJuceString(sanitizedOutput);
        }
    } else if (sanitizedOutput.isEmpty()) {
        if (auto* device = manager.getCurrentAudioDevice()) {
            setup.outputDeviceName = device->getName();
        } else if (type != nullptr) {
            // No device open yet (first startup or empty saved config).
            // Scan and pick the first available device so audio starts automatically.
            type->scanForDevices();
            const auto names = type->getDeviceNames(false);
            if (!names.isEmpty())
                setup.outputDeviceName = names[0];
            qDebug() << "[AudioDeviceService] No current device; using first available:"
                     << QString::fromUtf8(setup.outputDeviceName.toRawUTF8());
        }
        // setup.outputDeviceName may still be empty — JUCE will use its own default.
    } else {
        setup.outputDeviceName = toJuceString(sanitizedOutput);
    }

    int maxOutputChannels = 2;  // Safe default
    const auto maxRoutedChannel = [](int firstChannel) -> int {
        return firstChannel >= 1 ? firstChannel + 1 : 0;
    };
    int maxRequestedChannel = std::max({
        maxRoutedChannel(masterFirstChannel),
        maxRoutedChannel(masterFirstChannel + 2),  // second deck auto-assigned to next pair
        maxRoutedChannel(headphonesFirstChannel),
        maxRoutedChannel(boothFirstChannel),
        2
    });
    
    // JACK setup should avoid extra device probing because it can block.
    if (!jackBackendRequested) {
        if (auto* device = manager.getCurrentAudioDevice()) {
            const int namesCount = device->getOutputChannelNames().size();
            const int activeSetBits = device->getActiveOutputChannels().countNumberOfSetBits();
            int deviceChannels = 2;
            if (namesCount > 0)
                deviceChannels = namesCount;
            else if (activeSetBits > 0)
                deviceChannels = activeSetBits;
            deviceChannels = std::clamp(deviceChannels, 2, kMaxSupportedOutputChannel);

            // Use the larger of what the device currently has and what is being
            // requested.  If we only took the current active count (often 2 for a
            // default stereo open) we would wrongly reject e.g. ch 3-4 on a
            // 4-channel interface before JUCE even tries to open them.
            maxOutputChannels = std::max(maxRequestedChannel, deviceChannels);
            maxOutputChannels = std::clamp(maxOutputChannels, 2, kMaxSupportedOutputChannel);

            setup.bufferSize = choosePreferredBufferSize(device, setup.bufferSize);
        } else {
            // No device open yet — trust the requested channels; JUCE will error
            // if the hardware doesn't support them and the fallback path kicks in.
            maxOutputChannels = std::clamp(maxRequestedChannel, 2, kMaxSupportedOutputChannel);
        }
    } else {
        maxOutputChannels = std::clamp(maxRequestedChannel, 2, kMaxSupportedOutputChannel);
    }

    juce::BigInteger selectedOutputChannels;
    const auto setPairBits = [&selectedOutputChannels](int firstChannel) {
        if (firstChannel < 1)
            return;
        selectedOutputChannels.setBit(firstChannel - 1);
        selectedOutputChannels.setBit(firstChannel);
    };
    setPairBits(masterFirstChannel);
    setPairBits(masterFirstChannel + 2);  // second deck output pair
    setPairBits(headphonesFirstChannel);
    setPairBits(boothFirstChannel);
    if (jackBackendRequested) {
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        for (int ch = 0; ch < maxOutputChannels; ++ch)
            setup.outputChannels.setBit(ch);
    } else if (selectedOutputChannels.countNumberOfSetBits() > 0) {
        setup.useDefaultOutputChannels = false;
        setup.outputChannels = selectedOutputChannels;
    }

    auto* currentDevice = manager.getCurrentAudioDevice();
    const bool needsReopen = currentDevice != nullptr
        && ((std::abs(currentDevice->getCurrentSampleRate() - static_cast<double>(sampleRate)) > 0.5)
            || (currentDevice->getCurrentBufferSizeSamples() != bufferSize)
            || jackBackendRequested);

    if (needsReopen)
        manager.closeAudioDevice();

    // Try to apply the audio device setup with fallback strategy
    juce::String error = manager.setAudioDeviceSetup(setup, true);
    
    if (error.isNotEmpty()) {
        qWarning() << "[AudioDeviceService] Initial device setup failed:" << QString::fromStdString(error.toStdString());
        
        // Fallback 1: Try without custom channel routing
        if (setup.useDefaultOutputChannels == false) {
            qWarning() << "[AudioDeviceService] Retrying without custom channel routing";
            setup.useDefaultOutputChannels = true;
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        // Fallback 2: Try with default output device
        if (error.isNotEmpty() && setup.outputDeviceName.isNotEmpty()) {
            qWarning() << "[AudioDeviceService] Retrying with default output device";
            setup.outputDeviceName.clear();
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        // Fallback 3: Try minimum viable setup
        if (error.isNotEmpty()) {
            qWarning() << "[AudioDeviceService] Attempting minimum viable setup";
            manager.getAudioDeviceSetup(setup);
            setup.sampleRate = static_cast<double>(sampleRate);
            setup.bufferSize = clampToStableBufferSize(requestedType, bufferSize);
            setup.useDefaultOutputChannels = true;
            setup.outputDeviceName.clear();
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        if (error.isNotEmpty()) {
            qWarning() << "[AudioDeviceService] Failed to apply audio device settings after all fallbacks:" << QString::fromStdString(error.toStdString());
        }
    }

    auto* activeDevice = manager.getCurrentAudioDevice();
    bool deviceReady = activeDevice != nullptr && activeDevice->isOpen();

    // Last-ditch recovery: the targeted fallbacks above can return no error yet
    // still leave no open output device — e.g. when the saved device is an
    // unplugged controller (DDJ-FLX10) whose name lingers in the saved config.
    // Force JUCE's own default devices with safe stereo routing so audio keeps
    // working instead of restoring the unusable saved device (which leaves
    // everything silent and makes the whole mixer appear dead).
    if (error.isNotEmpty() || !deviceReady) {
        qWarning() << "[AudioDeviceService] No usable output after fallbacks; forcing system default device";
        const juce::String defErr = manager.initialiseWithDefaultDevices(0, 2);
        activeDevice = manager.getCurrentAudioDevice();
        deviceReady = activeDevice != nullptr && activeDevice->isOpen();
        if (deviceReady) {
            error = juce::String();
            m_outputRoutingPacked.store(packRouting(OutputRoutingConfig{}), std::memory_order_relaxed);
            emit routingChanged(1, -1, -1);
            setFallbackMessage(
                QStringLiteral("Saved audio device was unavailable. Using the system default "
                               "output — open Settings → Audio Setup to reconfigure."));
        } else if (defErr.isNotEmpty()) {
            qWarning() << "[AudioDeviceService] Default-device recovery failed:"
                       << QString::fromStdString(defErr.toStdString());
        }
    }

    if (error.isNotEmpty() || !deviceReady) {
        if (!deviceReady)
            qWarning() << "[AudioDeviceService] Audio device not available after apply; restoring previous device";

        if (!previousType.isEmpty() && previousType != manager.getCurrentAudioDeviceType())
            manager.setCurrentAudioDeviceType(previousType, true);

        const juce::String restoreErr = manager.setAudioDeviceSetup(previousSetup, true);
        if (restoreErr.isNotEmpty()) {
            qWarning() << "[AudioDeviceService] Failed to restore previous audio device:" << QString::fromStdString(restoreErr.toStdString());
        }

        m_outputRoutingPacked.store(packRouting(previousRouting), std::memory_order_relaxed);
        emit routingChanged(previousRouting.masterFirstChannel,
                            previousRouting.boothFirstChannel,
                            previousRouting.headphonesFirstChannel);

        QString errorText = QString::fromStdString(error.toStdString());
        if (errorText.isEmpty())
            errorText = QStringLiteral("Audio device setup failed.");
        if (jackBackendRequested && !errorText.contains(QStringLiteral("JACK"), Qt::CaseInsensitive))
            errorText = QStringLiteral("JACK device failed to open. Is the JACK server running?");
        return std::unexpected(errorText);
    }

    if (jackBackendRequested) {
        if (auto* activeJackDevice = manager.getCurrentAudioDevice()) {
            const int actualJackBuffer = activeJackDevice->getCurrentBufferSizeSamples();
            if (actualJackBuffer > 0 && actualJackBuffer != bufferSize) {
                setFallbackMessage(QStringLiteral(
                    "Requested %1 JACK frames/period, but the active JACK device opened at %2. "
                    "Check PipeWire/JACK server quantum settings.")
                    .arg(bufferSize)
                    .arg(actualJackBuffer));
                bufferSize = actualJackBuffer;
            }
        }
    }

    const bool deviceConfigurationChanged = previousDeviceType != currentDeviceType()
        || previousOutputDevice != currentOutputDevice()
        || previousSampleRate != currentSampleRate()
        || previousBufferSize != currentBufferSize()
        || packRouting(previousRouting) != m_outputRoutingPacked.load(std::memory_order_relaxed);
    publishDeviceConfigurationSnapshot(currentSampleRate(), currentBufferSize());
    if (deviceConfigurationChanged && previousSampleRate == currentSampleRate()
        && previousBufferSize == currentBufferSize())
        emit configurationChanged();
    return {};
}


void AudioDeviceService::setLastError(const QString& error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit errorChanged();
}


void AudioDeviceService::setFallbackMessage(const QString& message)
{
    if (m_fallbackMessage == message)
        return;
    m_fallbackMessage = message;
    emit fallbackChanged();
}


void AudioDeviceService::setOutputFirstChannel(int firstChannel)
{
    auto routing = outputRouting();
    const int clamped = std::max(1, firstChannel);
    if (routing.masterFirstChannel == clamped)
        return;
    routing.masterFirstChannel = clamped;
    m_outputRoutingPacked.store(packRouting(routing), std::memory_order_relaxed);
    emit routingChanged(routing.masterFirstChannel,
                        routing.boothFirstChannel,
                        routing.headphonesFirstChannel);
    emit configurationChanged();
}

OutputRoutingConfig AudioDeviceService::outputRouting() const noexcept
{
    return unpackRouting(m_outputRoutingPacked.load(std::memory_order_relaxed));
}

void AudioDeviceService::publishDeviceConfigurationSnapshot(int sampleRate, int bufferSize)
{
    const ConfigurationSnapshot next{
        .sampleRate = normalizeSampleRate(sampleRate),
        .bufferSize = normalizeBufferSize(bufferSize)
    };
    if (next == m_configuration)
        return;
    m_configuration = next;
    emit configurationChanged();
}

bool AudioDeviceService::ensureDeviceAvailable()
{
    if (m_manager.getCurrentAudioDevice() != nullptr)
        return true;
    const juce::String error = m_manager.initialiseWithDefaultDevices(0, 2);
    const bool available = error.isEmpty() && m_manager.getCurrentAudioDevice() != nullptr;
    if (available)
        emit configurationChanged();
    else
        setLastError(QString::fromStdString(error.toStdString()));
    return available;
}

void AudioDeviceService::closeAudioDevice()
{
    m_manager.closeAudioDevice();
}
