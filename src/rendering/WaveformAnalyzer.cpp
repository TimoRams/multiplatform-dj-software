#include "WaveformAnalyzer.h"
#include "WaveformEnvelopePass.h"
#include "WaveformAnalysisOrchestrator.h"
#include "WaveformCache.h"
#include "analysis/AnalysisWorkingData.h"
#include <QFileInfo>
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

// Cap concurrent full-track analyses (BPM/key/beatgrid over the whole file are
// CPU-heavy). Derived from the host core count so it scales with hardware: a
// 4-core ARM64 board runs one at a time (leaving cores for audio + UI), while a
// many-core x86 desktop allows several. Cached on first use.
int maxConcurrentAnalyses()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 1;
    // Full-track DSP is background work. Reserve enough CPU for Vulkan/Qt,
    // audio callbacks and cache decoding even when all four decks are loaded.
    return hw <= 8 ? 1 : 2;
}

void lowerAnalysisThreadPriority()
{
#ifdef __linux__
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 12);
#endif
}

std::mutex g_analysisSlotsMutex;
std::condition_variable g_analysisSlotsCv;
int g_activeAnalyses = 0;

class AnalysisSlot
{
public:
    explicit AnalysisSlot(juce::Thread& owner)
    {
        static const int kMaxConcurrentAnalyses = maxConcurrentAnalyses();
        while (!owner.threadShouldExit()) {
            std::unique_lock<std::mutex> lock(g_analysisSlotsMutex);
            if (g_activeAnalyses < kMaxConcurrentAnalyses) {
                ++g_activeAnalyses;
                m_acquired = true;
                return;
            }

            g_analysisSlotsCv.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    ~AnalysisSlot()
    {
        if (!m_acquired)
            return;

        {
            std::lock_guard<std::mutex> lock(g_analysisSlotsMutex);
            g_activeAnalyses = std::max(0, g_activeAnalyses - 1);
        }
        g_analysisSlotsCv.notify_one();
    }

    bool acquired() const { return m_acquired; }

private:
    bool m_acquired = false;
};

class AnalysisCompletionNotifier
{
public:
    AnalysisCompletionNotifier(WaveformAnalyzer& analyzer,
                               WaveformAnalyzer::AnalysisGeneration generation,
                               QString filePath)
        : m_analyzer(analyzer), m_generation(generation), m_filePath(std::move(filePath)) {}

    ~AnalysisCompletionNotifier()
    {
        m_analyzer.notifyCompletion(m_completed, m_generation, m_filePath, std::move(m_result));
    }

    void markCompleted(WaveformAnalyzer::ResultPtr result)
    { m_result = std::move(result); m_completed = static_cast<bool>(m_result); }

private:
    WaveformAnalyzer& m_analyzer;
    WaveformAnalyzer::AnalysisGeneration m_generation;
    QString m_filePath;
    bool m_completed = false;
    WaveformAnalyzer::ResultPtr m_result;
};

} // namespace

WaveformAnalyzer::WaveformAnalyzer(juce::AudioFormatManager* formatManager, int pointsPerSecond)
    : juce::Thread("WaveformAnalyzerThread"), m_formatManager(formatManager), m_pointsPerSecond(pointsPerSecond)
{
}

WaveformAnalyzer::~WaveformAnalyzer()
{
    shutdownAndJoin();
}

WaveformAnalyzer::AnalysisGeneration WaveformAnalyzer::startAnalysis(const QString& filePath,
                                                                     double seekHintSec,
                                                                     std::uint64_t trackGeneration,
                                                                     analysis::AnalysisResult seed)
{
    shutdownAndJoin();
    const auto newGeneration = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_runGeneration = newGeneration;
    m_filePath = filePath;
    m_seed = std::move(seed);
    const QFileInfo info(filePath);
    m_identity.canonicalFilePath = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    m_identity.trackGeneration = trackGeneration;
    m_identity.fileSize = static_cast<std::uint64_t>(std::max<qint64>(0, info.size()));
    m_identity.fileModifiedMs = info.lastModified().toMSecsSinceEpoch();
    m_identity.analysisVersion = analysis::kCurrentAnalysisVersion;
    m_identity.requestGeneration = newGeneration;
    m_seekHintSec.store(seekHintSec, std::memory_order_relaxed);
    m_jobState.store(AnalysisJobState::Queued, std::memory_order_release);
    ProgressCallback progress;
    { std::lock_guard<std::mutex> lock(m_callbackMutex); progress = m_progressCallback; }
    if (progress) progress(0.0, true, newGeneration);
    startThread(juce::Thread::Priority::background);
    return newGeneration;
}

void WaveformAnalyzer::setSeekHint(double positionSec)
{
    m_seekHintSec.store(positionSec, std::memory_order_relaxed);
}

void WaveformAnalyzer::requestCancel() noexcept
{
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    m_jobState.store(AnalysisJobState::CancelRequested, std::memory_order_release);
    signalThreadShouldExit();
}

void WaveformAnalyzer::shutdownAndJoin()
{
    if (!isThreadRunning())
        return;

    requestCancel();
    // TrackData and the format manager are raw, owner-held dependencies. Their
    // owner must never destroy or reuse them while run() can still access them.
    stopThread(-1);
}

void WaveformAnalyzer::setCompletionCallback(CompletionCallback callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_completionCallback = std::move(callback);
}

void WaveformAnalyzer::setProgressCallback(ProgressCallback callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_progressCallback = std::move(callback);
}

void WaveformAnalyzer::setChunkCallback(ChunkCallback callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_chunkCallback = std::move(callback);
}

void WaveformAnalyzer::notifyCompletion(bool completed,
                                        AnalysisGeneration completedGeneration,
                                        const QString& completedFilePath,
                                        ResultPtr result)
{
    const bool current = completedGeneration == generation();
    const bool accepted = completed && current && !threadShouldExit();
    m_jobState.store(accepted ? AnalysisJobState::Finished
                              : (threadShouldExit() ? AnalysisJobState::CancelRequested
                                                    : AnalysisJobState::Failed),
                     std::memory_order_release);

    CompletionCallback callback;
    ProgressCallback progress;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        callback = m_completionCallback;
        progress = m_progressCallback;
    }

    if (progress) progress(accepted ? 1.0 : 0.0, false, completedGeneration);

    if (callback)
        callback(accepted, completedGeneration, completedFilePath,
                 accepted ? std::move(result) : ResultPtr{});
}


void WaveformAnalyzer::run()
{
    lowerAnalysisThreadPriority();
    const auto runGeneration = m_runGeneration;
    const QString runFilePath = m_filePath;
    m_jobState.store(AnalysisJobState::Running, std::memory_order_release);
    AnalysisCompletionNotifier completion(*this, runGeneration, runFilePath);

    ProgressCallback progress;
    ChunkCallback chunks;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        progress = m_progressCallback;
        chunks = m_chunkCallback;
    }
    analysis::AnalysisWorkingData working([progress, runGeneration](double value, bool active) {
        if (progress) progress(value, active, runGeneration);
    });
    working.seed(m_seed);

    AnalysisSlot slot(*this);
    if (!slot.acquired())
        return;

    juce::File file(m_filePath.toStdString());
    if (!file.existsAsFile()) return;

    std::unique_ptr<juce::AudioFormatReader> reader(m_formatManager->createReaderFor(file));
    if (!reader) return;

    const juce::int64 totalSamples = reader->lengthInSamples;
    const double      sampleRate   = reader->sampleRate;
    const double      duration     = totalSamples / sampleRate;
    const int         numPoints    = static_cast<int>(duration * m_pointsPerSecond);

    if (numPoints <= 0) return;

    // A user grabbing the platter during the post-load grace period wins before
    // any duration-sized worker buffers or sequential decoder reads begin.
    while (!threadShouldExit()
           && m_realtimeInteractionActive.load(std::memory_order_acquire)) {
        juce::Thread::sleep(4);
    }
    if (threadShouldExit())
        return;

    // Cached waveform from disk — skip the expensive Pass 1+2 and only run BPM/key.
    const int existingRgb      = working.getRgbWaveformSize();
    const int existingExpected = working.getTotalExpected();
    const bool haveFullWaveform = existingExpected >= static_cast<int>(numPoints * 0.95)
                               && existingRgb      >= static_cast<int>(numPoints * 0.95);

    if (!haveFullWaveform) {
        // Instant full-track preview so the deck overview never starts blank.
        if (working.getOverviewRgbData().isEmpty()) {
            auto preview = buildInstantOverview(reader.get(), 512);
            if (!preview.isEmpty())
                working.setOverviewRgbData(std::move(preview));
        }
        working.clearWaveformData();
        working.setTotalExpected(numPoints);
        working.reserve(numPoints);
    } else if (working.getOverviewRgbData().isEmpty()) {
        working.setOverviewRgbData(
            TrackData::downsampleOverview(working.getRgbWaveformData()));
    } else {
        working.reportAnalysisProgress(0.05, true);
    }



    if (!haveFullWaveform) {
        const waveform_internal::EnvelopePassInput envelopeInput{
            *reader,
            &working,
            *this,
            m_pointsPerSecond,
            m_seekHintSec.load(std::memory_order_relaxed),
            [this]() { return m_seekHintSec.load(std::memory_order_relaxed); },
            [this]() {
                return m_realtimeInteractionActive.load(std::memory_order_acquire);
            },
            totalSamples,
            sampleRate,
            numPoints,
            [chunks, runGeneration](int firstBin, int totalBins,
                                    QVector<TrackData::WaveformBin> waveform,
                                    QVector<TrackData::RgbWaveformFrame> rgb) {
                if (chunks)
                    chunks(runGeneration, firstBin, totalBins,
                           std::move(waveform), std::move(rgb));
            },
        };
        if (!waveform_internal::runEnvelopePass(envelopeInput))
            return;

        // Persist the finished waveform immediately. BPM/key/phrase analysis
        // can take considerably longer and does not alter these vectors; tying
        // cache creation to that later stage meant closing or switching the
        // deck lost an otherwise complete waveform.
        WaveformCache::Payload waveformPayload;
        waveformPayload.pointsPerSecond = m_pointsPerSecond;
        waveformPayload.totalExpected = working.getTotalExpected();
        waveformPayload.globalMaxPeak = working.getGlobalMaxPeak();
        waveformPayload.waveform = working.getWaveformData();
        waveformPayload.rgb = working.getRgbWaveformData();
        waveformPayload.peakMip = working.getPeakMipData();
        if (!WaveformCache::saveForFile(m_filePath, waveformPayload)) {
            qWarning() << "[WaveformAnalyzer] Failed to write waveform cache for" << m_filePath;
        } else {
            qDebug() << "[WaveformAnalyzer] Waveform cache written:"
                     << waveformPayload.waveform.size() << "bins,"
                     << waveformPayload.rgb.size() << "rgb frames";
        }
    }

    if (threadShouldExit()) return;

    {
        const waveform_internal::AnalysisOrchestratorInput orchestratorInput{
            *reader,
            &working,
            *this,
            m_pointsPerSecond,
            totalSamples,
            sampleRate,
            duration,
            haveFullWaveform,
        };
        if (!waveform_internal::runAnalysisOrchestrator(orchestratorInput))
            return;
    }

    if (!threadShouldExit()) {
        auto completedResult = std::move(working).finish(m_identity);
        if (completedResult.rgbWaveform && completedResult.peakMip) {
            completedResult.preparedWaveformLines = waveform::prepareWaveformLines(
                *completedResult.rgbWaveform, *completedResult.peakMip);
        }
        if (!analysis::validateResult(completedResult))
            return;
        completedResult.validated = true;
        auto result = std::make_shared<const analysis::AnalysisResult>(std::move(completedResult));
        completion.markCompleted(std::move(result));
    }

}
