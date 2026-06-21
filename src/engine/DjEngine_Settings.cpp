#include "DjEngineCommonIncludes.h"


bool DjEngine::applyAudioDeviceSettings(int sampleRate, int bufferSize)
{
    const auto routing = unpackRouting(s_outputRoutingPacked.load(std::memory_order_relaxed));
    setLastAudioDeviceError(QString());
    setAudioDeviceFallbackMessage(QString());
    const auto result = applyAudioDeviceSettingsExpected(getCurrentAudioDeviceType(),
                                                         getCurrentAudioOutputDevice(),
                                                         sampleRate,
                                                         bufferSize,
                                                         routing.masterFirstChannel,
                                                         routing.headphonesFirstChannel,
                                                         routing.boothFirstChannel);
    if (!result) {
        setLastAudioDeviceError(result.error());
        return false;
    }
    return true;
}


bool DjEngine::applyAudioDeviceSettings(const QString& deviceType,
                                        const QString& outputDevice,
                                        int sampleRate,
                                        int bufferSize,
                                        int masterFirstChannel,
                                        int headphonesFirstChannel,
                                        int boothFirstChannel)
{
    setLastAudioDeviceError(QString());
    setAudioDeviceFallbackMessage(QString());
    const auto result = applyAudioDeviceSettingsExpected(deviceType,
                                                         outputDevice,
                                                         sampleRate,
                                                         bufferSize,
                                                         masterFirstChannel,
                                                         headphonesFirstChannel,
                                                         boothFirstChannel);
    if (!result) {
        setLastAudioDeviceError(result.error());
        return false;
    }
    return true;
}


std::expected<void, QString> DjEngine::applyAudioDeviceSettingsExpected(const QString& deviceType,
                                                                          const QString& outputDevice,
                                                                          int sampleRate,
                                                                          int bufferSize,
                                                                          int masterFirstChannel,
                                                                          int headphonesFirstChannel,
                                                                          int boothFirstChannel)
{
    sampleRate = std::clamp(sampleRate, 44100, 96000);

    const auto previousRouting = unpackRouting(s_outputRoutingPacked.load(std::memory_order_relaxed));

    const QString requestedType = !deviceType.isEmpty() ? deviceType : getCurrentAudioDeviceType();
#if JUCE_LINUX || JUCE_BSD
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    bufferSize = std::clamp(bufferSize, 64, 4096);
#else
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    bufferSize = std::clamp(bufferSize, 64, 4096);
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
    const int previousMasterFirstChannel = m_masterFirstChannelAtomic.load(std::memory_order_relaxed);
    m_masterFirstChannelAtomic.store(masterFirstChannel, std::memory_order_relaxed);
    s_outputRoutingPacked.store(packRouting({
        .masterFirstChannel = masterFirstChannel,
        .headphonesFirstChannel = headphonesFirstChannel,
        .boothFirstChannel = boothFirstChannel
    }), std::memory_order_relaxed);
    DjMasterBus::setOutputRouting(masterFirstChannel, boothFirstChannel, headphonesFirstChannel);

    auto& manager = deviceManager;
    const juce::String previousType = manager.getCurrentAudioDeviceType();
    juce::AudioDeviceManager::AudioDeviceSetup previousSetup;
    manager.getAudioDeviceSetup(previousSetup);
    if (manager.getCurrentAudioDevice() == nullptr) {
        qWarning() << "[DjEngine] No current audio device before setup; trying default initialisation";
        const juce::String initErr = manager.initialiseWithDefaultDevices(0, 2);
        if (initErr.isNotEmpty())
            qWarning() << "[DjEngine] initialiseWithDefaultDevices failed:" << QString::fromStdString(initErr.toStdString());
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
        qWarning() << "[DjEngine]" << msg;
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
            qWarning() << "[DjEngine] Requested output device not found:" << sanitizedOutput
                       << "- falling back to system default";
            setAudioDeviceFallbackMessage(
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
            qWarning() << "[DjEngine]" << jackBufferMsg;
            setAudioDeviceFallbackMessage(jackBufferMsg);
        } else if (effectiveJackBuffer != bufferSize) {
            qWarning() << "[DjEngine]" << jackBufferMsg;
            setAudioDeviceFallbackMessage(jackBufferMsg);
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
                qWarning() << "[DjEngine]" << msg;
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
            qDebug() << "[DjEngine] No current device; using first available:"
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
        qWarning() << "[DjEngine] Initial device setup failed:" << QString::fromStdString(error.toStdString());
        
        // Fallback 1: Try without custom channel routing
        if (setup.useDefaultOutputChannels == false) {
            qWarning() << "[DjEngine] Retrying without custom channel routing";
            setup.useDefaultOutputChannels = true;
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        // Fallback 2: Try with default output device
        if (error.isNotEmpty() && setup.outputDeviceName.isNotEmpty()) {
            qWarning() << "[DjEngine] Retrying with default output device";
            setup.outputDeviceName.clear();
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        // Fallback 3: Try minimum viable setup
        if (error.isNotEmpty()) {
            qWarning() << "[DjEngine] Attempting minimum viable setup";
            manager.getAudioDeviceSetup(setup);
            setup.sampleRate = static_cast<double>(sampleRate);
            setup.bufferSize = clampToStableBufferSize(requestedType, bufferSize);
            setup.useDefaultOutputChannels = true;
            setup.outputDeviceName.clear();
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        if (error.isNotEmpty()) {
            qWarning() << "[DjEngine] Failed to apply audio device settings after all fallbacks:" << QString::fromStdString(error.toStdString());
        }
    }

    auto* activeDevice = manager.getCurrentAudioDevice();
    const bool deviceReady = activeDevice != nullptr && activeDevice->isOpen();
    if (error.isNotEmpty() || !deviceReady) {
        if (!deviceReady)
            qWarning() << "[DjEngine] Audio device not available after apply; restoring previous device";

        if (!previousType.isEmpty() && previousType != manager.getCurrentAudioDeviceType())
            manager.setCurrentAudioDeviceType(previousType, true);

        const juce::String restoreErr = manager.setAudioDeviceSetup(previousSetup, true);
        if (restoreErr.isNotEmpty()) {
            qWarning() << "[DjEngine] Failed to restore previous audio device:" << QString::fromStdString(restoreErr.toStdString());
        }

        s_outputRoutingPacked.store(packRouting(previousRouting), std::memory_order_relaxed);
        m_masterFirstChannelAtomic.store(previousMasterFirstChannel, std::memory_order_relaxed);

        QString errorText = QString::fromStdString(error.toStdString());
        if (errorText.isEmpty())
            errorText = QStringLiteral("Audio device setup failed.");
        if (jackBackendRequested && !errorText.contains(QStringLiteral("JACK"), Qt::CaseInsensitive))
            errorText = QStringLiteral("JACK device failed to open. Is the JACK server running?");
        return std::unexpected(errorText);
    }

    refreshHardwareLatency();

    if (jackBackendRequested) {
        if (auto* activeJackDevice = manager.getCurrentAudioDevice()) {
            const int actualJackBuffer = activeJackDevice->getCurrentBufferSizeSamples();
            if (actualJackBuffer > 0 && actualJackBuffer != bufferSize) {
                setAudioDeviceFallbackMessage(QStringLiteral(
                    "Requested %1 JACK frames/period, but the active JACK device opened at %2. "
                    "Check PipeWire/JACK server quantum settings.")
                    .arg(bufferSize)
                    .arg(actualJackBuffer));
                bufferSize = actualJackBuffer;
            }
        }
    }

    // AudioDeviceManager is shared across all deck instances. Reconfiguring the
    // device can cause internal sources to re-prepare and some playback ratios
    // to fall back to defaults. Reapply tempo/keylock state for every deck so
    // no deck is left at stale speed after another deck applies settings.
    std::vector<DjEngine*> decks;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        decks = s_syncDecks;
    }

    for (auto* deck : decks) {
        if (!deck)
            continue;

        deck->refreshHardwareLatency();

        // During active scratch/release, onTimer() owns the scratch routing.
        if (deck->m_scratch.scrubbing() || deck->m_scratch.releaseGlide())
            continue;

        deck->updateSpeedAndPitch();
    }

    return {};
}


void DjEngine::setLastAudioDeviceError(const QString& error)
{
    if (m_lastAudioDeviceError == error)
        return;
    m_lastAudioDeviceError = error;
    emit audioDeviceErrorChanged();
}


void DjEngine::setAudioDeviceFallbackMessage(const QString& message)
{
    if (m_audioDeviceFallbackMessage == message)
        return;
    m_audioDeviceFallbackMessage = message;
    emit audioDeviceFallbackChanged();
}


void DjEngine::setOutputFirstChannel(int firstChannel)
{
    const int clamped = std::max(1, firstChannel);
    m_masterFirstChannelAtomic.store(clamped, std::memory_order_relaxed);
}


void DjEngine::setMasterVolume(float v) {
    DjMasterBus::setMasterVolume(v);
}


void DjEngine::setAntiClip(bool enabled) {
    DjMasterBus::setAntiClipEnabled(enabled);
}

