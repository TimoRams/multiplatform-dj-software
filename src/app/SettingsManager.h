#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <QString>

class SettingsManager
{
public:
    static SettingsManager& getInstance();

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

private:
    SettingsManager() = default;
    ~SettingsManager() = default;
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    juce::ApplicationProperties appProperties;

    juce::PropertiesFile* getUserSettingsOrNull();
    void ensureMappingsDirectoryExists() const;
};
