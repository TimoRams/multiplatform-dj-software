#include "SettingsManager.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/device/AudioDeviceUtils.h"
#include <algorithm>
#include <cmath>
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

double readDoubleSetting(const SettingsManager& manager, const char* key, double fallback)
{
    const auto* settings = userSettings(manager);
    if (settings == nullptr)
        return fallback;

    return settings->getValue(key, juce::String(fallback)).getDoubleValue();
}

QString normalizeCrossfaderAssign(const QString& assign)
{
    if (assign == QLatin1String("A") || assign == QLatin1String("B") || assign == QLatin1String("T"))
        return assign;
    return QStringLiteral("A");
}

QString normalizeCrossfaderCurveMode(const QString& mode)
{
    if (mode == QLatin1String("linear"))
        return QStringLiteral("linear");
    return QStringLiteral("exponential");
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

    // Older settings could persist a real Master device together with channel
    // -1 ("None"). That opens the hardware successfully but intentionally routes
    // no Master samples to it. Migrate that contradictory state to stereo 1-2.
    const QString savedMasterOutput = QString::fromUtf8(
        userSettings->getValue("Audio/Master/OutputDevice",
                               userSettings->getValue("Audio/OutputDevice")).toRawUTF8());
    const int savedMasterFirstChannel = userSettings->getIntValue("Audio/Master/FirstChannel", 1);
    const int normalizedMasterFirstChannel = normalizeMasterFirstChannelForOutput(
        savedMasterOutput, savedMasterFirstChannel);
    if (normalizedMasterFirstChannel != savedMasterFirstChannel) {
        userSettings->setValue("Audio/Master/FirstChannel", normalizedMasterFirstChannel);
        qInfo() << "[SettingsManager] Migrated Master output routing to channel pair"
                << normalizedMasterFirstChannel << "-" << normalizedMasterFirstChannel + 1;
    }

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
    return normalizeMasterFirstChannelForOutput(getAudioMasterOutputDevice(), value);
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
    return m_audioDeviceService ? m_audioDeviceService->availableDeviceTypes() : QStringList{};
}

QStringList SettingsManager::getAvailableAudioOutputDevices(const QString& deviceType) const
{
    if (!m_audioDeviceService)
        return { QStringLiteral("None") };

    QStringList preferredDevices;
    const auto appendForMatchingType = [&preferredDevices, &deviceType](const QString& savedType,
                                                                        const QString& savedDevice) {
        if ((savedType.isEmpty()
             || savedType.trimmed().compare(deviceType.trimmed(), Qt::CaseInsensitive) == 0)
            && !savedDevice.trimmed().isEmpty()) {
            preferredDevices.push_back(savedDevice);
        }
    };
    appendForMatchingType(getAudioMasterDeviceType(), getAudioMasterOutputDevice());
    appendForMatchingType(getAudioHeadphonesDeviceType(), getAudioHeadphonesOutputDevice());
    appendForMatchingType(getAudioBoothDeviceType(), getAudioBoothOutputDevice());

    return mergePreferredOutputDevices(
        m_audioDeviceService->availableOutputDevices(deviceType), preferredDevices);
}

void SettingsManager::setAudioConfiguration(const QString& deviceType,
                                            const QString& masterOutputDevice,
                                            int masterFirstChannel,
                                            const QString& headphonesOutputDevice,
                                            int headphonesFirstChannel,
                                            const QString& boothOutputDevice,
                                            int boothFirstChannel,
                                            int sampleRate,
                                            int bufferSize)
{
    auto* settings = userSettings(*this);
    if (settings == nullptr)
        return;

    const auto asJuce = [](const QString& value) {
        return juce::String::fromUTF8(value.trimmed().toUtf8().constData());
    };
    const auto clampChannel = [](int channel) {
        return channel == -1 ? -1 : std::clamp(channel, 1, 127);
    };

    const juce::String type = asJuce(deviceType);
    settings->setValue("Audio/DeviceType", type);
    settings->setValue("Audio/OutputDevice", asJuce(masterOutputDevice));
    settings->setValue("Audio/Master/DeviceType", type);
    settings->setValue("Audio/Master/OutputDevice", asJuce(masterOutputDevice));
    settings->setValue("Audio/Master/FirstChannel",
                       normalizeMasterFirstChannelForOutput(masterOutputDevice, masterFirstChannel));
    settings->setValue("Audio/Headphones/DeviceType", type);
    settings->setValue("Audio/Headphones/OutputDevice", asJuce(headphonesOutputDevice));
    settings->setValue("Audio/Headphones/FirstChannel", clampChannel(headphonesFirstChannel));
    settings->setValue("Audio/Booth/DeviceType", type);
    settings->setValue("Audio/Booth/OutputDevice", asJuce(boothOutputDevice));
    settings->setValue("Audio/Booth/FirstChannel", clampChannel(boothFirstChannel));
    settings->setValue("Audio/SampleRate", std::clamp(sampleRate, 8000, 384000));
    settings->setValue("Audio/BufferSize", std::clamp(bufferSize, 64, 4096));
    settings->save();
    emit audioSettingsChanged();
}

void SettingsManager::persistActiveAudioConfiguration(const QString& deviceType,
                                                      const QString& outputDevice,
                                                      int sampleRate,
                                                      int bufferSize)
{
    const QString canonicalType = deviceType.trimmed();
    const QString canonicalOutput = outputDevice.trimmed();
    if (canonicalType.isEmpty() || canonicalOutput.isEmpty() || sampleRate <= 0 || bufferSize <= 0)
        return;

    auto* settings = userSettings(*this);
    if (settings == nullptr)
        return;

    const juce::String type = juce::String::fromUTF8(canonicalType.toUtf8().constData());
    const juce::String output = juce::String::fromUTF8(canonicalOutput.toUtf8().constData());
    const int normalizedRate = std::clamp(sampleRate, 8000, 384000);
    const int normalizedBuffer = std::clamp(bufferSize, 64, 4096);
    const bool unchanged = getAudioMasterDeviceType() == canonicalType
        && getAudioMasterOutputDevice() == canonicalOutput
        && getAudioSampleRate() == normalizedRate
        && getAudioBufferSize() == normalizedBuffer;
    if (unchanged)
        return;

    settings->setValue("Audio/DeviceType", type);
    settings->setValue("Audio/OutputDevice", output);
    settings->setValue("Audio/Master/DeviceType", type);
    settings->setValue("Audio/Master/OutputDevice", output);
    settings->setValue("Audio/SampleRate", normalizedRate);
    settings->setValue("Audio/BufferSize", normalizedBuffer);
    settings->save();
    emit audioSettingsChanged();
}

void SettingsManager::setAudioDeviceService(AudioDeviceService* service)
{
    m_audioDeviceService = service;
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

bool SettingsManager::tightDoubleSync() const
{
    return getUiState(QStringLiteral("tightDoubleSync"), QStringLiteral("0")) == QLatin1String("1");
}

void SettingsManager::setTightDoubleSync(bool enabled)
{
    const QString value = enabled ? QStringLiteral("1") : QStringLiteral("0");
    if (getUiState(QStringLiteral("tightDoubleSync"), QStringLiteral("0")) == value)
        return;

    setUiState(QStringLiteral("tightDoubleSync"), value);
    emit tightDoubleSyncChanged();
}

QString SettingsManager::timeStretchBackend() const
{
    const auto backend = readStringSetting(*this, "Audio/TimeStretchBackend", "signalsmith").trimmed().toLower();
    return backend == QLatin1String("rubberband") ? QStringLiteral("rubberband")
                                                   : QStringLiteral("signalsmith");
}

int SettingsManager::waveformRenderStyle() const
{
    // 0 is the compatible spectral RGB default.  Keep this UI preference out
    // of TrackData/cache identity: it changes only render-cache keys.
    return std::clamp(readIntSetting(*this, "Waveform/RenderStyle", 0), 0, 1);
}

void SettingsManager::setWaveformRenderStyle(int style)
{
    const int normalized = std::clamp(style, 0, 1);
    if (waveformRenderStyle() == normalized)
        return;
    writeSetting(*this, "Waveform/RenderStyle", normalized);
    emit waveformRenderStyleChanged();
}

void SettingsManager::setTimeStretchBackend(const QString& backend)
{
    const QString normalized = backend.trimmed().compare(QLatin1String("rubberband"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("rubberband") : QStringLiteral("signalsmith");
    if (timeStretchBackend() == normalized)
        return;
    writeSetting(*this, "Audio/TimeStretchBackend",
                 juce::String(normalized.toUtf8().constData()));
    emit timeStretchBackendChanged();
}

QString SettingsManager::getUiState(const QString& key, const QString& fallback) const
{
    const auto* settings = userSettings(*this);
    if (settings == nullptr)
        return fallback;

    const juce::String storageKey = juce::String("UI/") + juce::String(key.toUtf8().constData());
    if (!settings->containsKey(storageKey))
        return fallback;

    return QString::fromUtf8(settings->getValue(storageKey).toRawUTF8());
}

void SettingsManager::setUiState(const QString& key, const QString& value)
{
    auto* settings = userSettings(*this);
    if (settings == nullptr)
        return;

    const juce::String storageKey = juce::String("UI/") + juce::String(key.toUtf8().constData());
    if (settings->getValue(storageKey) == juce::String(value.toUtf8().constData()))
        return;

    settings->setValue(storageKey, juce::String(value.toUtf8().constData()));
    settings->saveIfNeeded();
}

void SettingsManager::flushToDisk()
{
    auto* userSettings = getUserSettingsOrNull();
    if (userSettings == nullptr)
        return;

    userSettings->save();
}

double SettingsManager::getCrossfaderPosition() const
{
    return std::clamp(readDoubleSetting(*this, "Crossfader/Position", 0.0), -1.0, 1.0);
}

void SettingsManager::setCrossfaderPosition(double position)
{
    const double clamped = std::clamp(position, -1.0, 1.0);
    if (std::abs(getCrossfaderPosition() - clamped) < 0.0001)
        return;

    writeSetting(*this, "Crossfader/Position", clamped);
    emit crossfaderSettingsChanged();
}

double SettingsManager::getCrossfaderSharpness() const
{
    return std::clamp(readDoubleSetting(*this, "Crossfader/Sharpness", 0.0), 0.0, 1.0);
}

void SettingsManager::setCrossfaderSharpness(double sharpness)
{
    const double clamped = std::clamp(sharpness, 0.0, 1.0);
    if (std::abs(getCrossfaderSharpness() - clamped) < 0.0001)
        return;

    writeSetting(*this, "Crossfader/Sharpness", clamped);
    emit crossfaderSettingsChanged();
}

QString SettingsManager::getCrossfaderCurveMode() const
{
    return normalizeCrossfaderCurveMode(readStringSetting(*this, "Crossfader/CurveMode", "exponential"));
}

void SettingsManager::setCrossfaderCurveMode(const QString& mode)
{
    const QString normalized = normalizeCrossfaderCurveMode(mode);
    if (getCrossfaderCurveMode() == normalized)
        return;

    writeSetting(*this, "Crossfader/CurveMode", normalized.toUtf8().constData());
    emit crossfaderSettingsChanged();
}

QString SettingsManager::getCrossfaderAssignA() const
{
    return normalizeCrossfaderAssign(readStringSetting(*this, "Crossfader/AssignA", "A"));
}

void SettingsManager::setCrossfaderAssignA(const QString& assign)
{
    const QString normalized = normalizeCrossfaderAssign(assign);
    if (getCrossfaderAssignA() == normalized)
        return;

    writeSetting(*this, "Crossfader/AssignA", normalized.toUtf8().constData());
    emit crossfaderSettingsChanged();
}

QString SettingsManager::getCrossfaderAssignB() const
{
    return normalizeCrossfaderAssign(readStringSetting(*this, "Crossfader/AssignB", "B"));
}

void SettingsManager::setCrossfaderAssignB(const QString& assign)
{
    const QString normalized = normalizeCrossfaderAssign(assign);
    if (getCrossfaderAssignB() == normalized)
        return;

    writeSetting(*this, "Crossfader/AssignB", normalized.toUtf8().constData());
    emit crossfaderSettingsChanged();
}

QString SettingsManager::getCrossfaderAssignC() const
{
    return normalizeCrossfaderAssign(readStringSetting(*this, "Crossfader/AssignC", "A"));
}

void SettingsManager::setCrossfaderAssignC(const QString& assign)
{
    const QString normalized = normalizeCrossfaderAssign(assign);
    if (getCrossfaderAssignC() == normalized)
        return;

    writeSetting(*this, "Crossfader/AssignC", normalized.toUtf8().constData());
    emit crossfaderSettingsChanged();
}

QString SettingsManager::getCrossfaderAssignD() const
{
    return normalizeCrossfaderAssign(readStringSetting(*this, "Crossfader/AssignD", "B"));
}

void SettingsManager::setCrossfaderAssignD(const QString& assign)
{
    const QString normalized = normalizeCrossfaderAssign(assign);
    if (getCrossfaderAssignD() == normalized)
        return;

    writeSetting(*this, "Crossfader/AssignD", normalized.toUtf8().constData());
    emit crossfaderSettingsChanged();
}
