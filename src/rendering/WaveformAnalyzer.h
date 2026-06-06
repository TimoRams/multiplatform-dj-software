#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <QString>
#include <QDebug>
#include <atomic>
#include <functional>
#include <mutex>
#include "TrackData.h"

class WaveformAnalyzer : public juce::Thread
{
public:
    WaveformAnalyzer(TrackData* trackData, juce::AudioFormatManager* formatManager, int pointsPerSecond = 600);
    ~WaveformAnalyzer();

    void startAnalysis(const QString& filePath, double seekHintSec = 0.0);
    void stopAnalysis();
    void run() override;

    // Ultra-fast full-track overview (~512 bins, no heavy filterbank). Safe to call
    // from any thread; intended for the load thread so the deck overview appears
    // before the full analysis pass starts.
    static QVector<TrackData::RgbWaveformFrame> buildInstantOverview(
        juce::AudioFormatReader* reader, int maxBins = 512);
    void setSeekHint(double positionSec);
    void setCompletionCallback(std::function<void(bool completed)> callback);
    void notifyCompletion(bool completed);

private:
   TrackData* m_trackData = nullptr;
   juce::AudioFormatManager* m_formatManager = nullptr;
   QString m_filePath;
   int m_pointsPerSecond;
   std::atomic<double> m_seekHintSec{0.0};
   std::mutex m_callbackMutex;
   std::function<void(bool completed)> m_completionCallback;
};
