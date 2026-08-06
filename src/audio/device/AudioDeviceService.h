#pragma once

#include "AudioDeviceUtils.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <algorithm>
#include <expected>

#include <juce_audio_devices/juce_audio_devices.h>

class AudioDeviceService final : public QObject
{
    Q_OBJECT

public:
    struct ConfigurationSnapshot {
        int sampleRate = 44100;
        int bufferSize = 512;
        friend bool operator==(const ConfigurationSnapshot&, const ConfigurationSnapshot&) = default;
    };
    static constexpr int normalizeSampleRate(int sampleRate) noexcept
    {
        return std::clamp(sampleRate, 44100, 96000);
    }
    static constexpr int normalizeBufferSize(int bufferSize) noexcept
    {
        return std::clamp(bufferSize, 64, 4096);
    }
    explicit AudioDeviceService(QObject* parent = nullptr);
    ~AudioDeviceService() override;

    AudioDeviceService(const AudioDeviceService&) = delete;
    AudioDeviceService& operator=(const AudioDeviceService&) = delete;

    [[nodiscard]] juce::AudioDeviceManager& manager() noexcept { return m_manager; }
    [[nodiscard]] const juce::AudioDeviceManager& manager() const noexcept { return m_manager; }

    bool applySettings(int sampleRate, int bufferSize);
    bool applySettings(const QString& deviceType,
                       const QString& outputDevice,
                       int sampleRate,
                       int bufferSize,
                       int masterFirstChannel = 1,
                       int headphonesFirstChannel = -1,
                       int boothFirstChannel = -1);

    [[nodiscard]] QStringList availableDeviceTypes() const;
    [[nodiscard]] QStringList availableOutputDevices(const QString& deviceType = {}) const;
    [[nodiscard]] QStringList availableOutputChannelPairs(const QString& deviceType = {},
                                                          const QString& outputDevice = {}) const;
    [[nodiscard]] QString currentDeviceType() const;
    [[nodiscard]] QString currentOutputDevice() const;
    [[nodiscard]] int currentSampleRate() const;
    [[nodiscard]] int currentBufferSize() const;
    [[nodiscard]] bool isJackServerRunning() const;
    [[nodiscard]] QString jackServerStatus() const;
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] QString fallbackMessage() const { return m_fallbackMessage; }
    [[nodiscard]] OutputRoutingConfig outputRouting() const noexcept;
    [[nodiscard]] ConfigurationSnapshot configurationSnapshot() const noexcept { return m_configuration; }

    // Control/owner-thread handoff for an observed device configuration. This
    // never opens/closes hardware and is also the hardware-independent policy seam.
    void publishDeviceConfigurationSnapshot(int sampleRate, int bufferSize);

    void setOutputFirstChannel(int firstChannel);
    void closeAudioDevice();

signals:
    void configurationChanged();
    void errorChanged();
    void fallbackChanged();
    void routingChanged(int masterFirstChannel,
                        int boothFirstChannel,
                        int headphonesFirstChannel);

private:
    std::expected<void, QString> applySettingsExpected(const QString& deviceType,
                                                       const QString& outputDevice,
                                                       int sampleRate,
                                                       int bufferSize,
                                                       int masterFirstChannel,
                                                       int headphonesFirstChannel,
                                                       int boothFirstChannel);
    void setLastError(const QString& error);
    void setFallbackMessage(const QString& message);

    juce::AudioDeviceManager m_manager;
    std::atomic<uint64_t> m_outputRoutingPacked{packRouting(OutputRoutingConfig{})};
    QString m_lastError;
    QString m_fallbackMessage;
    ConfigurationSnapshot m_configuration;
};
