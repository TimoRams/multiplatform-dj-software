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
// Read the server-owned timing without changing the JACK/PipeWire graph.
bool readJackServerConfiguration(int& bufferFrames, int& sampleRate, QString& message);
#endif

#if JUCE_LINUX || JUCE_BSD
#include <QSet>
#include <optional>

// JUCE lists every JACK client with input ports as an output device, including
// MIDI-only clients. Returns the clients that own audio playback ports, or
// nullopt when the JACK server cannot be queried.
std::optional<QSet<QString>> readJackAudioPlaybackClients();
// JUCE deliberately lists ALSA hardware PCMs even when another sound server
// (PipeWire/PulseAudio) holds them exclusively. Returns the JUCE output names
// whose hardware PCM is currently busy.
QSet<QString> readBusyAlsaOutputNames();
#endif
