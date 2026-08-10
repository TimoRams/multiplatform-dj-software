#pragma once

#include <QObject>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <QString>
#include <QStringList>
#include <QPointer>

class AudioDeviceService;

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
    Q_PROPERTY(bool flx10ControllerSupportEnabled READ flx10ControllerSupportEnabled WRITE setFlx10ControllerSupportEnabled NOTIFY controllerSettingsChanged)
    Q_PROPERTY(bool tightDoubleSync READ tightDoubleSync WRITE setTightDoubleSync NOTIFY tightDoubleSyncChanged)
    Q_PROPERTY(QString timeStretchBackend READ timeStretchBackend WRITE setTimeStretchBackend NOTIFY timeStretchBackendChanged)
    Q_PROPERTY(bool previousRunUnclean READ previousRunUnclean CONSTANT)
    Q_PROPERTY(QString previousRunWarningMessage READ previousRunWarningMessage CONSTANT)

    Q_PROPERTY(double crossfaderPosition READ getCrossfaderPosition WRITE setCrossfaderPosition NOTIFY crossfaderSettingsChanged)
    Q_PROPERTY(double crossfaderSharpness READ getCrossfaderSharpness WRITE setCrossfaderSharpness NOTIFY crossfaderSettingsChanged)
    Q_PROPERTY(QString crossfaderCurveMode READ getCrossfaderCurveMode WRITE setCrossfaderCurveMode NOTIFY crossfaderSettingsChanged)
    Q_PROPERTY(QString crossfaderAssignA READ getCrossfaderAssignA WRITE setCrossfaderAssignA NOTIFY crossfaderSettingsChanged)
    Q_PROPERTY(QString crossfaderAssignB READ getCrossfaderAssignB WRITE setCrossfaderAssignB NOTIFY crossfaderSettingsChanged)
    Q_PROPERTY(QString crossfaderAssignC READ getCrossfaderAssignC WRITE setCrossfaderAssignC NOTIFY crossfaderSettingsChanged)
    Q_PROPERTY(QString crossfaderAssignD READ getCrossfaderAssignD WRITE setCrossfaderAssignD NOTIFY crossfaderSettingsChanged)

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
    Q_INVOKABLE QStringList getAvailableAudioOutputDevices(const QString& deviceType) const;
    Q_INVOKABLE void setAudioConfiguration(const QString& deviceType,
                                           const QString& masterOutputDevice,
                                           int masterFirstChannel,
                                           const QString& headphonesOutputDevice,
                                           int headphonesFirstChannel,
                                           const QString& boothOutputDevice,
                                           int boothFirstChannel,
                                           int sampleRate,
                                           int bufferSize);
    void persistActiveAudioConfiguration(const QString& deviceType,
                                         const QString& outputDevice,
                                         int sampleRate,
                                         int bufferSize);
    void setAudioDeviceService(AudioDeviceService* service);

    int getAudioSampleRate() const;
    void setAudioSampleRate(int sampleRate);

    int getAudioBufferSize() const;
    void setAudioBufferSize(int bufferSize);

    bool flx10ControllerSupportEnabled() const;
    void setFlx10ControllerSupportEnabled(bool enabled);

    bool tightDoubleSync() const;
    void setTightDoubleSync(bool enabled);
    QString timeStretchBackend() const;
    void setTimeStretchBackend(const QString& backend);

    // Generic persisted UI/layout state (mode, deck count, panel visibility, ...).
    // Stored under a "UI/" key prefix in the user properties file.
    Q_INVOKABLE QString getUiState(const QString& key, const QString& fallback = QString()) const;
    Q_INVOKABLE void setUiState(const QString& key, const QString& value);

    bool previousRunUnclean() const { return m_previousRunUnclean; }
    QString previousRunWarningMessage() const;
    void markCleanShutdown();

    Q_INVOKABLE void flushToDisk();

    Q_INVOKABLE void setRequestManualBackupOnExit(bool requested) { m_requestManualBackupOnExit = requested; }
    bool requestManualBackupOnExit() const { return m_requestManualBackupOnExit; }

    double getCrossfaderPosition() const;
    void setCrossfaderPosition(double position);

    double getCrossfaderSharpness() const;
    void setCrossfaderSharpness(double sharpness);

    QString getCrossfaderCurveMode() const;
    void setCrossfaderCurveMode(const QString& mode);

    QString getCrossfaderAssignA() const;
    void setCrossfaderAssignA(const QString& assign);
    QString getCrossfaderAssignB() const;
    void setCrossfaderAssignB(const QString& assign);
    QString getCrossfaderAssignC() const;
    void setCrossfaderAssignC(const QString& assign);
    QString getCrossfaderAssignD() const;
    void setCrossfaderAssignD(const QString& assign);

signals:
    void audioSettingsChanged();
    void controllerSettingsChanged();
    void tightDoubleSyncChanged();
    void timeStretchBackendChanged();
    void crossfaderSettingsChanged();

private:
    ~SettingsManager() = default;
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    juce::ApplicationProperties appProperties;
    bool m_previousRunUnclean = false;
    bool m_requestManualBackupOnExit = false;
    QPointer<AudioDeviceService> m_audioDeviceService;

    juce::PropertiesFile* getUserSettingsOrNull();
    void ensureMappingsDirectoryExists() const;
};
