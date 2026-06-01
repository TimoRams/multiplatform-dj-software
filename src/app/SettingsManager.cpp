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
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("App/CleanShutdown", true);
    userSettings->setValue("App/LastCleanShutdown", juce::Time::getCurrentTime().toString(true, true));
    userSettings->save();
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

QString SettingsManager::getAudioMasterDeviceType() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();

    const auto value = QString::fromUtf8(userSettings->getValue("Audio/Master/DeviceType", "").toRawUTF8());
    return value.isEmpty() ? getAudioDeviceType() : value;
}

void SettingsManager::setAudioMasterDeviceType(const QString& deviceType)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Master/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioMasterOutputDevice() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();

    const auto value = QString::fromUtf8(userSettings->getValue("Audio/Master/OutputDevice", "").toRawUTF8());
    return value.isEmpty() ? getAudioOutputDevice() : value;
}

void SettingsManager::setAudioMasterOutputDevice(const QString& deviceName)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Master/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

int SettingsManager::getAudioMasterFirstChannel() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return 1;

    const int value = userSettings->getValue("Audio/Master/FirstChannel", "1").getIntValue();
    return value == -1 ? -1 : std::clamp(value, 1, 127);
}

void SettingsManager::setAudioMasterFirstChannel(int firstChannel)
{
    const int clamped = firstChannel == -1 ? -1 : std::clamp(firstChannel, 1, 127);
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Master/FirstChannel", clamped);
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioHeadphonesDeviceType() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Audio/Headphones/DeviceType", "").toRawUTF8());
}

void SettingsManager::setAudioHeadphonesDeviceType(const QString& deviceType)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Headphones/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioHeadphonesOutputDevice() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Audio/Headphones/OutputDevice", "").toRawUTF8());
}

void SettingsManager::setAudioHeadphonesOutputDevice(const QString& deviceName)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Headphones/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

int SettingsManager::getAudioHeadphonesFirstChannel() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return -1;

    const int value = userSettings->getValue("Audio/Headphones/FirstChannel", "-1").getIntValue();
    return value == -1 ? -1 : std::clamp(value, 1, 127);
}

void SettingsManager::setAudioHeadphonesFirstChannel(int firstChannel)
{
    const int clamped = firstChannel == -1 ? -1 : std::clamp(firstChannel, 1, 127);
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Headphones/FirstChannel", clamped);
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioBoothDeviceType() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Audio/Booth/DeviceType", "").toRawUTF8());
}

void SettingsManager::setAudioBoothDeviceType(const QString& deviceType)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Booth/DeviceType", juce::String::fromUTF8(deviceType.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

QString SettingsManager::getAudioBoothOutputDevice() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return QString();
    return QString::fromUtf8(userSettings->getValue("Audio/Booth/OutputDevice", "").toRawUTF8());
}

void SettingsManager::setAudioBoothOutputDevice(const QString& deviceName)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Booth/OutputDevice", juce::String::fromUTF8(deviceName.toUtf8().constData()));
    userSettings->saveIfNeeded();
    emit audioSettingsChanged();
}

int SettingsManager::getAudioBoothFirstChannel() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return -1;

    const int value = userSettings->getValue("Audio/Booth/FirstChannel", "-1").getIntValue();
    return value == -1 ? -1 : std::clamp(value, 1, 127);
}

void SettingsManager::setAudioBoothFirstChannel(int firstChannel)
{
    const int clamped = firstChannel == -1 ? -1 : std::clamp(firstChannel, 1, 127);
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->setValue("Audio/Booth/FirstChannel", clamped);
    userSettings->saveIfNeeded();
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
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return 44100;

    const int value = userSettings->getValue("Audio/SampleRate", "44100").getIntValue();
    return value > 0 ? value : 44100;
}

void SettingsManager::setAudioSampleRate(int sampleRate)
{
    sampleRate = std::clamp(sampleRate, 8000, 384000);

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
        return 512;

    const int value = userSettings->getValue("Audio/BufferSize", "512").getIntValue();
    return std::clamp(value, 64, 4096);
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

bool SettingsManager::flx10ControllerSupportEnabled() const
{
    const auto* userSettings = const_cast<SettingsManager*>(this)->getUserSettingsOrNull();
    if (userSettings == nullptr)
        return false;

    return userSettings->getBoolValue("Controllers/DDJFLX10/Enabled", false);
}

void SettingsManager::setFlx10ControllerSupportEnabled(bool enabled)
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    if (userSettings->getBoolValue("Controllers/DDJFLX10/Enabled", false) == enabled)
        return;

    userSettings->setValue("Controllers/DDJFLX10/Enabled", enabled);
    userSettings->saveIfNeeded();
    emit controllerSettingsChanged();
}

void SettingsManager::flushToDisk()
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->save();
}
