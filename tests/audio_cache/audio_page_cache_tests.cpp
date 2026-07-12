#include "audio/cache/AudioPageCache.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool writeWave(const QString& path, double sampleRate, int channels, int samples)
{
    juce::WavAudioFormat format;
    auto fileStream = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fileStream->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    auto options = juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate)
        .withNumChannels(channels).withBitsPerSample(16);
    auto writer = format.createWriterFor(stream, options);
    if (!writer) return false;
    juce::AudioBuffer<float> buffer(channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            buffer.setSample(ch, i, static_cast<float>(0.2 * std::sin(
                2.0 * juce::MathConstants<double>::pi * (220.0 + ch * 110.0) * i / sampleRate)));
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

AudioPageReadGuard waitPage(AudioPageCache& cache, const AudioCacheHandle& handle,
                            std::int64_t page, std::chrono::seconds timeout = std::chrono::seconds(5))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        auto guard = cache.tryGetPage(handle, page);
        if (guard) return guard;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}
}

static_assert(noexcept(std::declval<const AudioPageCache&>().tryGetPage({}, 0)));
static_assert(AudioPage::pageIndexForSample(-1) == -1);
static_assert(AudioPage::pageIndexForSample(0) == 0);
static_assert(AudioPage::pageIndexForSample(16383) == 0);
static_assert(AudioPage::pageIndexForSample(16384) == 1);
static_assert(AudioPage::firstSampleForPage(2) == 32768);

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir dir;
    const QString mono = dir.filePath("mono.wav");
    const QString stereo = dir.filePath("stereo.wav");
    const QString exact = dir.filePath("exact.wav");
    ok &= require(writeWave(mono, 44100, 1, 127), "short mono fixture");
    ok &= require(writeWave(stereo, 48000, 2, 32769), "multi-page stereo fixture");
    ok &= require(writeWave(exact, 96000, 2, 16384), "exact-page fixture");

    AudioPageCache cache(2 * 16384 * sizeof(float) + 1024);
    auto monoHandle = cache.openTrack({mono});
    ok &= require(monoHandle.isValid() && monoHandle.channelCount() == 1, "mono handle metadata");
    ok &= require(monoHandle.pageCount() == 1, "short file page count");
    ok &= require(!cache.tryGetPage(monoHandle, 0), "miss never decodes synchronously");
    ok &= require(cache.requestPage(monoHandle, 0, AudioCachePriority::RealtimeCritical), "request accepted");
    auto monoPage = waitPage(cache, monoHandle, 0);
    ok &= require(monoPage && monoPage->validSampleCount == 127, "short final page count");
    ok &= require(monoPage->channelCount == 1 && monoPage->channelData(0), "mono PCM layout");
    ok &= require(!cache.requestPage(monoHandle, -1, AudioCachePriority::Background), "negative page rejected");
    ok &= require(!cache.requestPage(monoHandle, 1, AudioCachePriority::Background), "past-end page rejected");

    auto sharedA = cache.openTrack({stereo});
    auto sharedB = cache.openTrack({stereo});
    ok &= require(sharedA.id() == sharedB.id() && sharedA.generation() == sharedB.generation(),
                  "same version shares one entry");
    cache.releaseTrack(sharedA);
    ok &= require(cache.requestPage(sharedB, 2, AudioCachePriority::ScratchNearPlayhead),
                  "second user survives first release");
    monoPage = {}; // allow worker eviction of the small mono page
    auto finalPage = waitPage(cache, sharedB, 2);
    ok &= require(finalPage && finalPage->validSampleCount == 1, "last partial stereo page");
    ok &= require(finalPage->channelCount == 2, "stereo channel layout");

    auto exactHandle = cache.openTrack({exact});
    ok &= require(exactHandle.pageCount() == 1 && exactHandle.sampleRate() == 96000,
                  "exact page and sample rate");
    ok &= require(cache.requestRange(exactHandle, 0, 0, AudioCachePriority::PlaybackReadAhead),
                  "range request accepted");
    finalPage = {};
    auto exactPage = waitPage(cache, exactHandle, 0);
    ok &= require(exactPage && exactPage->validSampleCount == 16384, "exact page count");
    ok &= require(cache.stats().residentBytes <= cache.budgetBytes(), "budget is never exceeded");
    ok &= require(cache.stats().evictedPages > 0, "small budget causes worker eviction");
    if (qEnvironmentVariableIsSet("BROCKDJ_CACHE_BENCHMARK")) {
        constexpr int iterations = 200000;
        const auto hitStart = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) { auto guard = cache.tryGetPage(exactHandle, 0); }
        const auto missStart = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) { auto guard = cache.tryGetPage({}, 0); }
        const auto requestStart = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i)
            cache.requestPage(exactHandle, 0, AudioCachePriority::RealtimeCritical);
        const auto end = std::chrono::steady_clock::now();
        const auto ns = [](auto a, auto b) { return std::chrono::duration<double, std::nano>(b - a).count() / iterations; };
        std::cout << "cache benchmark ns/op: hit=" << ns(hitStart, missStart)
                  << " miss=" << ns(missStart, requestStart)
                  << " duplicate-request=" << ns(requestStart, end) << '\n';
    }
    exactPage = {};

    const auto oldGeneration = sharedB.generation();
    cache.releaseTrack(sharedB);
    ok &= require(!cache.requestPage(sharedB, 0, AudioCachePriority::RealtimeCritical),
                  "released final handle is rejected");
    auto reopened = cache.openTrack({stereo});
    ok &= require(reopened.id() != sharedB.id() || reopened.generation() != oldGeneration,
                  "reopen cannot accept stale generation");

    QFile bad(dir.filePath("bad.wav"));
    ok &= require(bad.open(QIODevice::WriteOnly), "damaged fixture opens");
    bad.write("broken"); bad.close();
    ok &= require(!cache.openTrack({bad.fileName()}).isValid(), "damaged decoder rejected");

    std::mt19937 rng(0xBADC0DEu);
    for (int i = 0; i < 2000; ++i) {
        const auto page = static_cast<std::int64_t>(rng() % 3);
        cache.requestPage(reopened, page, static_cast<AudioCachePriority>(rng() % 5));
        auto guard = cache.tryGetPage(reopened, page);
        if (guard)
            for (unsigned ch = 0; ch < guard->channelCount; ++ch)
                ok &= require(std::isfinite(guard->channelData(ch)[0]), "decoded PCM stays finite");
    }

    const auto before = cache.stats();
    ok &= require(before.hits > 0 && before.misses > 0 && before.decodedPages > 0,
                  "realtime statistics are populated");
    cache.shutdownAndJoin();
    ok &= require(!cache.requestPage(reopened, 0, AudioCachePriority::RealtimeCritical),
                  "shutdown rejects requests");

    { AudioPageCache destructorShutdown(1024 * 1024); auto h = destructorShutdown.openTrack({stereo});
      destructorShutdown.requestRange(h, 0, 2, AudioCachePriority::Background); }
    return ok ? 0 : 1;
}
