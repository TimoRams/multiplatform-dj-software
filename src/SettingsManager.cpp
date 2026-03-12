#include "SettingsManager.h"
#include <QDebug>

SettingsManager& SettingsManager::getInstance()
{
    static SettingsManager instance;
    return instance;
}

void SettingsManager::init()
{
    juce::PropertiesFile::Options options;
    options.applicationName       = "RamsbrockDJ";
    options.folderName            = "RamsbrockDJ";
    options.filenameSuffix        = ".xml";
    options.osxLibrarySubFolder   = "Application Support";
    options.commonToAllUsers      = false;
    options.ignoreCaseOfKeyNames  = true;
    options.storageFormat         = juce::PropertiesFile::storeAsXML;

    appProperties.setStorageParameters(options);

    auto* userSettings = appProperties.getUserSettings();
    if (userSettings != nullptr)
    {
        userSettings->setValue("ProofOfConcept_FileCreated", true);
        userSettings->setValue("LastRun", juce::Time::getCurrentTime().toString(true, true));
        userSettings->saveIfNeeded();

        qDebug() << "Settings-Datei erfolgreich erstellt/geladen unter:"
                 << QString::fromUtf8(userSettings->getFile().getFullPathName().toRawUTF8());
    }
}
