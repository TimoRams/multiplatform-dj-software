#include "engine/deck/DeckTrackLoader.h"
#include "audio/cache/AudioPageCache.h"
#include "rendering/WaveformCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool writeWave(const QString& path, double sampleRate, int channels, double seconds)
{
    juce::WavAudioFormat format;
    auto fileStream = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fileStream->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    const auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(sampleRate)
        .withNumChannels(channels)
        .withBitsPerSample(16);
    auto writer = format.createWriterFor(stream, options);
    if (!writer) return false;
    const int samples = static_cast<int>(sampleRate * seconds);
    juce::AudioBuffer<float> buffer(channels, samples);
    for (int channel = 0; channel < channels; ++channel)
        for (int i = 0; i < samples; ++i)
            buffer.setSample(channel, i, i < 128 ? 0.0f
                : static_cast<float>(0.15 * std::sin(2.0 * juce::MathConstants<double>::pi
                                                     * 440.0 * i / sampleRate)));
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

struct ResultWaiter {
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<TrackLoadResult> result;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    DeckTrackLoader::CompletionCallback callback()
    {
        return [state = state](TrackLoadResult incoming) {
            {
                std::lock_guard lock(state->mutex);
                state->result.emplace(std::move(incoming));
            }
            state->condition.notify_one();
        };
    }

    bool wait(std::chrono::seconds timeout = std::chrono::seconds(10))
    {
        std::unique_lock lock(state->mutex);
        return state->condition.wait_for(lock, timeout, [this] { return state->result.has_value(); });
    }

    TrackLoadResult& value() { return *state->result; }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir directory;
    ok &= require(directory.isValid(), "temporary directory must be available");
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    const QString monoPath = directory.filePath(QStringLiteral("Artist - Mono.wav"));
    const QString stereoPath = directory.filePath(QStringLiteral("Stereo.wav"));
    const QString mp3Path = QDir::cleanPath(
        QCoreApplication::applicationDirPath()
        + QStringLiteral("/../libs/JUCE/examples/Assets/Notifications/sounds/solemn.mp3"));
    ok &= require(writeWave(monoPath, 44100.0, 1, 0.2), "mono fixture must be generated");
    ok &= require(writeWave(stereoPath, 48000.0, 2, 0.3), "stereo fixture must be generated");

    AudioPageCache cache(16 * 1024 * 1024);
    DeckTrackLoader loader(cache, 100);
    {
        ResultWaiter waiter;
        const auto generation = loader.loadTrack(monoPath, waiter.callback());
        ok &= require(waiter.wait(), "valid mono load must complete");
        ok &= require(waiter.value().succeeded(), "valid mono load must succeed");
        ok &= require(waiter.value().generation == generation, "result generation must match request");
        ok &= require(waiter.value().metadata.channelCount == 1, "mono channel count must be retained");
        ok &= require(std::abs(waiter.value().metadata.sampleRate - 44100.0) < 0.1,
                      "mono sample rate must be retained");
        ok &= require(waiter.value().metadata.title == QStringLiteral("Mono"),
                      "filename metadata fallback must be retained");
        ok &= require(waiter.value().metadata.lengthInSamples > 0,
                      "loader returns metadata only; playback reader belongs to cache");
        ok &= require(waiter.value().cacheHandle.isValid(),
                      "playback cache handle must be prepared off the owner thread");
        ok &= require(static_cast<bool>(cache.tryGetPage(waiter.value().cacheHandle, 0)),
                      "successful load must publish with its first playback page warm");
        cache.releaseTrack(waiter.value().cacheHandle);
    }
    {
        WaveformCache::Payload payload;
        payload.pointsPerSecond = 100;
        payload.totalExpected = 20;
        payload.globalMaxPeak = 0.8f;
        payload.waveform.resize(payload.totalExpected);
        payload.rgb.resize(payload.totalExpected);
        payload.waveform[7].low = 0.65f;
        payload.rgb[7].rms = 0.7f;
        ok &= require(WaveformCache::saveForFile(monoPath, payload),
                      "waveform fixture cache must be saved atomically");

        ResultWaiter waiter;
        loader.loadTrack(monoPath, waiter.callback());
        ok &= require(waiter.wait(), "cached waveform load must complete");
        ok &= require(waiter.value().succeeded(), "cached waveform load must succeed");
        ok &= require(waiter.value().waveformCacheLoaded,
                      "track loader must restore a saved waveform cache");
        ok &= require(waiter.value().waveformCache.rgb.size() == payload.totalExpected
                          && waiter.value().waveformCache.rgb[7].rms > 0.69f,
                      "restored waveform cache must preserve its timeline");
        ok &= require(waiter.value().waveformCache.preparedLines
                          && waiter.value().waveformCache.preparedLines->totalLineCount
                              == static_cast<std::uint32_t>(payload.totalExpected),
                      "cached render lines must be prepared before owner-thread install");
        ok &= require(!waiter.value().instantOverview.isEmpty()
                          && waiter.value().instantOverviewExpected == payload.totalExpected,
                      "cached overview must be prepared before owner-thread install");
        cache.releaseTrack(waiter.value().cacheHandle);
        QFile::remove(WaveformCache::cachePathFor(monoPath, payload.pointsPerSecond));
    }
    {
        // The compact render cache is independently valid and streams the
        // current playhead chunk before the beginning of a long timeline.
        WaveformCache::Payload payload;
        payload.pointsPerSecond = 100;
        payload.totalExpected = 9000;
        payload.globalMaxPeak = 0.9f;
        payload.waveform.resize(payload.totalExpected);
        payload.rgb.resize(payload.totalExpected);
        payload.rgb[8500].rms = 0.75f;
        payload.rgb[8500].low = 0.8f;
        ok &= require(WaveformCache::saveForFile(monoPath, payload),
                      "render-cache fixture must be saved atomically");

        WaveformCache::RenderInfo info;
        ok &= require(WaveformCache::inspectRenderCache(
                          monoPath, payload.pointsPerSecond, &info),
                      "compact render cache must validate independently");
        ok &= require(info.totalLines == payload.totalExpected
                          && !info.overview.isEmpty(),
                      "render cache must retain timeline and instant overview");

        std::vector<int> streamedFirstLines;
        bool restoredProbe = false;
        ok &= require(WaveformCache::streamRenderCache(
                          monoPath, payload.pointsPerSecond,
                          [] { return false; },
                          [] { return 85.0; },
                          [&](int firstLine, int totalLines,
                              std::shared_ptr<const std::vector<WaveformLine>> lines) {
                              streamedFirstLines.push_back(firstLine);
                              if (firstLine <= 8500
                                  && 8500 < firstLine + static_cast<int>(lines->size())) {
                                  const auto& line = (*lines)[static_cast<size_t>(8500 - firstLine)];
                                  restoredProbe = line.maximum > 0 && line.red > 0;
                              }
                              ok &= require(totalLines == payload.totalExpected,
                                            "every render chunk retains total line count");
                          }),
                      "compact render cache must stream every immutable chunk");
        ok &= require(!streamedFirstLines.empty()
                          && streamedFirstLines.front() == 8192,
                      "render-cache restore must start at the playhead chunk");
        ok &= require(restoredProbe,
                      "streamed render lines must preserve amplitude and colour");
        QFile::remove(WaveformCache::cachePathFor(monoPath, payload.pointsPerSecond));
        QFile::remove(WaveformCache::renderCachePathFor(
            monoPath, payload.pointsPerSecond));
    }
    {
        // A long-track cache must not delay publication of the audio handle.
        // Padding a valid fixture exercises the loader's bounded startup policy
        // without creating an hour-long PCM test file.
        WaveformCache::Payload payload;
        payload.pointsPerSecond = 100;
        payload.totalExpected = 20;
        payload.globalMaxPeak = 0.8f;
        payload.waveform.resize(payload.totalExpected);
        payload.rgb.resize(payload.totalExpected);
        ok &= require(WaveformCache::saveForFile(monoPath, payload),
                      "oversized waveform fixture must be created");
        const QString cachePath = WaveformCache::cachePathFor(
            monoPath, payload.pointsPerSecond);
        QFile oversizedCache(cachePath);
        ok &= require(oversizedCache.open(QIODevice::ReadWrite)
                          && oversizedCache.resize(9 * 1024 * 1024),
                      "waveform fixture must exceed the immediate-load budget");
        oversizedCache.close();

        ResultWaiter waiter;
        const auto started = std::chrono::steady_clock::now();
        loader.loadTrack(monoPath, waiter.callback());
        ok &= require(waiter.wait(), "oversized-cache track load must complete");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ok &= require(waiter.value().succeeded()
                          && waiter.value().cacheHandle.isValid(),
                      "oversized cache must not block audio readiness");
        ok &= require(!waiter.value().waveformCacheLoaded,
                      "oversized waveform must use progressive background loading");
        ok &= require(elapsed < std::chrono::seconds(2),
                      "oversized waveform cache must not gate track publication");
        cache.releaseTrack(waiter.value().cacheHandle);
        QFile::remove(cachePath);
    }
    {
        ResultWaiter waiter;
        const auto ownerThread = std::this_thread::get_id();
        std::thread::id completionThread;
        const auto generation = loader.loadTrack(mp3Path, [&waiter, &completionThread](TrackLoadResult result) {
            completionThread = std::this_thread::get_id();
            waiter.callback()(std::move(result));
        });
        ok &= require(waiter.wait(), "real MP3 bootstrap must complete");
        ok &= require(waiter.value().succeeded(), "real MP3 bootstrap must succeed");
        ok &= require(waiter.value().generation == generation, "MP3 generation must match request");
        ok &= require(waiter.value().cacheHandle.isValid(), "MP3 playback cache must be opened before install");
        ok &= require(completionThread != ownerThread,
                      "MP3 reader and cache open must not run on the Qt owner thread");
        cache.releaseTrack(waiter.value().cacheHandle);
    }
    {
        ResultWaiter waiter;
        loader.loadTrack(QString(), waiter.callback());
        ok &= require(waiter.wait(), "empty path must produce a result");
        ok &= require(waiter.value().error == TrackLoadError::EmptyPath,
                      "empty path must have a defined error");
    }
    {
        ResultWaiter waiter;
        loader.loadTrack(directory.filePath(QStringLiteral("missing.wav")), waiter.callback());
        ok &= require(waiter.wait(), "missing path must produce a result");
        ok &= require(waiter.value().error == TrackLoadError::FileNotFound,
                      "missing file must have a defined error");
    }
    {
        const QString badPath = directory.filePath(QStringLiteral("damaged.wav"));
        QFile bad(badPath);
        ok &= require(bad.open(QIODevice::WriteOnly), "damaged fixture must open");
        bad.write("not audio");
        bad.close();
        ResultWaiter waiter;
        loader.loadTrack(badPath, waiter.callback());
        ok &= require(waiter.wait(), "damaged file must produce a result");
        ok &= require(waiter.value().error == TrackLoadError::UnsupportedFormat,
                      "damaged file must fail decoder creation");
    }
    {
        struct CompletionState {
            std::mutex mutex;
            std::condition_variable condition;
            std::vector<std::uint64_t> completed;
        };
        auto state = std::make_shared<CompletionState>();
        const auto collect = [state](TrackLoadResult result) {
            {
                std::lock_guard lock(state->mutex);
                state->completed.push_back(result.generation);
            }
            state->condition.notify_one();
        };
        loader.loadTrack(monoPath, collect);
        loader.loadTrack(stereoPath, collect);
        loader.loadTrack(monoPath, collect);
        const auto current = loader.loadTrack(stereoPath, collect);
        std::unique_lock lock(state->mutex);
        ok &= require(state->condition.wait_for(lock, std::chrono::seconds(10), [&] {
                          return !state->completed.empty() && state->completed.back() == current;
                      }), "latest rapid load must complete");
        ok &= require(state->completed.size() == 1 && state->completed.front() == current,
                      "only the current generation may publish a result");
    }

    loader.requestCancel();
    ok &= require(loader.state() == TrackLoadState::CancelRequested
                      || loader.state() == TrackLoadState::Cancelled,
                  "explicit cancel has a defined state");
    loader.shutdownAndJoin();
    ok &= require(loader.state() == TrackLoadState::ShuttingDown,
                  "shutdown has a terminal state and joins the worker");

    {
        DeckTrackLoader destructorJoin(cache, 100);
        destructorJoin.loadTrack(stereoPath, [](TrackLoadResult) {});
    }

    return ok ? 0 : 1;
}
