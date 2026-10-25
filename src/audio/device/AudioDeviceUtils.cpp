#include "AudioDeviceUtils.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

juce::String toJuceString(const QString& text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

QStringList mergePreferredOutputDevices(QStringList discoveredDevices,
                                        const QStringList& preferredDevices)
{
    if (discoveredDevices.isEmpty())
        discoveredDevices.push_back(QStringLiteral("None"));

    int insertionIndex = 0;
    if (discoveredDevices.front().trimmed().compare(QStringLiteral("None"),
                                                     Qt::CaseInsensitive) == 0) {
        insertionIndex = 1;
    }

    for (const QString& preferredValue : preferredDevices) {
        const QString preferred = preferredValue.trimmed();
        if (preferred.isEmpty()
            || preferred.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0) {
            continue;
        }

        const auto existing = std::find_if(discoveredDevices.cbegin(), discoveredDevices.cend(),
                                           [&preferred](const QString& candidate) {
                                               return candidate.trimmed().compare(
                                                          preferred, Qt::CaseInsensitive) == 0;
                                           });
        if (existing == discoveredDevices.cend()) {
            discoveredDevices.insert(insertionIndex, preferred);
            ++insertionIndex;
        }
    }

    return discoveredDevices;
}

int normalizeMasterFirstChannelForOutput(const QString& outputDevice, int firstChannel)
{
    const QString output = outputDevice.trimmed();
    if (output.isEmpty() || output.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
        return -1;
    return firstChannel < 1 ? 1 : std::clamp(firstChannel, 1, 127);
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

    const QString selectedOutput = outputDevice.trimmed();
    if (selectedOutput.isEmpty()
        || selectedOutput.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0) {
        return 0;
    }

    int channelCount = 0;
    juce::AudioDeviceManager probe;
    if (auto* type = findDeviceType(probe, deviceType)) {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        const auto selectedName = toJuceString(selectedOutput);
        if (names.contains(selectedName)) {
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(selectedName, {}));
            if (device)
                channelCount = device->getOutputChannelNames().size();
        }
    }

    channelCount = std::clamp(channelCount, 0, kMaxSupportedOutputChannel);
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

    if (channelCount < 2)
        return pairs;
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

bool readJackServerConfiguration(int& bufferFrames, int& sampleRate, QString& message)
{
    bufferFrames = 0;
    sampleRate = 0;

    jack_status_t status = JackFailure;
    jack_client_t* client = jack_client_open("BrockDJTimingProbe", JackNoStartServer, &status);
    if (client == nullptr) {
        QString probeMessage;
        probeJackServer(probeMessage);
        message = probeMessage;
        return false;
    }

    bufferFrames = static_cast<int>(jack_get_buffer_size(client));
    sampleRate = static_cast<int>(jack_get_sample_rate(client));
    jack_client_close(client);
    message = QStringLiteral("JACK server: %1 Hz, %2 frames/period.")
        .arg(sampleRate).arg(bufferFrames);
    return bufferFrames > 0 && sampleRate > 0;
}

#endif
