#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <mutex>

juce::String toJuceString(const QString& text);
QStringList mergePreferredOutputDevices(QStringList discoveredDevices,
                                        const QStringList& preferredDevices);
int normalizeMasterFirstChannelForOutput(const QString& outputDevice, int firstChannel);
int choosePreferredBufferSize(juce::AudioIODevice* device, int requestedSize);
int minimumStableBufferSizeForBackend(const QString& deviceType);
int clampToStableBufferSize(const QString& deviceType, int requestedSize);
juce::AudioIODeviceType* findDeviceType(juce::AudioDeviceManager& deviceManager, const QString& typeName);

struct OutputLatencySnapshot {
    int outputRawSamples = 0;
    int callbackBufferSamples = 0;
    int backendOutputSamples = 0;
    double sampleRate = 0.0;

    [[nodiscard]] int roundedSampleRate() const noexcept
    {
        return sampleRate > 0.0 ? static_cast<int>(std::lround(sampleRate)) : 0;
    }
};

struct OutputRoutingConfig {
    int masterFirstChannel = 1;
    int headphonesFirstChannel = -1;
    int boothFirstChannel = -1;
};

constexpr uint64_t kRoutingFieldMask = 0x1fu;
constexpr int kMaxSupportedOutputChannel = 30;

int clampFirstChannelForPack(int firstChannel);
uint64_t packRouting(const OutputRoutingConfig& cfg);
OutputRoutingConfig unpackRouting(uint64_t packed);

void clearOutputChannelCountCache();
int readDeviceOutputChannelCount(const QString& deviceType, const QString& outputDevice);
int readCurrentDeviceOutputChannelCount(const juce::AudioDeviceManager& manager,
                                        const QString& deviceType,
                                        const QString& outputDevice);
QStringList buildChannelPairList(int channelCount);
OutputLatencySnapshot readOutputLatencySnapshot(juce::AudioIODevice* device);
QString describeDeviceState(juce::AudioDeviceManager& manager);

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>

bool probeJackServer(QString& message);
int readJackBufferSize(jack_client_t* client);
bool waitForJackBufferSize(jack_client_t* client, int requestedFrames, int& effectiveFrames);
bool forcePipeWireQuantum(int requestedFrames, QString& message);
bool requestJackBufferSize(int requestedFrames, int& effectiveFrames, int& effectiveSampleRate, QString& message);
#endif
