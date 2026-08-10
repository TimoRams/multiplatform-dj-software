#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <QString>
#include <QDebug>
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>
#include <deque>
#include <optional>
#include "TrackData.h"
#include "analysis/AnalysisResult.h"

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

    using ResultPtr = std::shared_ptr<const analysis::AnalysisResult>;
    using CompletionCallback = std::function<void(bool completed,
                                                   AnalysisGeneration generation,
                                                   const QString& filePath,
                                                   ResultPtr result)>;
    using ProgressCallback = std::function<void(double progress, bool active,
                                                 AnalysisGeneration generation)>;
    using ChunkCallback = std::function<void(AnalysisGeneration generation, int firstBin,
                                             int totalBins,
                                             QVector<TrackData::WaveformBin> waveform,
                                             QVector<TrackData::RgbWaveformFrame> rgb)>;

    WaveformAnalyzer(juce::AudioFormatManager* formatManager, int pointsPerSecond = 600);
    ~WaveformAnalyzer();

    AnalysisGeneration startAnalysis(const QString& filePath, double seekHintSec = 0.0,
                                       std::uint64_t trackGeneration = 0,
                                       analysis::AnalysisResult seed = {});
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
    void setRealtimeInteractionActive(bool active) noexcept {
        m_realtimeInteractionActive.store(active, std::memory_order_release);
    }
    void setCompletionCallback(CompletionCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setChunkCallback(ChunkCallback callback);
    void notifyCompletion(bool completed, AnalysisGeneration generation, const QString& filePath,
                          ResultPtr result = {});
    [[nodiscard]] AnalysisGeneration generation() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }
    [[nodiscard]] AnalysisJobState jobState() const noexcept {
        return m_jobState.load(std::memory_order_acquire);
    }

private:
   juce::AudioFormatManager* m_formatManager = nullptr;
   QString m_filePath;
   int m_pointsPerSecond;
   std::atomic<double> m_seekHintSec{0.0};
   std::atomic<bool> m_realtimeInteractionActive{false};
   std::atomic<AnalysisGeneration> m_generation{0};
   AnalysisGeneration m_runGeneration = 0; // written before startThread(), read only by that run
   std::atomic<AnalysisJobState> m_jobState{AnalysisJobState::Finished};
   std::mutex m_callbackMutex;
   CompletionCallback m_completionCallback;
    ProgressCallback m_progressCallback;
    ChunkCallback m_chunkCallback;
   analysis::AnalysisResult m_seed;
   analysis::AnalysisIdentity m_identity;
};

// Bounded latest-only worker/control handoff. Publishing never calls QObject;
// the control clock or an owner-thread timer drains it.
class AnalyzerResultMailbox final
{
public:
    struct Chunk {
        WaveformAnalyzer::AnalysisGeneration generation = 0;
        int firstBin = 0;
        int totalBins = 0;
        std::shared_ptr<const QVector<TrackData::WaveformBin>> waveform;
        std::shared_ptr<const QVector<TrackData::RgbWaveformFrame>> rgb;
    };
    struct Completion {
        bool completed = false;
        WaveformAnalyzer::AnalysisGeneration generation = 0;
        QString filePath;
        WaveformAnalyzer::ResultPtr result;
    };
    void publish(Completion value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completion) ++m_replaced;
        // The validated final result contains the complete waveform. Pending
        // progressive chunks are then obsolete and must not delay or overwrite
        // that immutable result on later control ticks.
        if (value.completed && value.result) {
            m_replaced += m_chunks.size();
            m_chunks.clear();
        }
        m_completion = std::move(value);
    }
    std::optional<Completion> take()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto value = std::move(m_completion);
        m_completion.reset();
        return value;
    }
    void publishProgress(double value, bool active,
                         WaveformAnalyzer::AnalysisGeneration generation)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = value; m_active = active; m_progressGeneration = generation; m_progressDirty = true;
    }
    bool takeProgress(double& value, bool& active,
                      WaveformAnalyzer::AnalysisGeneration& generation)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_progressDirty) return false;
        value = m_progress; active = m_active; generation = m_progressGeneration;
        m_progressDirty = false;
        return true;
    }
    void publishChunk(Chunk value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A cursor-priority pass publishes its window before the sequential
        // pass. A tiny FIFO could drop that cursor window again before the next
        // 60 Hz control tick. 256 small chunks cover a large worker burst while
        // keeping the mailbox bounded to only a few MiB.
        constexpr size_t kMaxChunks = 256;
        // A new track generation supersedes every queued progressive update.
        // Keeping old chunks would delay the newly loaded track for seconds
        // now that draining is intentionally bounded per control tick.
        if (!m_chunks.empty() && m_chunks.back().generation != value.generation) {
            m_replaced += m_chunks.size();
            m_chunks.clear();
        }
        if (m_chunks.size() == kMaxChunks) {
            m_chunks.pop_front();
            ++m_replaced;
        }
        m_chunks.push_back(std::move(value));
    }
    std::vector<Chunk> takeChunks()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Bound owner-thread work. Six 128-bin chunks are enough to keep the
        // progressive display moving rapidly without monopolising a 60 Hz UI
        // tick when a fast decoder publishes a large burst.
        constexpr size_t kMaxChunksPerDrain = 6;
        const size_t count = std::min(kMaxChunksPerDrain, m_chunks.size());
        std::vector<Chunk> value;
        value.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            value.push_back(std::move(m_chunks.front()));
            m_chunks.pop_front();
        }
        return value;
    }
    [[nodiscard]] std::uint64_t replaced() const
    { std::lock_guard<std::mutex> lock(m_mutex); return m_replaced; }
private:
    mutable std::mutex m_mutex;
    std::optional<Completion> m_completion;
    std::deque<Chunk> m_chunks;
    double m_progress = 0.0;
    bool m_active = false;
    bool m_progressDirty = false;
    WaveformAnalyzer::AnalysisGeneration m_progressGeneration = 0;
    std::uint64_t m_replaced = 0;
};
