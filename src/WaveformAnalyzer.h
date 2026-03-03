#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <QString>
#include <QDebug>
#include "TrackData.h"

class WaveformAnalyzer : public juce::Thread
{
public:
    WaveformAnalyzer(TrackData* trackData, juce::AudioFormatManager* formatManager, int pointsPerSecond = 150);
    ~WaveformAnalyzer();

    void startAnalysis(const QString& filePath);
    void stopAnalysis();
    void run() override;

private:
   TrackData* m_trackData = nullptr;
   juce::AudioFormatManager* m_formatManager = nullptr;
   QString m_filePath;
   int m_pointsPerSecond;
};
