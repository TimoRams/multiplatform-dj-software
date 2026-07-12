#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <QString>
#include <QDebug>
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>
#include "TrackData.h"

class WaveformAnalyzer : public juce::Thread
{
public:
    using AnalysisGeneration = std::uint64_t;

    enum class AnalysisJobState {
        Queued,
        Running,
        CancelRequested,
        Finished,
        Failed
    };

    using CompletionCallback = std::function<void(bool completed,
                                                   AnalysisGeneration generation,
                                                   const QString& filePath)>;

    WaveformAnalyzer(TrackData* trackData, juce::AudioFormatManager* formatManager, int pointsPerSecond = 600);
    ~WaveformAnalyzer();

    AnalysisGeneration startAnalysis(const QString& filePath, double seekHintSec = 0.0);
    void requestCancel() noexcept;
    void shutdownAndJoin();
    void stopAnalysis() { shutdownAndJoin(); }
    void run() override;

    // Ultra-fast full-track overview (~512 bins, no heavy filterbank). Safe to call
    // from any thread; intended for the load thread so the deck overview appears
    // before the full analysis pass starts.
    static QVector<TrackData::RgbWaveformFrame> buildInstantOverview(
        juce::AudioFormatReader* reader, int maxBins = 512);
    void setSeekHint(double positionSec);
    void setCompletionCallback(CompletionCallback callback);
    void notifyCompletion(bool completed, AnalysisGeneration generation, const QString& filePath);
    [[nodiscard]] AnalysisGeneration generation() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }
    [[nodiscard]] AnalysisJobState jobState() const noexcept {
        return m_jobState.load(std::memory_order_acquire);
    }

private:
   TrackData* m_trackData = nullptr;
   juce::AudioFormatManager* m_formatManager = nullptr;
   QString m_filePath;
   int m_pointsPerSecond;
   std::atomic<double> m_seekHintSec{0.0};
   std::atomic<AnalysisGeneration> m_generation{0};
   AnalysisGeneration m_runGeneration = 0; // written before startThread(), read only by that run
   std::atomic<AnalysisJobState> m_jobState{AnalysisJobState::Finished};
   std::mutex m_callbackMutex;
   CompletionCallback m_completionCallback;
};
