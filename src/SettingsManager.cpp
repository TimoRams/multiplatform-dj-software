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

    qDebug() << "Settings-Datei erfolgreich erstellt/geladen unter:"
             << QString::fromUtf8(userSettings->getFile().getFullPathName().toRawUTF8());
}
