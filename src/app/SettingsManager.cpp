#include "SettingsManager.h"
#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QString>
#include <QStringList>
#include <juce_audio_devices/juce_audio_devices.h>

namespace {
juce::PropertiesFile* userSettings(SettingsManager& manager)
{
    return manager.getAppProperties().getUserSettings();
}

const juce::PropertiesFile* userSettings(const SettingsManager& manager)
{
    return const_cast<SettingsManager&>(manager).getAppProperties().getUserSettings();
}

QString readStringSetting(const SettingsManager& manager, const char* key, const char* fallback = "")
{
    const auto* settings = userSettings(manager);
    if (settings == nullptr)
        return QString();

    return QString::fromUtf8(settings->getValue(key, fallback).toRawUTF8());
}

int readIntSetting(const SettingsManager& manager, const char* key, int fallback)
{
    const auto* settings = userSettings(manager);
    if (settings == nullptr)
        return fallback;

    return settings->getValue(key, juce::String(std::to_string(fallback))).getIntValue();
}

bool readBoolSetting(const SettingsManager& manager, const char* key, bool fallback)
{
    const auto* settings = userSettings(manager);
    if (settings == nullptr)
        return fallback;

    return settings->getBoolValue(key, fallback);
}

template <typename Value>
bool writeSetting(SettingsManager& manager, const char* key, const Value& value)
{
    auto* settings = userSettings(manager);
    if (settings == nullptr)
        return false;

    settings->setValue(key, value);
    settings->saveIfNeeded();
    return true;
}
}

SettingsManager& SettingsManager::getInstance()
{
    static SettingsManager instance;
    return instance;
}

SettingsManager::SettingsManager(QObject* parent)
    : QObject(parent)
{
}

void SettingsManager::init()
{
    juce::PropertiesFile::Options options;
    options.applicationName      = "BrockDJ";
    options.filenameSuffix       = ".xml";
    options.osxLibrarySubFolder  = "Application Support";
    options.commonToAllUsers     = false;
    options.ignoreCaseOfKeyNames = true;
    options.storageFormat        = juce::PropertiesFile::storeAsXML;

    // On Linux, PropertiesFile resolves paths as ~/folderName/ (not ~/.config/folderName/).
    // To land in the XDG-compliant ~/.config directory we must prepend ".config/".
   #if JUCE_LINUX || JUCE_BSD
    options.folderName = ".config/BrockDJ";
   #else
    options.folderName = "BrockDJ";
   #endif

    appProperties.setStorageParameters(options);

    auto* userSettings = appProperties.getUserSettings();
    if (userSettings == nullptr)
    {
        qWarning() << "SettingsManager: getUserSettings() returned nullptr — cannot write settings!";
        return;
    }

    // Ensure the target directory exists before writing.
    userSettings->getFile().getParentDirectory().createDirectory();

    m_previousRunUnclean = !userSettings->getBoolValue("App/CleanShutdown", true);
    if (m_previousRunUnclean)
        qWarning() << "SettingsManager: previous run did not finish cleanly";

    userSettings->setValue("App/CleanShutdown", false);
    userSettings->setValue("App/CurrentRunStarted", juce::Time::getCurrentTime().toString(true, true));
    userSettings->setValue("ProofOfConcept_FileCreated", true);
    userSettings->setValue("LastRun", juce::Time::getCurrentTime().toString(true, true));

    // save() forces an immediate write; saveIfNeeded() skips if the file already
    // exists and is up to date, which can silently swallow the first write.
    userSettings->save();

    ensureMappingsDirectoryExists();

    qDebug() << "Settings-Datei erfolgreich erstellt/geladen unter:"
             << QString::fromUtf8(userSettings->getFile().getFullPathName().toRawUTF8());
}

void SettingsManager::shutdown()
{
    flushToDisk();
    appProperties.closeFiles();
}

QString SettingsManager::previousRunWarningMessage() const
{
    return m_previousRunUnclean
        ? QStringLiteral("BrockDJ did not shut down cleanly last time. Your library was reopened on startup; please check your recent changes if anything looks off.")
        : QString();
}

void SettingsManager::markCleanShutdown()
{
    if (auto* settings = userSettings(*this); settings != nullptr) {
        settings->setValue("App/CleanShutdown", true);
        settings->setValue("App/LastCleanShutdown", juce::Time::getCurrentTime().toString(true, true));
        settings->save();
    }
}

juce::PropertiesFile* SettingsManager::getUserSettingsOrNull()
{
    return appProperties.getUserSettings();
}

QString SettingsManager::getConfigDirectoryPath() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();

    return QString::fromUtf8(userSettings->getFile().getParentDirectory().getFullPathName().toRawUTF8());
}

QString SettingsManager::getMappingsDirectoryPath() const
{
    const QString configDir = getConfigDirectoryPath();
    if (configDir.isEmpty())
        return QString();
    return QDir(configDir).filePath("mappings");
}

void SettingsManager::ensureMappingsDirectoryExists() const
{
    const QString mappingsDir = getMappingsDirectoryPath();
    if (mappingsDir.isEmpty())
        return;

    QDir dir(mappingsDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning() << "SettingsManager: Konnte mappings-Ordner nicht erstellen:" << mappingsDir;
    }
}

QString SettingsManager::getMidiInputIdentifier() const
{
    return readStringSetting(*this, "Midi/InputIdentifier");
}

void SettingsManager::setMidiInputIdentifier(const QString& identifier)
{
    writeSetting(*this, "Midi/InputIdentifier", juce::String::fromUTF8(identifier.toUtf8().constData()));
}

QString SettingsManager::getMidiOutputIdentifier() const
{
    return readStringSetting(*this, "Midi/OutputIdentifier");
}

void SettingsManager::setMidiOutputIdentifier(const QString& identifier)
{
    writeSetting(*this, "Midi/OutputIdentifier", juce::String::fromUTF8(identifier.toUtf8().constData()));
}

QString SettingsManager::getSelectedController() const
{
    return readStringSetting(*this, "Midi/SelectedController");
}

void SettingsManager::setSelectedController(const QString& controllerName)
{
    writeSetting(*this, "Midi/SelectedController", juce::String::fromUTF8(controllerName.toUtf8().constData()));
}

QString SettingsManager::getSelectedMappingFile() const
{
    return readStringSetting(*this, "Midi/SelectedMappingFile");
}

void SettingsManager::setSelectedMappingFile(const QString& mappingFileName)
{
    writeSetting(*this, "Midi/SelectedMappingFile", juce::String::fromUTF8(mappingFileName.toUtf8().constData()));
}

QString SettingsManager::getAudioDeviceType() const
{
    return readStringSetting(*this, "Audio/DeviceType");
}

void SettingsManager::setAudioDeviceType(const QString& deviceType)
{
    writeSetting(*this, "Audio/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioOutputDevice() const
{
    return readStringSetting(*this, "Audio/OutputDevice");
}

void SettingsManager::setAudioOutputDevice(const QString& deviceName)
{
    writeSetting(*this, "Audio/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioMasterDeviceType() const
{
    const auto value = readStringSetting(*this, "Audio/Master/DeviceType");
    return value.isEmpty() ? getAudioDeviceType() : value;
}

void SettingsManager::setAudioMasterDeviceType(const QString& deviceType)
{
    writeSetting(*this, "Audio/Master/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioMasterOutputDevice() const
{
    const auto value = readStringSetting(*this, "Audio/Master/OutputDevice");
    return value.isEmpty() ? getAudioOutputDevice() : value;
}

void SettingsManager::setAudioMasterOutputDevice(const QString& deviceName)
{
    writeSetting(*this, "Audio/Master/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    emit audioSettingsChanged();
}

int SettingsManager::getAudioMasterFirstChannel() const
{
    const int value = readIntSetting(*this, "Audio/Master/FirstChannel", 1);
    return value == -1 ? -1 : std::clamp(value, 1, 127);
}

void SettingsManager::setAudioMasterFirstChannel(int firstChannel)
{
    const int clamped = firstChannel == -1 ? -1 : std::clamp(firstChannel, 1, 127);
    writeSetting(*this, "Audio/Master/FirstChannel", clamped);
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioHeadphonesDeviceType() const
{
    return readStringSetting(*this, "Audio/Headphones/DeviceType");
}

void SettingsManager::setAudioHeadphonesDeviceType(const QString& deviceType)
{
    writeSetting(*this, "Audio/Headphones/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioHeadphonesOutputDevice() const
{
    return readStringSetting(*this, "Audio/Headphones/OutputDevice");
}

void SettingsManager::setAudioHeadphonesOutputDevice(const QString& deviceName)
{
    writeSetting(*this, "Audio/Headphones/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    emit audioSettingsChanged();
}

int SettingsManager::getAudioHeadphonesFirstChannel() const
{
    const int value = readIntSetting(*this, "Audio/Headphones/FirstChannel", -1);
    return value == -1 ? -1 : std::clamp(value, 1, 127);
}

void SettingsManager::setAudioHeadphonesFirstChannel(int firstChannel)
{
    const int clamped = firstChannel == -1 ? -1 : std::clamp(firstChannel, 1, 127);
    writeSetting(*this, "Audio/Headphones/FirstChannel", clamped);
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioBoothDeviceType() const
{
    return readStringSetting(*this, "Audio/Booth/DeviceType");
}

void SettingsManager::setAudioBoothDeviceType(const QString& deviceType)
{
    writeSetting(*this, "Audio/Booth/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioBoothOutputDevice() const
{
    return readStringSetting(*this, "Audio/Booth/OutputDevice");
}

void SettingsManager::setAudioBoothOutputDevice(const QString& deviceName)
{
    writeSetting(*this, "Audio/Booth/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    emit audioSettingsChanged();
}

int SettingsManager::getAudioBoothFirstChannel() const
{
    const int value = readIntSetting(*this, "Audio/Booth/FirstChannel", -1);
    return value == -1 ? -1 : std::clamp(value, 1, 127);
}

void SettingsManager::setAudioBoothFirstChannel(int firstChannel)
{
    const int clamped = firstChannel == -1 ? -1 : std::clamp(firstChannel, 1, 127);
    writeSetting(*this, "Audio/Booth/FirstChannel", clamped);
    emit audioSettingsChanged();
}

QStringList SettingsManager::getAvailableAudioDeviceTypes() const
{
    QStringList types;
    QString jackType;
    QString pipewireType;
    QString pulseType;

    juce::AudioDeviceManager deviceManager;
    juce::String err = deviceManager.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty())
        return types;

    for (auto* type : deviceManager.getAvailableDeviceTypes()) {
        if (type == nullptr || type->getTypeName().isNotEmpty() == false)
            continue;

        const QString name = QString::fromUtf8(type->getTypeName().toRawUTF8());
        types.push_back(name);

        const QString lower = name.toLower();
        if (jackType.isEmpty() && lower == QStringLiteral("jack"))
            jackType = name;
        if (pipewireType.isEmpty() && lower.contains(QStringLiteral("pipewire")))
            pipewireType = name;
        if (pulseType.isEmpty() && (lower.contains(QStringLiteral("pulse")) || lower.contains(QStringLiteral("pulseaudio"))))
            pulseType = name;
    }

    const QString preferredType = !jackType.isEmpty() ? jackType
                                : !pipewireType.isEmpty() ? pipewireType
                                : !pulseType.isEmpty() ? pulseType
                                : QString();

    const int preferredIndex = types.indexOf(preferredType);
    if (preferredIndex > 0)
        types.move(preferredIndex, 0);

    return types;
}

int SettingsManager::getAudioSampleRate() const
{
    const int value = readIntSetting(*this, "Audio/SampleRate", 44100);
    return value > 0 ? value : 44100;
}

void SettingsManager::setAudioSampleRate(int sampleRate)
{
    sampleRate = std::clamp(sampleRate, 8000, 384000);

    writeSetting(*this, "Audio/SampleRate", sampleRate);
    emit audioSettingsChanged();
}

int SettingsManager::getAudioBufferSize() const
{
    const int value = readIntSetting(*this, "Audio/BufferSize", 512);
    return std::clamp(value, 64, 4096);
}

void SettingsManager::setAudioBufferSize(int bufferSize)
{
    bufferSize = std::clamp(bufferSize, 64, 4096);

    writeSetting(*this, "Audio/BufferSize", bufferSize);
    emit audioSettingsChanged();
}

bool SettingsManager::flx10ControllerSupportEnabled() const
{
    return readBoolSetting(*this, "Controllers/DDJFLX10/Enabled", false);
}

void SettingsManager::setFlx10ControllerSupportEnabled(bool enabled)
{
    if (readBoolSetting(*this, "Controllers/DDJFLX10/Enabled", false) == enabled)
        return;

    writeSetting(*this, "Controllers/DDJFLX10/Enabled", enabled);
    emit controllerSettingsChanged();
}

void SettingsManager::flushToDisk()
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->save();
}
