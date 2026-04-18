#include "SettingsManager.h"
#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QString>
#include <QStringList>
#include <juce_audio_devices/juce_audio_devices.h>

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
    options.applicationName      = "RamsbrockDJ";
    options.filenameSuffix       = ".xml";
    options.osxLibrarySubFolder  = "Application Support";
    options.commonToAllUsers     = false;
    options.ignoreCaseOfKeyNames = true;
    options.storageFormat        = juce::PropertiesFile::storeAsXML;

    // On Linux, PropertiesFile resolves paths as ~/folderName/ (not ~/.config/folderName/).
    // To land in the XDG-compliant ~/.config directory we must prepend ".config/".
   #if JUCE_LINUX || JUCE_BSD
    options.folderName = ".config/RamsbrockDJ";
   #else
    options.folderName = "RamsbrockDJ";
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

    userSettings->setValue("ProofOfConcept_FileCreated", true);
    userSettings->setValue("LastRun", juce::Time::getCurrentTime().toString(true, true));

    // save() forces an immediate write; saveIfNeeded() skips if the file already
    // exists and is up to date, which can silently swallow the first write.
    userSettings->save();

    ensureMappingsDirectoryExists();

    qDebug() << "Settings-Datei erfolgreich erstellt/geladen unter:"
             << QString::fromUtf8(userSettings->getFile().getFullPathName().toRawUTF8());
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
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Midi/InputIdentifier", "").toRawUTF8());
}

void SettingsManager::setMidiInputIdentifier(const QString& identifier)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;
    userSettings->setValue("Midi/InputIdentifier", juce::String::fromUTF8(identifier.toUtf8().constData()));
    userSettings->saveIfNeeded();
}

QString SettingsManager::getMidiOutputIdentifier() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Midi/OutputIdentifier", "").toRawUTF8());
}

void SettingsManager::setMidiOutputIdentifier(const QString& identifier)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;
    userSettings->setValue("Midi/OutputIdentifier", juce::String::fromUTF8(identifier.toUtf8().constData()));
    userSettings->saveIfNeeded();
}

QString SettingsManager::getSelectedController() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Midi/SelectedController", "").toRawUTF8());
}

void SettingsManager::setSelectedController(const QString& controllerName)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;
    userSettings->setValue("Midi/SelectedController", juce::String::fromUTF8(controllerName.toUtf8().constData()));
    userSettings->saveIfNeeded();
}

QString SettingsManager::getSelectedMappingFile() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Midi/SelectedMappingFile", "").toRawUTF8());
}

void SettingsManager::setSelectedMappingFile(const QString& mappingFileName)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;
    userSettings->setValue("Midi/SelectedMappingFile", juce::String::fromUTF8(mappingFileName.toUtf8().constData()));
    userSettings->saveIfNeeded();
}

QString SettingsManager::getAudioDeviceType() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Audio/DeviceType", "").toRawUTF8());
}

void SettingsManager::setAudioDeviceType(const QString& deviceType)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioOutputDevice() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Audio/OutputDevice", "").toRawUTF8());
}

void SettingsManager::setAudioOutputDevice(const QString& deviceName)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QStringList SettingsManager::getAvailableAudioDeviceTypes() const
{
    QStringList types;

    juce::AudioDeviceManager deviceManager;
    juce::String err = deviceManager.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty())
        return types;

    for (auto* type : deviceManager.getAvailableDeviceTypes()) {
        if (type != nullptr && type->getTypeName().isNotEmpty())
            types.push_back(QString::fromUtf8(type->getTypeName().toRawUTF8()));
    }

    return types;
}

int SettingsManager::getAudioSampleRate() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return 44100;

    const int value = userSettings->getValue("Audio/SampleRate", "44100").getIntValue();
    return value > 0 ? value : 44100;
}

void SettingsManager::setAudioSampleRate(int sampleRate)
{
    sampleRate = std::clamp(sampleRate, 44100, 96000);

    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/SampleRate", sampleRate);
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

int SettingsManager::getAudioBufferSize() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return 128;

    const int value = userSettings->getValue("Audio/BufferSize", "128").getIntValue();
    return value > 0 ? value : 128;
}

void SettingsManager::setAudioBufferSize(int bufferSize)
{
    bufferSize = std::clamp(bufferSize, 64, 4096);

    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/BufferSize", bufferSize);
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}
