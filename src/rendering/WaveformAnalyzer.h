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
#include <map>
#include <optional>
#include "TrackData.h"
#include "analysis/AnalysisResult.h"
#include "waveform/WaveformNormalizationState.h"
#include "waveform/WaveformDemand.h"

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
                                             QVector<TrackData::RgbWaveformFrame> rgb,
                                             WaveformNormalizationState state)>;
    using OverviewCallback = std::function<void(
        AnalysisGeneration generation, int totalBins,
        QVector<TrackData::RgbWaveformFrame> overview)>;

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
    void setWaveformDemand(const waveform::WaveformDemand& demand);
    void setRealtimeInteractionActive(bool active) noexcept {
        m_realtimeInteractionActive.store(active, std::memory_order_release);
    }
    void setCompletionCallback(CompletionCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setChunkCallback(ChunkCallback callback);
    void setOverviewCallback(OverviewCallback callback);
    void notifyCompletion(bool completed, AnalysisGeneration generation, const QString& filePath,
                          ResultPtr result = {});
    [[nodiscard]] AnalysisGeneration generation() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }
    [[nodiscard]] AnalysisJobState jobState() const noexcept {
        return m_jobState.load(std::memory_order_acquire);
    }

private:
   [[nodiscard]] waveform::WaveformDemand waveformDemandSnapshot() const;
   juce::AudioFormatManager* m_formatManager = nullptr;
   QString m_filePath;
   int m_pointsPerSecond;
   std::atomic<double> m_seekHintSec{0.0};
   std::atomic<bool> m_realtimeInteractionActive{false};
   mutable std::mutex m_demandMutex;
   waveform::WaveformDemand m_waveformDemand;
   std::atomic<AnalysisGeneration> m_generation{0};
   AnalysisGeneration m_runGeneration = 0; // written before startThread(), read only by that run
   std::atomic<AnalysisJobState> m_jobState{AnalysisJobState::Finished};
   std::mutex m_callbackMutex;
   CompletionCallback m_completionCallback;
    ProgressCallback m_progressCallback;
    ChunkCallback m_chunkCallback;
    OverviewCallback m_overviewCallback;
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
        WaveformNormalizationState normalizationState =
            WaveformNormalizationState::Preview;
        std::uint64_t publicationSequence = 0;
    };
    struct Completion {
        bool completed = false;
        WaveformAnalyzer::AnalysisGeneration generation = 0;
        QString filePath;
        WaveformAnalyzer::ResultPtr result;
    };
    struct Overview {
        WaveformAnalyzer::AnalysisGeneration generation = 0;
        int totalBins = 0;
        std::shared_ptr<const QVector<TrackData::RgbWaveformFrame>> samples;
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
            if (m_overview) {
                ++m_replaced;
                m_overview.reset();
            }
        }
        m_completion = std::move(value);
    }
    void publishOverview(Overview value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_overview)
            ++m_replaced;
        m_overview = std::move(value);
    }
    std::optional<Overview> takeOverview()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto value = std::move(m_overview);
        m_overview.reset();
        return value;
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
        constexpr size_t kMaxChunks = 256;
        if (!m_chunks.empty() && m_chunks.begin()->first.first != value.generation) {
            m_replaced += m_chunks.size();
            m_chunks.clear();
            m_firstDetailPublished = false;
            m_chunkGeneration = value.generation;
        } else if (m_chunks.empty() && m_chunkGeneration != value.generation) {
            m_firstDetailPublished = false;
            m_chunkGeneration = value.generation;
        }
        const ChunkKey key{value.generation, value.firstBin};
        if (const auto previous = m_chunks.find(key); previous != m_chunks.end()) {
            if (previous->second.normalizationState
                    == WaveformNormalizationState::Final
                && value.normalizationState
                    == WaveformNormalizationState::Preview) {
                ++m_replaced;
                return;
            }
            value.publicationSequence = previous->second.publicationSequence;
            previous->second = std::move(value);
            ++m_replaced;
            return;
        }
        if (m_chunks.size() == kMaxChunks) {
            const auto oldest = std::min_element(
                m_chunks.begin(), m_chunks.end(), [](const auto& left,
                                                      const auto& right) {
                    return left.second.publicationSequence
                        < right.second.publicationSequence;
                });
            m_chunks.erase(oldest);
            ++m_replaced;
        }
        value.publicationSequence = ++m_publicationSequence;
        m_chunks.emplace(key, std::move(value));
    }
    std::vector<Chunk> takeChunks(
        const waveform::WaveformDemand& demand = {},
        double pointsPerSecond = 1200.0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        constexpr size_t kMaxChunksPerDrain = 6;
        std::vector<std::pair<ChunkKey, waveform::WaveformPriorityScore>> order;
        order.reserve(m_chunks.size());
        for (const auto& [key, chunk] : m_chunks) {
            const double begin = pointsPerSecond > 0.0
                ? static_cast<double>(chunk.firstBin) / pointsPerSecond : 0.0;
            const int chunkCount = chunk.rgb
                ? chunk.rgb->size() : (chunk.waveform ? chunk.waveform->size() : 0);
            const double end = pointsPerSecond > 0.0
                ? static_cast<double>(chunk.firstBin + std::max(1, chunkCount))
                    / pointsPerSecond
                : begin + 1.0;
            order.emplace_back(key,
                waveform::priorityForRange(demand, begin, end));
        }
        std::stable_sort(order.begin(), order.end(),
            [this](const auto& left, const auto& right) {
                if (waveform::higherPriority(left.second, right.second))
                    return true;
                if (waveform::higherPriority(right.second, left.second))
                    return false;
                return m_chunks.at(left.first).publicationSequence
                    < m_chunks.at(right.first).publicationSequence;
            });
        size_t count = std::min(kMaxChunksPerDrain, m_chunks.size());
        if (demand.valid() && !m_firstDetailPublished) {
            const auto playhead = std::find_if(
                order.begin(), order.end(), [](const auto& candidate) {
                    return candidate.second.expansionRank == 0;
                });
            // Loading elsewhere is not detail readiness.  Hold background
            // publications behind the complete overview until the immutable
            // chunk containing the current playhead is available.
            if (playhead == order.end())
                return {};
            if (playhead != order.cbegin())
                std::rotate(order.begin(), playhead, std::next(playhead));
            count = 1;
        }
        std::vector<Chunk> value;
        value.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto found = m_chunks.find(order[index].first);
            value.push_back(std::move(found->second));
            m_chunks.erase(found);
        }
        if (!value.empty())
            m_firstDetailPublished = true;
        return value;
    }
    [[nodiscard]] std::uint64_t replaced() const
    { std::lock_guard<std::mutex> lock(m_mutex); return m_replaced; }
private:
    using ChunkKey = std::pair<WaveformAnalyzer::AnalysisGeneration, int>;
    mutable std::mutex m_mutex;
    std::optional<Completion> m_completion;
    std::optional<Overview> m_overview;
    std::map<ChunkKey, Chunk> m_chunks;
    double m_progress = 0.0;
    bool m_active = false;
    bool m_progressDirty = false;
    WaveformAnalyzer::AnalysisGeneration m_progressGeneration = 0;
    std::uint64_t m_replaced = 0;
    std::uint64_t m_publicationSequence = 0;
    WaveformAnalyzer::AnalysisGeneration m_chunkGeneration = 0;
    bool m_firstDetailPublished = false;
};
