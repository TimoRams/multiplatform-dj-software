#include "audio/cache/AudioPageCache.h"
#include "audio/internal/ScratchResampler.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

namespace {
int environmentInt(const char* name, int fallback, int minimum, int maximum)
{
    const char* value = std::getenv(name);
    if (!value) return fallback;
    return std::clamp(std::atoi(value), minimum, maximum);
}

bool writeLongMonoWave(const QString& path, int durationSeconds)
{
    juce::WavAudioFormat format;
    auto fileStream = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fileStream->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(44'100).withNumChannels(1).withBitsPerSample(8));
    if (!writer) return false;

    constexpr int chunk = static_cast<int>(AudioPage::kSamplesPerChannel);
    juce::AudioBuffer<float> silence(1, chunk);
    silence.clear();
    std::int64_t remaining = static_cast<std::int64_t>(durationSeconds) * 44'100;
    while (remaining > 0) {
        const int samples = static_cast<int>(std::min<std::int64_t>(chunk, remaining));
        if (!writer->writeFromAudioSampleBuffer(silence, 0, samples)) return false;
        remaining -= samples;
    }
    return true;
}

void printStats(const AudioCacheStats& stats, std::uint64_t callbackCount,
                std::uint64_t pageMisses, double averageCallbackUs,
                double worstCallbackUs, std::uint64_t callbackOverruns,
                const engine::audio::ScratchCacheStats& scratchStats)
{
    const double averageQueueUs = stats.workerRequests == 0 ? 0.0
        : static_cast<double>(stats.workerRequestLatencyMicros) / stats.workerRequests;
    const double averageDecodeUs = stats.decodedPages == 0 ? 0.0
        : static_cast<double>(stats.decodeMicros) / stats.decodedPages;
    const double averageEvictionUs = stats.evictionScans == 0 ? 0.0
        : static_cast<double>(stats.evictionScanMicros) / stats.evictionScans;
    std::cout << "audio-cache-stress callbacks=" << callbackCount
              << " misses=" << pageMisses
              << " queued=" << stats.queuedRequests
              << " dropped=" << stats.droppedRequests
              << " decoded=" << stats.decodedPages
              << " evicted=" << stats.evictedPages
              << " queue-avg-us=" << averageQueueUs
              << " queue-worst-us=" << stats.worstWorkerRequestLatencyMicros
              << " decode-avg-us=" << averageDecodeUs
              << " decode-worst-us=" << stats.worstDecodeMicros
              << " eviction-scans=" << stats.evictionScans
              << " eviction-candidates=" << stats.evictionCandidatesVisited
              << " eviction-avg-us=" << averageEvictionUs
              << " eviction-worst-us=" << stats.worstEvictionScanMicros
              << " reader-wait-worst-us=" << stats.worstEvictionReaderWaitMicros
              << " queue-peak=" << stats.peakPendingRequests
              << " callback-avg-us=" << averageCallbackUs
              << " callback-worst-us=" << worstCallbackUs
              << " callback-overruns=" << callbackOverruns
              << " scratch-starvation-blocks=" << scratchStats.starvationBlocks
              << " scratch-dropped=" << scratchStats.droppedRequests << '\n';
}
}

// Non-interactive repro for cache starvation under wide, rapidly reversing
// scratch seeks. Defaults intentionally match the production bug report:
// a one-hour source and a 60-second sustained run. Tune only via the named
// environment variables when running on constrained CI hardware.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const int runtimeSeconds = environmentInt("BROCKDJ_CACHE_STRESS_SECONDS", 60, 1, 600);
    const int trackSeconds = environmentInt("BROCKDJ_CACHE_STRESS_TRACK_SECONDS", 3600, 60, 7200);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const QString path = directory.filePath("long-scratch-source.wav");
    if (!writeLongMonoWave(path, trackSeconds)) return 3;

    // Four mono pages: a deliberately small but valid live-cache budget, so
    // wide scratch jumps continuously exercise eviction rather than RAM size.
    AudioPageCache cache(4 * AudioPage::kSamplesPerChannel * sizeof(float) + 4096);
    const auto handle = cache.openTrack({path});
    if (!handle.isValid() || handle.pageCount() < 8) return 4;

    engine::audio::ScratchResampler scratch;
    scratch.prepare(2, 128, 44'100.0);
    scratch.setTrackLengthSamples(handle.lengthInSamples());
    scratch.setTrackCacheSource(&cache, handle);
    scratch.reset(static_cast<double>(handle.lengthInSamples() / 2));
    juce::AudioBuffer<float> scratchOutput(2, 128);

    std::mt19937 random(0x5CA7C4E5u);
    std::uniform_int_distribution<std::int64_t> page(0, handle.pageCount() - 1);
    constexpr int callbackSamples = 128;
    constexpr int jumpEveryCallbacks = 6;
    std::int64_t cursor = handle.pageCount() / 2;
    std::int64_t direction = 1;
    std::uint64_t callbackCount = 0;
    std::uint64_t misses = 0;
    std::uint64_t callbackOverruns = 0;
    double totalCallbackUs = 0.0;
    double worstCallbackUs = 0.0;
    bool outputFinite = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(runtimeSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const bool jumped = callbackCount % jumpEveryCallbacks == 0;
        if (jumped) {
            direction = -direction;
            cursor = page(random); // broad, non-sequential platter movement
        }
        if (jumped)
            scratch.setReadPositionSamples(static_cast<double>(cursor * AudioPage::kSamplesPerChannel));
        const auto callbackStartedAt = std::chrono::steady_clock::now();
        scratch.processBlock(direction * 1.5, {&scratchOutput, 0, callbackSamples});
        const auto callbackUs = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - callbackStartedAt).count();
        totalCallbackUs += callbackUs;
        worstCallbackUs = std::max(worstCallbackUs, callbackUs);
        if (callbackUs > callbackSamples * 1'000'000.0 / 44'100.0)
            ++callbackOverruns;
        const auto activePage = AudioPage::pageIndexForSample(
            static_cast<std::int64_t>(scratch.readPosition()));
        if (!cache.tryGetPage(handle, activePage)) ++misses;
        for (int channel = 0; channel < scratchOutput.getNumChannels(); ++channel)
            for (int sample = 0; sample < callbackSamples; ++sample)
                outputFinite = outputFinite && std::isfinite(scratchOutput.getSample(channel, sample));
        ++callbackCount;
        // Mimic a 128-sample/44.1 kHz audio callback without a busy loop that
        // would hide worker starvation by consuming all available CPU.
        std::this_thread::sleep_for(std::chrono::microseconds(
            callbackSamples * 1'000'000 / 44'100));
    }

    // Let the last requested scratch neighbourhood drain, then report the
    // worker/cache telemetry used to compare before and after a change.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto stats = cache.stats();
    const auto scratchStats = scratch.cacheStats();
    printStats(stats, callbackCount, misses,
               totalCallbackUs / static_cast<double>(callbackCount), worstCallbackUs,
               callbackOverruns, scratchStats);
    return stats.droppedRequests == 0 && stats.pendingRequests == 0
            && scratchStats.droppedRequests == 0 && outputFinite ? 0 : 5;
}
