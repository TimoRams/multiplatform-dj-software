#include "AudioDeviceService.h"

#include "AudioDeviceUtils.h"

#include <QDebug>
#include <QSet>
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
#else
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
#endif
    const int requestedBufferSize = normalizeBufferSize(bufferSize);
    bufferSize = clampToStableBufferSize(requestedType, requestedBufferSize);
    if (bufferSize != requestedBufferSize) {
        setFallbackMessage(QStringLiteral(
            "%1 requested %2 samples; using %3 samples for stable audio.")
            .arg(requestedType.isEmpty() ? QStringLiteral("This backend") : requestedType)
            .arg(requestedBufferSize)
            .arg(bufferSize));
    }

    if (jackBackendRequested) {
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
        QString jackMsg;
        if (!probeJackServer(jackMsg))
            return std::unexpected(jackMsg);
#else
        return std::unexpected(QStringLiteral("JACK backend not built in this binary."));
#endif
    }

    masterFirstChannel = normalizeMasterFirstChannelForOutput(outputDevice, masterFirstChannel);
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

    if (sanitizedOutput.isEmpty()) {
        manager.closeAudioDevice();
        publishDeviceConfigurationSnapshot(0, 0);
        emit configurationChanged();
        return {};
    }

    if (!jackBackendRequested && !sanitizedOutput.isEmpty() && type != nullptr) {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        bool found = false;
        for (const auto& name : names) {
            const QString canonicalName = QString::fromUtf8(name.toRawUTF8()).trimmed();
            if (canonicalName.compare(sanitizedOutput, Qt::CaseInsensitive) == 0) {
                sanitizedOutput = canonicalName;
                found = true;
                break;
            }
        }
        if (!found) {
            manager.closeAudioDevice();
            const QString message = QStringLiteral(
                "Audio device \"%1\" is unavailable. No fallback device was selected; output is silent.")
                .arg(sanitizedOutput);
            qWarning() << "[AudioDeviceService]" << message;
            return std::unexpected(message);
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
    } else {
        setup.outputDeviceName = toJuceString(sanitizedOutput);
    }

    int maxOutputChannels = 2;  // Safe default
    const auto maxRoutedChannel = [](int firstChannel) -> int {
        return firstChannel >= 1 ? firstChannel + 1 : 0;
    };
    int maxRequestedChannel = std::max({
        maxRoutedChannel(masterFirstChannel),
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
            // No device open yet: request exactly the configured logical buses.
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
            || (currentDevice->getCurrentBufferSizeSamples() != setup.bufferSize)
            || jackBackendRequested);

    if (needsReopen)
        manager.closeAudioDevice();

    // Apply exactly the selected device and routing. Failure means silence; the
    // service never substitutes a different device or channel layout.
    juce::String error = manager.setAudioDeviceSetup(setup, true);

    auto* activeDevice = manager.getCurrentAudioDevice();
    bool deviceReady = activeDevice != nullptr && activeDevice->isOpen();

    if (error.isNotEmpty() || !deviceReady) {
        manager.closeAudioDevice();

        QString errorText = QString::fromStdString(error.toStdString());
        if (errorText.isEmpty())
            errorText = QStringLiteral("Audio device setup failed. Output is silent; no fallback was used.");
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

void AudioDeviceService::closeAudioDevice()
{
    m_manager.closeAudioDevice();
}

QStringList AudioDeviceService::availableDeviceTypes() const
{
    QStringList types;

    auto& manager = const_cast<juce::AudioDeviceManager&>(m_manager);
    const QString currentType = currentDeviceType();
    QString jackType;
    QString pipewireType;
    QString pulseType;

    for (auto* type : manager.getAvailableDeviceTypes()) {
        if (type == nullptr || type->getTypeName().isEmpty())
            continue;
        const QString name = QString::fromUtf8(type->getTypeName().toRawUTF8());
        types.push_back(name);
#if JUCE_LINUX || JUCE_BSD
        const QString lower = name.toLower();
        if (jackType.isEmpty() && lower == QStringLiteral("jack"))
            jackType = name;
        if (pipewireType.isEmpty() && lower.contains(QStringLiteral("pipewire")))
            pipewireType = name;
        if (pulseType.isEmpty() && (lower.contains(QStringLiteral("pulse"))
                                    || lower.contains(QStringLiteral("pulseaudio")))) {
            pulseType = name;
        }
#endif
    }

    const QString preferredType = !jackType.isEmpty() ? jackType
                                : !pipewireType.isEmpty() ? pipewireType
                                : !pulseType.isEmpty() ? pulseType
                                : currentType;
    const int preferredIndex = types.indexOf(preferredType);
    if (preferredIndex > 0) {
        types.move(preferredIndex, 0);
    } else {
        const int currentIndex = types.indexOf(currentType);
        if (currentIndex > 0)
            types.move(currentIndex, 0);
    }

    return types;
}

QStringList AudioDeviceService::availableOutputDevices(const QString& deviceType) const
{
    const QString cacheKey = deviceType.trimmed().toCaseFolded();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_deviceListCacheMutex);
        const auto cached = m_deviceListCache.constFind(cacheKey);
        if (cached != m_deviceListCache.cend() && cached->expiresAt > now)
            return cached->devices;
    }

    QStringList devices { QStringLiteral("None") };
    QStringList allDevices;

    auto& manager = const_cast<juce::AudioDeviceManager&>(m_manager);
    auto* type = findDeviceType(manager, deviceType);
    if (type == nullptr)
        return devices;

    type->scanForDevices();
    const QString selectedType = !deviceType.isEmpty()
        ? deviceType
        : QString::fromUtf8(type->getTypeName().toRawUTF8());
    const QString currentOutput = currentOutputDevice();
    const QString selectedTypeLower = selectedType.toLower();
    QSet<QString> seen;

    for (const auto& name : type->getDeviceNames(false)) {
        const QString outputName = QString::fromUtf8(name.toRawUTF8()).trimmed();
        if (outputName.isEmpty() || seen.contains(outputName))
            continue;

        seen.insert(outputName);
        allDevices.push_back(outputName);

        bool keep = true;
#if JUCE_LINUX || JUCE_BSD
        const QString lowerName = outputName.toLower();
        keep = !(lowerName.startsWith(QStringLiteral("hw:"))
                 || lowerName.startsWith(QStringLiteral("plughw:"))
                 || lowerName.startsWith(QStringLiteral("sysdefault:"))
                 || lowerName.startsWith(QStringLiteral("front:"))
                 || lowerName.startsWith(QStringLiteral("surround"))
                 || lowerName.startsWith(QStringLiteral("iec958:"))
                 || lowerName.startsWith(QStringLiteral("dmix:"))
                 || lowerName.startsWith(QStringLiteral("dsnoop:"))
                 || lowerName.startsWith(QStringLiteral("usbstream:"))
                 || lowerName.startsWith(QStringLiteral("jackinput"))
                 || lowerName.contains(QStringLiteral("internal"))
                 || lowerName.contains(QStringLiteral("loopback"))
                 || lowerName.startsWith(QStringLiteral("lavaplayer"))
                 || lowerName.startsWith(QStringLiteral("Combined"))
                 || lowerName == QStringLiteral("null"));

        if (!keep && (lowerName == QStringLiteral("default")
                      || lowerName.contains(QStringLiteral("pipewire"))
                      || lowerName.contains(QStringLiteral("pulse")))) {
            keep = true;
        }

        if (keep && (selectedTypeLower.contains(QStringLiteral("alsa"))
                     || selectedTypeLower.contains(QStringLiteral("pipewire"))
                     || selectedTypeLower.contains(QStringLiteral("pulse")))) {
            keep = !lowerName.contains(QStringLiteral("@"))
                && !lowerName.startsWith(QStringLiteral("builtin_"))
                && !lowerName.contains(QStringLiteral(":CARD="))
                && !lowerName.contains(QStringLiteral(":DEV="));
        }
#endif

        if (keep)
            devices.push_back(outputName);
    }

    if (devices.size() == 1)
        devices.append(allDevices);
    if (!currentOutput.isEmpty() && !devices.contains(currentOutput)
        && allDevices.contains(currentOutput)) {
        devices.push_front(currentOutput);
    }

    const int currentOutputIndex = devices.indexOf(currentOutput);
    if (currentOutputIndex > 1)
        devices.move(currentOutputIndex, 1);

    // Settings opens with an immediate scan followed by one short retry. Cache
    // only successful scans so the retry can still discover a backend that was
    // not ready during application startup.
    if (devices.size() > 1) {
        std::lock_guard<std::mutex> lock(m_deviceListCacheMutex);
        m_deviceListCache.insert(cacheKey,
                                 DeviceListCacheEntry{devices, now + std::chrono::milliseconds(1500)});
    }
    return devices;
}

QStringList AudioDeviceService::availableOutputChannelPairs(const QString& deviceType,
                                                             const QString& outputDevice) const
{
    QString selectedType = deviceType;
    QString selectedOutput = outputDevice;
    if (selectedType.isEmpty())
        selectedType = currentDeviceType();
    if (selectedOutput.isEmpty())
        selectedOutput = currentOutputDevice();
    if (selectedOutput.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
        return { QStringLiteral("None") };

    const QString loweredType = selectedType.trimmed().toLower();
    if (loweredType == QStringLiteral("jack") || loweredType.contains(QStringLiteral("jack")))
        return buildChannelPairList(kMaxSupportedOutputChannel);

    int channelCount = readCurrentDeviceOutputChannelCount(m_manager, selectedType, selectedOutput);
    if (channelCount < 2)
        channelCount = readDeviceOutputChannelCount(selectedType, selectedOutput);
    return buildChannelPairList(channelCount);
}

QString AudioDeviceService::currentDeviceType() const
{
    return QString::fromUtf8(m_manager.getCurrentAudioDeviceType().toRawUTF8());
}

QString AudioDeviceService::currentOutputDevice() const
{
    if (auto* device = m_manager.getCurrentAudioDevice())
        return QString::fromUtf8(device->getName().toRawUTF8());
    return {};
}

int AudioDeviceService::currentSampleRate() const
{
    if (auto* device = m_manager.getCurrentAudioDevice())
        return static_cast<int>(std::lround(device->getCurrentSampleRate()));
    return 0;
}

int AudioDeviceService::currentBufferSize() const
{
    if (auto* device = m_manager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();
    return 0;
}

std::uint64_t AudioDeviceService::hardwareXRunCount() const noexcept
{
    if (auto* device = m_manager.getCurrentAudioDevice())
        return static_cast<std::uint64_t>(std::max(0, device->getXRunCount()));
    return 0;
}

bool AudioDeviceService::isJackServerRunning() const
{
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
    QString message;
    return probeJackServer(message);
#else
    return false;
#endif
}

QString AudioDeviceService::jackServerStatus() const
{
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
    QString message;
    probeJackServer(message);
    return message;
#else
    return QStringLiteral("JACK backend not built in this binary.");
#endif
}
