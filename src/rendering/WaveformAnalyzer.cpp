#include "WaveformAnalyzer.h"
#include "WaveformEnvelopePass.h"
#include "WaveformAnalysisOrchestrator.h"
#include "WaveformCache.h"
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

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
    return std::clamp(static_cast<int>(hw) / 3, 1, 4);
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
    explicit AnalysisCompletionNotifier(WaveformAnalyzer& analyzer)
        : m_analyzer(analyzer) {}

    ~AnalysisCompletionNotifier()
    {
        m_analyzer.notifyCompletion(m_completed);
    }

    void markCompleted() { m_completed = true; }

private:
    WaveformAnalyzer& m_analyzer;
    bool m_completed = false;
};

} // namespace

WaveformAnalyzer::WaveformAnalyzer(TrackData* trackData, juce::AudioFormatManager* formatManager, int pointsPerSecond)
    : juce::Thread("WaveformAnalyzerThread"), m_trackData(trackData), m_formatManager(formatManager), m_pointsPerSecond(pointsPerSecond)
{
}

WaveformAnalyzer::~WaveformAnalyzer()
{
    stopAnalysis();
}

void WaveformAnalyzer::startAnalysis(const QString& filePath, double seekHintSec)
{
    stopAnalysis();
    m_filePath = filePath;
    m_seekHintSec.store(seekHintSec, std::memory_order_relaxed);
    if (m_trackData)
        m_trackData->reportAnalysisProgress(0.0, true);
    startThread(juce::Thread::Priority::background);
}

void WaveformAnalyzer::setSeekHint(double positionSec)
{
    m_seekHintSec.store(positionSec, std::memory_order_relaxed);
}

void WaveformAnalyzer::stopAnalysis()
{
    signalThreadShouldExit();
    // Keep the wait short — blocking the GUI thread here freezes the whole app.
    stopThread(150);
}

void WaveformAnalyzer::setCompletionCallback(std::function<void(bool)> callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_completionCallback = std::move(callback);
}

void WaveformAnalyzer::notifyCompletion(bool completed)
{
    if (m_trackData)
        m_trackData->reportAnalysisProgress(completed ? 1.0 : m_trackData->analysisProgress(), false);

    std::function<void(bool)> callback;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        callback = m_completionCallback;
    }

    if (callback)
        callback(completed);
}


void WaveformAnalyzer::run()
{
    AnalysisCompletionNotifier completion(*this);

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

    // Cached waveform from disk — skip the expensive Pass 1+2 and only run BPM/key.
    const int existingRgb      = m_trackData->getRgbWaveformSize();
    const int existingExpected = m_trackData->getTotalExpected();
    const bool haveFullWaveform = existingExpected >= static_cast<int>(numPoints * 0.95)
                               && existingRgb      >= static_cast<int>(numPoints * 0.95);

    if (!haveFullWaveform) {
        // Instant full-track preview so the deck overview never starts blank.
        if (m_trackData->getOverviewRgbData().isEmpty()) {
            auto preview = buildInstantOverview(reader.get(), 512);
            if (!preview.isEmpty())
                m_trackData->setOverviewRgbData(std::move(preview));
        }
        m_trackData->clearWaveformData();
        m_trackData->setTotalExpected(numPoints);
        m_trackData->reserve(numPoints);
    } else if (m_trackData->getOverviewRgbData().isEmpty()) {
        m_trackData->setOverviewRgbData(
            TrackData::downsampleOverview(m_trackData->getRgbWaveformData()));
    } else {
        m_trackData->reportAnalysisProgress(0.05, true);
    }



    if (!haveFullWaveform) {
        const waveform_internal::EnvelopePassInput envelopeInput{
            *reader,
            m_trackData,
            *this,
            m_pointsPerSecond,
            m_seekHintSec.load(std::memory_order_relaxed),
            totalSamples,
            sampleRate,
            numPoints,
        };
        if (!waveform_internal::runEnvelopePass(envelopeInput))
            return;
    }

    if (threadShouldExit()) return;

    {
        const waveform_internal::AnalysisOrchestratorInput orchestratorInput{
            *reader,
            m_trackData,
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

    // -------------------------------------------------------------------------
    // Stage 6: Persist finished waveform vectors into file cache.
    // -------------------------------------------------------------------------
    if (!threadShouldExit()) {
        WaveformCache::Payload payload;
        payload.pointsPerSecond = m_pointsPerSecond;
        payload.totalExpected = m_trackData->getTotalExpected();
        payload.globalMaxPeak = m_trackData->getGlobalMaxPeak();
        payload.waveform = m_trackData->getWaveformData();
        payload.rgb = m_trackData->getRgbWaveformData();
        payload.peakMip = m_trackData->getPeakMipData();

        if (!payload.waveform.isEmpty() && !payload.rgb.isEmpty()) {
            if (!WaveformCache::saveForFile(m_filePath, payload)) {
                qWarning() << "[WaveformAnalyzer] Failed to write waveform cache for" << m_filePath;
            } else {
                qDebug() << "[WaveformAnalyzer] Waveform cache written:" << payload.waveform.size()
                         << "bins," << payload.rgb.size() << "rgb frames";
            }
        }
    }

    if (!threadShouldExit())
        completion.markCompleted();

}
