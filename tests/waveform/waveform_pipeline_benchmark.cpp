#include "audio/cache/AudioPageCache.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "waveform/WaveformAnalyzer.h"
#include "waveform/WaveformCache.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr int kBlockSamples = 512;

int environmentInt(const char* name, int fallback, int minimum, int maximum)
{
    const char* value = std::getenv(name);
    if (!value)
        return fallback;
    return std::clamp(std::atoi(value), minimum, maximum);
}

bool writeBenchmarkWave(const QString& path, int durationSeconds, double detuneHz)
{
    juce::WavAudioFormat format;
    auto file = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!file->openedOk())
        return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(file);
    auto writer = format.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}
                    .withSampleRate(kSampleRate)
                    .withNumChannels(2)
                    .withBitsPerSample(16));
    if (!writer)
        return false;

    juce::AudioBuffer<float> block(2, static_cast<int>(kSampleRate));
    for (int second = 0; second < durationSeconds; ++second) {
        for (int sample = 0; sample < block.getNumSamples(); ++sample) {
            const double time = static_cast<double>(second) + sample / kSampleRate;
            const double beatPhase = std::fmod(time * 2.0, 1.0);
            const double kick = beatPhase < 0.08
                ? std::sin(2.0 * juce::MathConstants<double>::pi
                           * (58.0 + detuneHz) * time)
                    * std::exp(-beatPhase * 45.0)
                : 0.0;
            const double mid = 0.11 * std::sin(
                2.0 * juce::MathConstants<double>::pi * (880.0 + detuneHz) * time);
            const double high = 0.035 * std::sin(
                2.0 * juce::MathConstants<double>::pi * (7'500.0 + detuneHz) * time);
            const float value = static_cast<float>(
                std::clamp(0.52 * kick + mid + high, -0.95, 0.95));
            block.setSample(0, sample, value);
            block.setSample(1, sample, value * 0.97f);
        }
        if (!writer->writeFromAudioSampleBuffer(block, 0, block.getNumSamples()))
            return false;
    }
    return true;
}

bool waitForPage(AudioPageCache& cache, const AudioCacheHandle& handle,
                 std::int64_t pageIndex)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cache.tryGetPage(handle, pageIndex))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::uint64_t peakRssBytes()
{
#if defined(__APPLE__) || defined(__linux__)
    rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
#else
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir configDirectory;
    QTemporaryDir sourceDirectory;
    if (!configDirectory.isValid() || !sourceDirectory.isValid())
        return 2;
    qputenv("XDG_CONFIG_HOME", configDirectory.path().toUtf8());
    QCoreApplication app(argc, argv);

    const int durationSeconds = environmentInt(
        "BROCKDJ_WAVEFORM_BENCHMARK_SECONDS", 30, 10, 300);
    const int playingDecks = environmentInt(
        "BROCKDJ_WAVEFORM_BENCHMARK_PLAYING_DECKS", 1, 1, 4);
    const bool audioPressure = environmentInt(
        "BROCKDJ_WAVEFORM_BENCHMARK_AUDIO_PRESSURE", 0, 0, 1) != 0;
    const QString realTrack = qEnvironmentVariable("BROCKDJ_WAVEFORM_BENCHMARK_TRACK");
    const QString deckAPath = sourceDirectory.filePath(QStringLiteral("deck-a.wav"));
    const QString deckBPath = realTrack.isEmpty()
        ? sourceDirectory.filePath(QStringLiteral("deck-b.wav"))
        : QFileInfo(realTrack).absoluteFilePath();
    const QFileInfo trackInfo(deckBPath);
    if (!realTrack.isEmpty() && (!trackInfo.isFile() || !trackInfo.isReadable())) {
        std::cerr << "The waveform benchmark input is not a readable file\n";
        return 3;
    }
    if (!writeBenchmarkWave(deckAPath, durationSeconds, 0.0)
        || (realTrack.isEmpty() && !writeBenchmarkWave(deckBPath, durationSeconds, 3.0))) {
        std::cerr << "Could not create the waveform benchmark fixture\n";
        return 3;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    AudioPageCache pageCache(64ULL * 1024ULL * 1024ULL);
    const auto deckAHandle = pageCache.openTrack({deckAPath});
    if (!deckAHandle.isValid()
        || !pageCache.requestRange(deckAHandle, 0, 2,
                                   AudioCachePriority::RealtimeCritical)
        || !waitForPage(pageCache, deckAHandle, 0)) {
        return 4;
    }

    CachedPlaybackAudioSource deckAPlayback(pageCache, deckAHandle);
    deckAPlayback.prepareToPlay(kBlockSamples, kSampleRate);
    deckAPlayback.setLoopRangeSamples(0, deckAHandle.lengthInSamples(), kSampleRate);
    std::vector<std::unique_ptr<CachedPlaybackAudioSource>> additionalDecks;
    for (int deck = 1; deck < playingDecks; ++deck) {
        auto playback = std::make_unique<CachedPlaybackAudioSource>(pageCache, deckAHandle);
        playback->prepareToPlay(kBlockSamples, kSampleRate);
        playback->setLoopRangeSamples(0, deckAHandle.lengthInSamples(), kSampleRate);
        additionalDecks.push_back(std::move(playback));
    }
    juce::AudioBuffer<float> output(2, kBlockSamples);

    WaveformAnalyzer analyzer(&formats, 600);
    std::mutex completionMutex;
    std::condition_variable completionCv;
    std::atomic<bool> analysisFinished { false };
    std::atomic<bool> analysisAccepted { false };
    WaveformAnalyzer::ResultPtr result;
    analyzer.setCompletionCallback(
        [&](bool completed, WaveformAnalyzer::AnalysisGeneration, const QString&,
            WaveformAnalyzer::ResultPtr completedResult) {
            {
                std::lock_guard lock(completionMutex);
                analysisAccepted.store(completed, std::memory_order_release);
                result = std::move(completedResult);
                analysisFinished.store(true, std::memory_order_release);
            }
            completionCv.notify_one();
        });

    const auto cacheBefore = pageCache.stats();
    const auto analysisStarted = std::chrono::steady_clock::now();
    constexpr std::array<double, 3> milestones {0.62, 0.84, 0.94};
    std::array<std::atomic<std::int64_t>, milestones.size()> milestoneUsec {};
    analyzer.setProgressCallback(
        [&](double progress, bool active, WaveformAnalyzer::AnalysisGeneration) {
            if (!active)
                return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - analysisStarted).count() + 1;
            for (std::size_t i = 0; i < milestones.size(); ++i) {
                if (progress < milestones[i])
                    continue;
                std::int64_t unset = 0;
                milestoneUsec[i].compare_exchange_strong(unset, elapsed,
                                                         std::memory_order_relaxed);
            }
        });
    analyzer.setBackgroundWorkPaused(audioPressure);
    analyzer.startAnalysis(deckBPath, durationSeconds * 0.5);

    std::uint64_t callbackCount = 0;
    double callbackTotalUs = 0.0;
    double callbackWorstUs = 0.0;
    bool seekIssued = false;
    const auto timeout = analysisStarted + std::chrono::minutes(3);
    while (!analysisFinished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < timeout) {
        if (!seekIssued && callbackCount >= 32) {
            const auto destination = static_cast<juce::int64>(
                durationSeconds * 0.7 * kSampleRate);
            deckAPlayback.prefetchForSeek(destination);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            deckAPlayback.setCommandedReadPosition(destination);
            seekIssued = true;
        }

        const auto callbackStarted = std::chrono::steady_clock::now();
        deckAPlayback.getNextAudioBlock({&output, 0, kBlockSamples});
        for (auto& playback : additionalDecks)
            playback->getNextAudioBlock({&output, 0, kBlockSamples});
        const double callbackUs = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - callbackStarted).count();
        callbackTotalUs += callbackUs;
        callbackWorstUs = std::max(callbackWorstUs, callbackUs);
        ++callbackCount;
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int>(kBlockSamples * 1'000'000.0 / kSampleRate)));
    }

    if (!analysisFinished.load(std::memory_order_acquire)) {
        analyzer.requestCancel();
        analyzer.shutdownAndJoin();
        std::cerr << "Waveform analysis timed out\n";
        return 5;
    }
    while (analyzer.isThreadRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const double analysisMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - analysisStarted).count();
    if (!analysisAccepted.load(std::memory_order_acquire) || !result) {
        std::cerr << "Waveform analysis failed for " << deckBPath.toStdString() << '\n';
        return 6;
    }

    const QString payloadPath = WaveformCache::cachePathFor(deckBPath, 600);
    const QString renderPath = WaveformCache::renderCachePathFor(deckBPath, 600);
    const qint64 payloadBytes = QFileInfo(payloadPath).size();
    const qint64 renderBytes = QFileInfo(renderPath).size();

    WaveformCache::Payload cached;
    const auto cacheLoadStarted = std::chrono::steady_clock::now();
    const bool cacheLoaded = WaveformCache::loadForFile(deckBPath, 600, &cached);
    const double cacheLoadMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cacheLoadStarted).count();
    if (!cacheLoaded)
        return 7;

    const auto cacheAfter = pageCache.stats();
    auto playbackStats = deckAPlayback.cacheStats();
    for (const auto& playback : additionalDecks) {
        const auto stats = playback->cacheStats();
        playbackStats.pageHits += stats.pageHits;
        playbackStats.pageMisses += stats.pageMisses;
        playbackStats.starvationBlocks += stats.starvationBlocks;
        playbackStats.diskReadsFromAudioThread += stats.diskReadsFromAudioThread;
        playbackStats.decoderCallsFromAudioThread += stats.decoderCallsFromAudioThread;
    }
    std::cout << "waveform-pipeline-benchmark"
              << " duration-s=" << durationSeconds
              << " input=" << (realTrack.isEmpty() ? "synthetic" : "file")
              << " analyzed-duration-s=" << result->totalExpected / 600.0
              << " playing-decks=" << playingDecks
              << " audio-pressure=" << audioPressure
              << " analysis-ms=" << analysisMs
              << " waveform-ready-ms=" << milestoneUsec[0].load() / 1000.0
              << " features-near-ready-ms=" << milestoneUsec[1].load() / 1000.0
              << " grid-ready-ms=" << milestoneUsec[2].load() / 1000.0
              << " bpm=" << std::setprecision(14) << result->bpm << std::setprecision(6)
              << " key=" << result->detectedKey.toStdString()
              << " beats=" << result->beats.size()
              << " cache-load-ms=" << cacheLoadMs
              << " payload-bytes=" << payloadBytes
              << " render-bytes=" << renderBytes
              << " peak-rss-bytes=" << peakRssBytes()
              << " deck-a-callbacks=" << callbackCount
              << " callback-avg-us="
              << (callbackCount == 0 ? 0.0 : callbackTotalUs / callbackCount)
              << " callback-worst-us=" << callbackWorstUs
              << " playback-hits=" << playbackStats.pageHits
              << " playback-misses=" << playbackStats.pageMisses
              << " playback-starvation=" << playbackStats.starvationBlocks
              << " cache-decoded="
              << (cacheAfter.decodedPages - cacheBefore.decodedPages)
              << " cache-dropped="
              << (cacheAfter.droppedRequests - cacheBefore.droppedRequests)
              << " spectral-frames=" << cached.spectral.size()
              << " overview-frames=" << cached.overview.size()
              << '\n';
    if (environmentInt("BROCKDJ_WAVEFORM_BENCHMARK_PRINT_PARTS", 0, 0, 1) != 0) {
        for (const auto& part : result->phrases)
            std::cout << "part " << part.label.toStdString() << ' ' << part.startTime
                      << ' ' << part.endTime << '\n';
    }

    return playbackStats.diskReadsFromAudioThread == 0
            && playbackStats.decoderCallsFromAudioThread == 0
            && cacheAfter.droppedRequests == cacheBefore.droppedRequests
        ? 0 : 8;
}
