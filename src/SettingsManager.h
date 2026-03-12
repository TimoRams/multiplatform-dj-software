#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

class SettingsManager
{
public:
    static SettingsManager& getInstance();

    void init();

    juce::ApplicationProperties& getAppProperties() { return appProperties; }

private:
    SettingsManager() = default;
    ~SettingsManager() = default;
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    juce::ApplicationProperties appProperties;
};
