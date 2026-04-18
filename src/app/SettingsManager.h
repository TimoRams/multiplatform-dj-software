#pragma once

#include <QObject>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <QString>
#include <QStringList>

class SettingsManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString audioDeviceType READ getAudioDeviceType WRITE setAudioDeviceType NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString audioOutputDevice READ getAudioOutputDevice WRITE setAudioOutputDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioSampleRate READ getAudioSampleRate WRITE setAudioSampleRate NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioBufferSize READ getAudioBufferSize WRITE setAudioBufferSize NOTIFY audioSettingsChanged)

public:
    static SettingsManager& getInstance();

    explicit SettingsManager(QObject* parent = nullptr);

    void init();

    juce::ApplicationProperties& getAppProperties() { return appProperties; }

    QString getConfigDirectoryPath() const;
    QString getMappingsDirectoryPath() const;

    QString getMidiInputIdentifier() const;
    void setMidiInputIdentifier(const QString& identifier);

    QString getMidiOutputIdentifier() const;
    void setMidiOutputIdentifier(const QString& identifier);

    QString getSelectedController() const;
    void setSelectedController(const QString& controllerName);

    QString getSelectedMappingFile() const;
    void setSelectedMappingFile(const QString& mappingFileName);

    QString getAudioDeviceType() const;
    void setAudioDeviceType(const QString& deviceType);

    QString getAudioOutputDevice() const;
    void setAudioOutputDevice(const QString& deviceName);

    Q_INVOKABLE QStringList getAvailableAudioDeviceTypes() const;

    int getAudioSampleRate() const;
    void setAudioSampleRate(int sampleRate);

    int getAudioBufferSize() const;
    void setAudioBufferSize(int bufferSize);

signals:
    void audioSettingsChanged();

private:
    ~SettingsManager() = default;
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    juce::ApplicationProperties appProperties;

    juce::PropertiesFile* getUserSettingsOrNull();
    void ensureMappingsDirectoryExists() const;
};
