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
    Q_PROPERTY(QString audioMasterDeviceType READ getAudioMasterDeviceType WRITE setAudioMasterDeviceType NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString audioMasterOutputDevice READ getAudioMasterOutputDevice WRITE setAudioMasterOutputDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioMasterFirstChannel READ getAudioMasterFirstChannel WRITE setAudioMasterFirstChannel NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString audioHeadphonesDeviceType READ getAudioHeadphonesDeviceType WRITE setAudioHeadphonesDeviceType NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString audioHeadphonesOutputDevice READ getAudioHeadphonesOutputDevice WRITE setAudioHeadphonesOutputDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioHeadphonesFirstChannel READ getAudioHeadphonesFirstChannel WRITE setAudioHeadphonesFirstChannel NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString audioBoothDeviceType READ getAudioBoothDeviceType WRITE setAudioBoothDeviceType NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString audioBoothOutputDevice READ getAudioBoothOutputDevice WRITE setAudioBoothOutputDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioBoothFirstChannel READ getAudioBoothFirstChannel WRITE setAudioBoothFirstChannel NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioSampleRate READ getAudioSampleRate WRITE setAudioSampleRate NOTIFY audioSettingsChanged)
    Q_PROPERTY(int audioBufferSize READ getAudioBufferSize WRITE setAudioBufferSize NOTIFY audioSettingsChanged)

public:
    static SettingsManager& getInstance();

    explicit SettingsManager(QObject* parent = nullptr);

    void init();
    void shutdown();

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

    QString getAudioMasterDeviceType() const;
    void setAudioMasterDeviceType(const QString& deviceType);
    QString getAudioMasterOutputDevice() const;
    void setAudioMasterOutputDevice(const QString& deviceName);
    int getAudioMasterFirstChannel() const;
    void setAudioMasterFirstChannel(int firstChannel);

    QString getAudioHeadphonesDeviceType() const;
    void setAudioHeadphonesDeviceType(const QString& deviceType);
    QString getAudioHeadphonesOutputDevice() const;
    void setAudioHeadphonesOutputDevice(const QString& deviceName);
    int getAudioHeadphonesFirstChannel() const;
    void setAudioHeadphonesFirstChannel(int firstChannel);

    QString getAudioBoothDeviceType() const;
    void setAudioBoothDeviceType(const QString& deviceType);
    QString getAudioBoothOutputDevice() const;
    void setAudioBoothOutputDevice(const QString& deviceName);
    int getAudioBoothFirstChannel() const;
    void setAudioBoothFirstChannel(int firstChannel);

    Q_INVOKABLE QStringList getAvailableAudioDeviceTypes() const;

    int getAudioSampleRate() const;
    void setAudioSampleRate(int sampleRate);

    int getAudioBufferSize() const;
    void setAudioBufferSize(int bufferSize);

    Q_INVOKABLE void flushToDisk();

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
