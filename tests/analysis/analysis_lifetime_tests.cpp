#include "TrackData.h"
#include "WaveformAnalyzer.h"
#include "waveform/WaveformLineStore.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool writeSilentWave(const QString& path)
{
    juce::WavAudioFormat format;
    auto stream = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!stream->openedOk())
        return false;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), 44100.0, 2, 16, {}, 0));
    if (!writer)
        return false;

    juce::AudioBuffer<float> silence(2, 44100);
    silence.clear();
    for (int second = 0; second < 30; ++second) {
        if (!writer->writeFromAudioSampleBuffer(silence, 0, silence.getNumSamples()))
            return false;
    }
    return true;
}

class CountingReader final : public juce::AudioFormatReader
{
public:
    explicit CountingReader(juce::int64 samples)
        : juce::AudioFormatReader(nullptr, "counting")
    {
        sampleRate = 48000.0;
        lengthInSamples = samples;
        numChannels = 2;
        bitsPerSample = 32;
        usesFloatingPointData = true;
    }

    bool readSamples(int* const* destinations, int destinationCount,
                     int destinationOffset, juce::int64, int sampleCount) override
    {
        ++readCalls;
        for (int channel = 0; channel < destinationCount; ++channel) {
            if (destinations[channel])
                juce::zeromem(destinations[channel] + destinationOffset,
                              static_cast<size_t>(sampleCount) * sizeof(int));
        }
        return true;
    }

    int readCalls = 0;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    bool ok = true;
    {
        AnalyzerResultMailbox mailbox;
        const auto emptyWaveform = std::make_shared<const QVector<TrackData::WaveformBin>>();
        const auto emptyRgb = std::make_shared<const QVector<TrackData::RgbWaveformFrame>>();
        for (int index = 0; index < 10; ++index) {
            mailbox.publishChunk({42, index * 128, 1280, emptyWaveform, emptyRgb});
        }
        const auto firstDrain = mailbox.takeChunks();
        const auto secondDrain = mailbox.takeChunks();
        ok &= require(firstDrain.size() == 6 && secondDrain.size() == 4,
                      "progressive mailbox must bound work per owner-thread tick");

        // A new visible range must jump ahead of older background work, and a
        // later result for the same chunk must coalesce instead of extending a
        // FIFO queue.
        const auto oneSecondRgb = std::make_shared<const QVector<
            TrackData::RgbWaveformFrame>>(600);
        for (const int firstBin : {100 * 600, 101 * 600, 1 * 600,
                                   99 * 600, 102 * 600, 0}) {
            mailbox.publishChunk({43, firstBin, 120 * 600,
                                  emptyWaveform, oneSecondRgb});
        }
        mailbox.publishChunk({43, 100 * 600, 120 * 600,
                              emptyWaveform, oneSecondRgb,
                              WaveformNormalizationState::Final});
        mailbox.publishChunk({43, 100 * 600, 120 * 600,
                              emptyWaveform, oneSecondRgb,
                              WaveformNormalizationState::Preview});
        const auto demand = waveform::makeViewportDemand(
            100.5, 1200.0, 600.0, true, false, false, 0, 43);
        const auto prioritized = mailbox.takeChunks(demand, 600.0);
        ok &= require(!prioritized.empty()
                          && prioritized.front().firstBin == 100 * 600,
                      "visible playhead chunk was delayed behind FIFO background work");
        const auto sameChunkCount = std::count_if(
            prioritized.begin(), prioritized.end(), [](const auto& chunk) {
                return chunk.firstBin == 100 * 600;
            });
        ok &= require(sameChunkCount == 1
                          && prioritized.front().normalizationState
                              == WaveformNormalizationState::Final,
                      "Preview/Final publications for one chunk were not coalesced");

        AnalyzerResultMailbox firstPaintMailbox;
        firstPaintMailbox.publishChunk({44, 0, 120 * 600,
                                        emptyWaveform, oneSecondRgb});
        firstPaintMailbox.publishChunk({44, 60 * 600, 120 * 600,
                                        emptyWaveform, oneSecondRgb});
        const auto firstPaintDemand = waveform::makeViewportDemand(
            50.5, 1200.0, 600.0, true, false, false, 0, 44);
        ok &= require(firstPaintMailbox.takeChunks(
                          firstPaintDemand, 600.0).empty(),
                      "background detail escaped before the playhead chunk");
        firstPaintMailbox.publishChunk({44, 50 * 600, 120 * 600,
                                        emptyWaveform, oneSecondRgb});
        const auto firstPaint = firstPaintMailbox.takeChunks(
            firstPaintDemand, 600.0);
        ok &= require(firstPaint.size() == 1
                          && firstPaint.front().firstBin == 50 * 600,
                      "first published detail did not contain the playhead");

        auto overviewSamples = std::make_shared<const QVector<
            TrackData::RgbWaveformFrame>>(512);
        mailbox.publishOverview({43, 72'000, overviewSamples});
        const auto overview = mailbox.takeOverview();
        ok &= require(overview && overview->generation == 43
                          && overview->totalBins == 72'000
                          && overview->samples == overviewSamples,
                      "instant overview did not cross the bounded owner mailbox");
        mailbox.publishOverview({43, 72'000, overviewSamples});
        mailbox.publishChunk({43, 0, 128, emptyWaveform, emptyRgb});
        mailbox.publish({true, 43, QStringLiteral("track.wav"),
                         std::make_shared<const analysis::AnalysisResult>()});
        ok &= require(mailbox.takeChunks().empty(),
                      "validated completion must discard obsolete progressive chunks");
        ok &= require(!mailbox.takeOverview(),
                      "validated completion must discard its obsolete overview publication");
        ok &= require(mailbox.take().has_value(),
                      "validated completion must remain immediately available");
    }
    {
        CountingReader hourLongReader(60LL * 60LL * 48000LL);
        const auto overview = WaveformAnalyzer::buildInstantOverview(&hourLongReader, 512);
        ok &= require(overview.size() == 512,
                      "long-track instant overview must keep its fixed output size");
        ok &= require(hourLongReader.readCalls <= 512,
                      "instant overview decoder work must not grow with track duration");
    }
    {
        TrackData data;
        WaveformAnalyzer analyzer(&formats, 600);
        std::atomic<int> callbacks{0};
        std::atomic<WaveformAnalyzer::AnalysisGeneration> callbackGeneration{0};
        analyzer.setCompletionCallback(
            [&](bool completed, WaveformAnalyzer::AnalysisGeneration generation, const QString&,
                WaveformAnalyzer::ResultPtr) {
                ok &= require(!completed, "an unreadable file must finish as failed");
                callbackGeneration.store(generation, std::memory_order_release);
                callbacks.fetch_add(1, std::memory_order_release);
            });

        const auto first = analyzer.startAnalysis(QStringLiteral("/not/a/track.wav"));
        analyzer.shutdownAndJoin();
        ok &= require(callbacks.load(std::memory_order_acquire) == 1,
                      "failed analysis must notify exactly once");
        ok &= require(callbackGeneration.load(std::memory_order_acquire) == first,
                      "completion must carry its captured generation");
        ok &= require(analyzer.jobState() != WaveformAnalyzer::AnalysisJobState::Running,
                      "joined failed analysis must have a terminal state");

        const auto second = analyzer.startAnalysis(QStringLiteral("/also/not/a/track.wav"));
        analyzer.shutdownAndJoin();
        ok &= require(second > first, "repeated analysis of the same owner needs a new generation");
    }

    QTemporaryDir dir;
    const QString wavePath = dir.filePath(QStringLiteral("cancel.wav"));
    ok &= require(dir.isValid() && writeSilentWave(wavePath), "test wave must be writable");
    if (ok) {
        WaveformAnalyzer analyzer(&formats, 600);
        std::mutex chunkMutex;
        std::condition_variable chunkReady;
        int firstRgbBin = -1;
        std::atomic<int> publishedOverviewBins{0};
        std::atomic<int> publishedOverviewTotal{0};
        analyzer.setOverviewCallback(
            [&](WaveformAnalyzer::AnalysisGeneration, int totalBins,
                QVector<TrackData::RgbWaveformFrame> overview) {
                publishedOverviewTotal.store(totalBins, std::memory_order_release);
                publishedOverviewBins.store(overview.size(), std::memory_order_release);
            });
        analyzer.setChunkCallback(
            [&](WaveformAnalyzer::AnalysisGeneration, int firstBin, int,
                QVector<TrackData::WaveformBin>,
                QVector<TrackData::RgbWaveformFrame> rgb,
                WaveformNormalizationState) {
                if (rgb.isEmpty())
                    return;
                {
                    std::lock_guard lock(chunkMutex);
                    if (firstRgbBin < 0)
                        firstRgbBin = firstBin;
                }
                chunkReady.notify_one();
            });
        analyzer.startAnalysis(wavePath, 15.0);
        {
            std::unique_lock lock(chunkMutex);
            ok &= require(chunkReady.wait_for(lock, std::chrono::seconds(5),
                                              [&] { return firstRgbBin >= 0; }),
                          "cursor-priority waveform chunk must be published promptly");
        }
        // Without a renderer demand yet, the cold-start range must still span
        // at least one complete immutable store chunk on either side. This is
        // stronger than the old fixed 0.5 s context: it guarantees that the
        // chunk containing the playhead can become READY rather than exposing
        // a partially populated detail tile.
        ok &= require(firstRgbBin
                          == (15 * 600
                              / static_cast<int>(WaveformLineStore::kChunkSize))
                              * static_cast<int>(WaveformLineStore::kChunkSize),
                      "first lazy-loaded range must be the aligned playhead chunk");
        ok &= require(publishedOverviewBins.load(std::memory_order_acquire) == 512
                          && publishedOverviewTotal.load(std::memory_order_acquire)
                              == 30 * 600,
                      "fresh analysis did not publish one complete bounded overview first");
        analyzer.shutdownAndJoin();
    }
    if (ok) {
        WaveformAnalyzer analyzer(&formats, 600);
        std::mutex chunkMutex;
        std::condition_variable chunkReady;
        int firstRgbBin = -1;
        std::atomic<int> scratchOverviewBins{0};
        analyzer.setOverviewCallback(
            [&](WaveformAnalyzer::AnalysisGeneration, int,
                QVector<TrackData::RgbWaveformFrame> overview) {
                scratchOverviewBins.store(overview.size(),
                                          std::memory_order_release);
            });
        analyzer.setChunkCallback(
            [&](WaveformAnalyzer::AnalysisGeneration, int firstBin, int,
                QVector<TrackData::WaveformBin>,
                QVector<TrackData::RgbWaveformFrame> rgb,
                WaveformNormalizationState) {
                if (rgb.isEmpty())
                    return;
                {
                    std::lock_guard lock(chunkMutex);
                    if (firstRgbBin < 0)
                        firstRgbBin = firstBin;
                }
                chunkReady.notify_one();
            });
        analyzer.setRealtimeInteractionActive(true);
        analyzer.startAnalysis(wavePath, 0.0);
        {
            std::unique_lock lock(chunkMutex);
            ok &= require(chunkReady.wait_for(lock, std::chrono::seconds(5),
                                              [&] { return firstRgbBin >= 0; }),
                          "active scratch must still receive its priority waveform chunk");
        }
        ok &= require(firstRgbBin == 0,
                      "cold-start waveform must publish from the playhead before full buffers");
        ok &= require(scratchOverviewBins.load(std::memory_order_acquire) == 512,
                      "active scratch suppressed the always-available overview");
        analyzer.shutdownAndJoin();
    }
    if (ok) {
        TrackData data;
        auto analyzer = std::make_unique<WaveformAnalyzer>(&formats, 600);
        std::atomic<int> acceptedCompletions{0};
        analyzer->setCompletionCallback(
            [&](bool completed, WaveformAnalyzer::AnalysisGeneration, const QString&,
                WaveformAnalyzer::ResultPtr) {
                if (completed)
                    acceptedCompletions.fetch_add(1, std::memory_order_relaxed);
            });
        analyzer->startAnalysis(wavePath);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        analyzer->requestCancel();
        analyzer.reset(); // destructor must join before TrackData leaves scope
        ok &= require(acceptedCompletions.load(std::memory_order_relaxed) == 0,
                      "cancelled/stale generation must not be accepted as complete");
        ok &= require(!data.isAnalyzing(), "cancelled analysis must clear active progress state");
    }

    return ok ? 0 : 1;
}
