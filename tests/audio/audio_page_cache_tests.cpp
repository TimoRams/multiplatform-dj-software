#include "audio/cache/AudioPageCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    if (!cache.waitForPageRange(
            handle, page, page,
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout))) {
        return {};
    }
    return cache.tryGetPage(handle, page);
}

#ifdef Q_OS_LINUX
bool processHasOpenFile(const QString& path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QDir descriptors(QStringLiteral("/proc/self/fd"));
    for (const QString& name : descriptors.entryList(QDir::Files | QDir::System
                                                      | QDir::NoDotAndDotDot)) {
        QString target = QFileInfo(descriptors.filePath(name)).symLinkTarget();
        target.remove(QStringLiteral(" (deleted)"));
        if (target == canonical)
            return true;
    }
    return false;
}
#endif
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
    const QString longScratch = dir.filePath("long-scratch.wav");
    ok &= require(writeWave(mono, 44100, 1, 127), "short mono fixture");
    ok &= require(writeWave(stereo, 48000, 2, 32769), "multi-page stereo fixture");
    ok &= require(writeWave(exact, 96000, 2, 16384), "exact-page fixture");
    ok &= require(writeWave(longScratch, 48000, 1,
                            65 * static_cast<int>(AudioPage::kSamplesPerChannel)),
                  "long scratch fixture");

    // A running deck must be able to promote pages that it previously queued
    // as speculative read-ahead. Loading/warming another deck must not pin the
    // live playhead behind the lower-priority copy, and promotion duplicates
    // must still decode every page at most once.
    {
        const auto bytesPerPage = static_cast<std::uint64_t>(
            AudioPage::kSamplesPerChannel * sizeof(float));
        AudioPageCache promotionCache(70 * bytesPerPage);
        const auto promotionHandle = promotionCache.openTrack({longScratch});
        ok &= require(promotionCache.requestRange(
                          promotionHandle, 0, promotionHandle.pageCount() - 1,
                          AudioCachePriority::Background),
                      "background read-ahead range queued");
        for (std::int64_t page = promotionHandle.pageCount(); page-- > 0;) {
            ok &= require(promotionCache.requestPage(
                              promotionHandle, page,
                              AudioCachePriority::RealtimeCritical),
                          "queued read-ahead page promoted to realtime");
        }
        ok &= require(promotionCache.waitForPageRange(
                          promotionHandle, 0, promotionHandle.pageCount() - 1,
                          std::chrono::seconds(5)),
                      "promoted range becomes resident");
        const auto promotionStats = promotionCache.stats();
        ok &= require(promotionStats.priorityPromotions > 0,
                      "read-ahead promotion path exercised");
        ok &= require(promotionStats.decodedPages
                          == static_cast<std::uint64_t>(promotionHandle.pageCount()),
                      "promotion duplicates decode each page only once");
    }

    {
        AudioPageCache sealedCache(8 * 2 * AudioPage::kSamplesPerChannel * sizeof(float));
        const auto sealedHandle = sealedCache.openTrack({stereo});
        ok &= require(sealedHandle.isValid() && sealedHandle.pageCount() == 3,
                      "seal fixture opens with multiple pages");
        ok &= require(sealedCache.requestRange(
                          sealedHandle, 0, sealedHandle.pageCount() - 1,
                          AudioCachePriority::Background)
                          && sealedCache.waitForPageRange(
                              sealedHandle, 0, sealedHandle.pageCount() - 1,
                              std::chrono::seconds(5)),
                      "all pages become resident before sealing");
        const auto beforeSeal = sealedCache.handleStats(sealedHandle);
        ok &= require(beforeSeal.residentPages == beforeSeal.totalPages && !beforeSeal.sealed,
                      "handle progress reports a fully resident unsealed track");
        ok &= require(sealedCache.sealTrack(sealedHandle),
                      "fully resident track seals for removable-media playback");
        const auto afterSeal = sealedCache.handleStats(sealedHandle);
        ok &= require(afterSeal.sealed && afterSeal.residentPages == afterSeal.totalPages,
                      "sealed track retains every decoded page");
#ifdef Q_OS_LINUX
        ok &= require(!processHasOpenFile(stereo),
                      "sealing closes the removable-media file handle");
#endif
        auto sealedLastPage = sealedCache.tryGetPage(sealedHandle, sealedHandle.pageCount() - 1);
        ok &= require(sealedLastPage && sealedLastPage->validSampleCount == 1,
                      "sealed track remains readable after its reader is closed");
        sealedCache.releaseTrack(sealedHandle);
    }

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
    exactPage = {}; // the wide-scratch regression deliberately needs this tiny budget free

    // Regression for wide scratch seeks: eviction must scale with resident
    // pages, not every page slot in a long track.  The small budget gives the
    // worker only two resident mono pages while the source has 65 pages.
    auto longHandle = cache.openTrack({longScratch});
    ok &= require(longHandle.pageCount() == 65, "long scratch fixture page count");
    const auto beforeWideScratch = cache.stats();
    for (std::int64_t page = 0; page < longHandle.pageCount(); page += 2) {
        ok &= require(cache.requestPage(longHandle, page, AudioCachePriority::ScratchNearPlayhead),
                      "wide scratch request accepted");
        auto guard = waitPage(cache, longHandle, page);
        ok &= require(static_cast<bool>(guard), "wide scratch request becomes resident");
    }
    const auto afterWideScratch = cache.stats();
    const auto evictionDelta = afterWideScratch.evictedPages - beforeWideScratch.evictedPages;
    const auto candidateDelta = afterWideScratch.evictionCandidatesVisited
        - beforeWideScratch.evictionCandidatesVisited;
    ok &= require(evictionDelta > 0 && candidateDelta <= evictionDelta * 8 + 8,
                  "wide scratch eviction remains independent of track page count");
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
    const auto oldGeneration = sharedB.generation();
    cache.releaseTrack(sharedB);
#ifdef Q_OS_LINUX
    ok &= require(!processHasOpenFile(stereo),
                  "last cache release closes the removable-media file handle");
#endif
    cache.releaseTrack(longHandle);
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
