#include "AudioDeviceUtils.h"

#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <mutex>

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

juce::String toJuceString(const QString& text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

int choosePreferredBufferSize(juce::AudioIODevice* device, int requestedSize)
{
    if (device == nullptr)
        return requestedSize;

    const auto availableSizes = device->getAvailableBufferSizes();
    if (availableSizes.isEmpty())
        return requestedSize;

    if (availableSizes.contains(requestedSize))
        return requestedSize;

    int smallestAtLeastRequested = 0;
    int largestAvailable = availableSizes[0];
    for (const int candidate : availableSizes) {
        largestAvailable = std::max(largestAvailable, candidate);
        if (candidate >= requestedSize
            && (smallestAtLeastRequested == 0 || candidate < smallestAtLeastRequested))
            smallestAtLeastRequested = candidate;
    }

    if (smallestAtLeastRequested > 0)
        return smallestAtLeastRequested;

    return largestAvailable;
}

int minimumStableBufferSizeForBackend(const QString& deviceType)
{
#if JUCE_LINUX || JUCE_BSD
    const QString lower = deviceType.toLower();
    if (lower.contains(QStringLiteral("jack")))
        return 64;
    if (!lower.contains(QStringLiteral("jack")))
        return 512;
#endif
    return 128;
}

int clampToStableBufferSize(const QString& deviceType, int requestedSize)
{
    return std::clamp(requestedSize, minimumStableBufferSizeForBackend(deviceType), 4096);
}

juce::AudioIODeviceType* findDeviceType(juce::AudioDeviceManager& deviceManager, const QString& typeName)
{
    if (typeName.isEmpty())
        return deviceManager.getCurrentDeviceTypeObject();

    for (auto* type : deviceManager.getAvailableDeviceTypes()) {
        if (type != nullptr && QString::fromUtf8(type->getTypeName().toRawUTF8()) == typeName)
            return type;
    }

    return nullptr;
}

int clampFirstChannelForPack(int firstChannel)
{
    if (firstChannel < 1)
        return -1;
    return std::clamp(firstChannel, 1, kMaxSupportedOutputChannel);
}

uint64_t packRouting(const OutputRoutingConfig& cfg)
{
    const auto encode = [](int firstChannel) -> uint64_t {
        const int clamped = clampFirstChannelForPack(firstChannel);
        return static_cast<uint64_t>(clamped < 1 ? 0 : (clamped + 1));
    };

    uint64_t packed = 0;
    packed |= encode(cfg.masterFirstChannel);
    packed |= encode(cfg.headphonesFirstChannel) << 5;
    packed |= encode(cfg.boothFirstChannel) << 10;
    return packed;
}

OutputRoutingConfig unpackRouting(uint64_t packed)
{
    const auto decode = [](uint64_t value) -> int {
        const int decoded = static_cast<int>(value & kRoutingFieldMask) - 1;
        return decoded < 1 ? -1 : decoded;
    };

    return {
        .masterFirstChannel = decode(packed),
        .headphonesFirstChannel = decode(packed >> 5),
        .boothFirstChannel = decode(packed >> 10)
    };
}

namespace {

std::mutex s_outputChannelCountCacheMutex;
QHash<QString, int> s_outputChannelCountCache;

} // namespace

void clearOutputChannelCountCache()
{
    std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
    s_outputChannelCountCache.clear();
}

int readDeviceOutputChannelCount(const QString& deviceType, const QString& outputDevice)
{
    const QString key = deviceType.trimmed() + QStringLiteral("\n") + outputDevice.trimmed();
    {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        const auto it = s_outputChannelCountCache.constFind(key);
        if (it != s_outputChannelCountCache.cend())
            return it.value();
    }

#if JUCE_LINUX || JUCE_BSD
    const QString loweredType = deviceType.trimmed().toLower();
    if (loweredType == QStringLiteral("jack") || loweredType.contains(QStringLiteral("jack"))) {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        s_outputChannelCountCache.insert(key, 2);
        return 2;
    }
#endif

    int channelCount = 2;

    juce::AudioDeviceManager probe;
    const juce::String initErr = probe.initialiseWithDefaultDevices(0, 2);
    if (initErr.isNotEmpty() || probe.getCurrentAudioDevice() == nullptr) {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        s_outputChannelCountCache.insert(key, channelCount);
        return channelCount;
    }

    juce::AudioIODeviceType* type = nullptr;
    if (!deviceType.isEmpty()) {
        type = findDeviceType(probe, deviceType);
        if (type != nullptr)
            probe.setCurrentAudioDeviceType(type->getTypeName(), true);
    } else {
        type = probe.getCurrentDeviceTypeObject();
    }

    if (type == nullptr)
        type = probe.getCurrentDeviceTypeObject();

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    probe.getAudioDeviceSetup(setup);
    setup.useDefaultInputChannels = true;
    setup.inputDeviceName.clear();
    setup.useDefaultOutputChannels = true;
    QString sanitizedOutput = outputDevice.trimmed();
    if (sanitizedOutput.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
        sanitizedOutput.clear();
    if (!sanitizedOutput.isEmpty() && type != nullptr) {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        bool found = false;
        for (const auto& name : names) {
            if (QString::fromUtf8(name.toRawUTF8()).trimmed() == sanitizedOutput) {
                found = true;
                break;
            }
        }
        if (!found)
            sanitizedOutput.clear();
    }

    if (!sanitizedOutput.isEmpty())
        setup.outputDeviceName = toJuceString(sanitizedOutput);

    juce::String error = probe.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty() && setup.outputDeviceName.isNotEmpty()) {
        setup.outputDeviceName.clear();
        error = probe.setAudioDeviceSetup(setup, true);
    }

    if (auto* device = probe.getCurrentAudioDevice()) {
        const int namesCount = device->getOutputChannelNames().size();
        const auto activeChannels = device->getActiveOutputChannels();
        const int activeSetBits = activeChannels.countNumberOfSetBits();

        if (namesCount > 0 && activeSetBits > 0)
            channelCount = std::max(namesCount, activeSetBits);
        else if (namesCount > 0)
            channelCount = namesCount;
        else if (activeSetBits > 0)
            channelCount = activeSetBits;
    }

    channelCount = std::clamp(channelCount, 2, kMaxSupportedOutputChannel);
    {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        s_outputChannelCountCache.insert(key, channelCount);
    }
    return channelCount;
}

int readCurrentDeviceOutputChannelCount(const juce::AudioDeviceManager& manager,
                                        const QString& deviceType,
                                        const QString& outputDevice)
{
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return -1;

    const QString currentType = QString::fromUtf8(manager.getCurrentAudioDeviceType().toRawUTF8());
    if (!deviceType.isEmpty() && currentType != deviceType)
        return -1;

    const QString currentOutput = QString::fromUtf8(device->getName().toRawUTF8());
    if (!outputDevice.isEmpty() && currentOutput != outputDevice)
        return -1;

    const int namesCount = device->getOutputChannelNames().size();
    const int activeSetBits = device->getActiveOutputChannels().countNumberOfSetBits();
    int channelCount = 2;
    if (namesCount > 0 && activeSetBits > 0)
        channelCount = std::max(namesCount, activeSetBits);
    else if (namesCount > 0)
        channelCount = namesCount;
    else if (activeSetBits > 0)
        channelCount = activeSetBits;

    return std::clamp(channelCount, 2, kMaxSupportedOutputChannel);
}

QStringList buildChannelPairList(int channelCount)
{
    QStringList pairs;
    pairs.push_back(QStringLiteral("None"));

    channelCount = std::clamp(channelCount, 2, kMaxSupportedOutputChannel);
    for (int first = 1; first + 1 <= channelCount; first += 2)
        pairs.push_back(QStringLiteral("%1-%2").arg(first).arg(first + 1));

    return pairs;
}

OutputLatencySnapshot readOutputLatencySnapshot(juce::AudioIODevice* device)
{
    if (!device)
        return {};

    const int outputRawSamples = std::max(0, device->getOutputLatencyInSamples());
    const int callbackBufferSamples = std::max(0, device->getCurrentBufferSizeSamples());
    int backendSamples;
    if (outputRawSamples <= 0) {
        backendSamples = 0;
    } else if (callbackBufferSamples > 0
               && outputRawSamples % callbackBufferSamples == 0
               && outputRawSamples >= 2 * callbackBufferSamples) {
        backendSamples = outputRawSamples - callbackBufferSamples;
    } else {
        backendSamples = outputRawSamples;
    }
    return {
        .outputRawSamples = outputRawSamples,
        .callbackBufferSamples = callbackBufferSamples,
        .backendOutputSamples = backendSamples,
        .sampleRate = device->getCurrentSampleRate()
    };
}

QString describeDeviceState(juce::AudioDeviceManager& manager)
{
    auto* device = manager.getCurrentAudioDevice();
    const QString typeName = QString::fromUtf8(manager.getCurrentAudioDeviceType().toRawUTF8());
    if (device == nullptr)
        return QStringLiteral("type=%1, device=<none>").arg(typeName);

    const QString deviceName = QString::fromUtf8(device->getName().toRawUTF8());
    const int outLatency = std::max(0, device->getOutputLatencyInSamples());
    const int buffer = std::max(0, device->getCurrentBufferSizeSamples());
    const double sampleRate = device->getCurrentSampleRate();
    return QStringLiteral("type=%1, device=%2, sr=%3, buf=%4, outLat=%5")
        .arg(typeName,
             deviceName,
             QString::number(sampleRate, 'f', 1),
             QString::number(buffer),
             QString::number(outLatency));
}

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)

bool probeJackServer(QString& message)
{
    jack_status_t status = JackFailure;
    jack_client_t* client = jack_client_open("BrockDJProbe", JackNoStartServer, &status);
    if (client == nullptr) {
        if (status & JackVersionError)
            message = QStringLiteral("JACK protocol version mismatch.");
        else if (status & JackServerError)
            message = QStringLiteral("JACK server error. Is PipeWire-JACK running?");
        else if (status & JackServerFailed)
            message = QStringLiteral("JACK server not running. Start PipeWire or jackd.");
        else
            message = QStringLiteral("JACK server not available. Start PipeWire-JACK.");
        return false;
    }

    jack_client_close(client);
    message = QStringLiteral("JACK server running.");
    return true;
}

int readJackBufferSize(jack_client_t* client)
{
    return client != nullptr ? static_cast<int>(jack_get_buffer_size(client)) : 0;
}

bool waitForJackBufferSize(jack_client_t* client, int requestedFrames, int& effectiveFrames)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        effectiveFrames = readJackBufferSize(client);
        if (effectiveFrames == requestedFrames)
            return true;
        QThread::msleep(25);
    }

    effectiveFrames = readJackBufferSize(client);
    return effectiveFrames == requestedFrames;
}

bool forcePipeWireQuantum(int requestedFrames, QString& message)
{
    if (requestedFrames <= 0)
        return false;

    if (QStandardPaths::findExecutable(QStringLiteral("pw-metadata")).isEmpty()) {
        message = QStringLiteral("PipeWire metadata tool not found; cannot force JACK quantum.");
        return false;
    }

    QProcess process;
    process.start(QStringLiteral("pw-metadata"),
                  {QStringLiteral("-n"),
                   QStringLiteral("settings"),
                   QStringLiteral("0"),
                   QStringLiteral("clock.force-quantum"),
                   QString::number(requestedFrames)});
    if (!process.waitForFinished(700)) {
        process.kill();
        process.waitForFinished(100);
        message = QStringLiteral("Timed out while asking PipeWire for %1 frames/period.")
            .arg(requestedFrames);
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        message = stderrText.isEmpty()
            ? QStringLiteral("PipeWire rejected %1 frames/period.").arg(requestedFrames)
            : stderrText;
        return false;
    }

    message = QStringLiteral("PipeWire quantum forced to %1 frames/period.").arg(requestedFrames);
    return true;
}

bool requestJackBufferSize(int requestedFrames, int& effectiveFrames, int& effectiveSampleRate, QString& message)
{
    effectiveFrames = requestedFrames;
    effectiveSampleRate = 0;

    if (requestedFrames <= 0) {
        message.clear();
        return true;
    }

    jack_status_t status = JackFailure;
    jack_client_t* client = jack_client_open("BrockDJBufferSetup", JackNoStartServer, &status);
    if (client == nullptr) {
        if (status & JackVersionError)
            message = QStringLiteral("JACK protocol version mismatch.");
        else if (status & JackServerError)
            message = QStringLiteral("JACK server error. Is PipeWire-JACK running?");
        else if (status & JackServerFailed)
            message = QStringLiteral("JACK server not running. Start PipeWire or jackd.");
        else
            message = QStringLiteral("JACK server not available. Start PipeWire-JACK.");
        return false;
    }

    effectiveSampleRate = static_cast<int>(jack_get_sample_rate(client));
    const jack_nframes_t current = jack_get_buffer_size(client);
    const int currentFrames = static_cast<int>(current);

    const int requestedForServer = requestedFrames;
    const jack_nframes_t requested = static_cast<jack_nframes_t>(requestedForServer);
    if (current == requested) {
        jack_client_close(client);
        message = QStringLiteral("JACK already uses %1 frames/period.").arg(effectiveFrames);
        return true;
    }

    const int result = jack_set_buffer_size(client, requested);

    if (result != 0) {
        jack_client_close(client);
        effectiveFrames = currentFrames > 0 ? currentFrames : requestedFrames;
        message = QStringLiteral("JACK rejected %1 frames/period. Change it in your JACK or PipeWire settings.")
            .arg(requestedForServer);
        return false;
    }

    waitForJackBufferSize(client, requestedForServer, effectiveFrames);
    effectiveSampleRate = static_cast<int>(jack_get_sample_rate(client));

    if (effectiveFrames != requestedForServer) {
        QString pipeWireMsg;
        if (forcePipeWireQuantum(requestedForServer, pipeWireMsg)) {
            waitForJackBufferSize(client, requestedForServer, effectiveFrames);
            if (effectiveFrames == requestedForServer) {
                effectiveSampleRate = static_cast<int>(jack_get_sample_rate(client));
                jack_client_close(client);
                message = pipeWireMsg;
                return true;
            }
        }
    }

    jack_client_close(client);

    if (effectiveFrames != requestedForServer) {
        message = QStringLiteral("Requested %1 JACK frames/period, but JACK reports %2.")
            .arg(requestedForServer)
            .arg(effectiveFrames);
    } else {
        message = QStringLiteral("JACK uses %1 frames/period.").arg(effectiveFrames);
    }
    return true;
}

#endif
