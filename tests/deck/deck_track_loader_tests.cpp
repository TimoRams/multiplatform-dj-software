#include "deck/DeckTrackLoader.h"
#include "audio/cache/AudioPageCache.h"
#include "waveform/WaveformCache.h"
#include "waveform/WaveformCache.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <array>
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
        QStringLiteral(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/libs/JUCE/examples/Assets/Notifications/sounds/solemn.mp3"));
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
        ExternalTrackLoadSnapshot external;
        external.sourceId = QStringLiteral("test-usb");
        external.sourceAwareId = QStringLiteral("rekordbox:test-usb:1001");
        external.metadata.title = QStringLiteral("External title");
        external.metadata.artist = QStringLiteral("External artist");
        external.metadata.album = QStringLiteral("External album");
        external.metadata.genre = QStringLiteral("External genre");
        external.metadata.key = QStringLiteral("8A");
        external.metadata.tagBpm = 128.0;
        external.analysis.bpm = 128.0;
        external.analysis.beats = {
            {0.5, true, true, 0, 1, 1, 1.0f, false, true},
            {1.0, true, false, 0, 1, 2, 1.0f, false, true}
        };
        external.analysis.beatGridInfo.type = TrackData::BeatGridType::ConstantTempo;
        external.analysis.beatGridInfo.lockedByUser = true;
        external.analysis.beatGridInfo.origin = TrackData::AnalysisOrigin::RekordboxDevice;

        ResultWaiter waiter;
        const auto generation = loader.loadExternalTrack(
            monoPath, external, waiter.callback());
        ok &= require(waiter.wait(), "external USB load must complete");
        ok &= require(waiter.value().succeeded()
                          && waiter.value().generation == generation,
                      "external USB audio must load directly through the normal cache");
        ok &= require(waiter.value().external.has_value()
                          && waiter.value().external->readOnly
                          && waiter.value().external->sourceAwareId
                              == QStringLiteral("rekordbox:test-usb:1001"),
                      "external source/read-only snapshot was not preserved");
        ok &= require(waiter.value().metadata.title == QStringLiteral("External title")
                          && waiter.value().metadata.artist == QStringLiteral("External artist")
                          && waiter.value().metadata.album == QStringLiteral("External album")
                          && waiter.value().metadata.genre == QStringLiteral("External genre")
                          && waiter.value().metadata.key == QStringLiteral("8A")
                          && waiter.value().metadata.tagBpm == 128.0,
                      "external Rekordbox metadata was not installed");
        ok &= require(waiter.value().external->analysis.beats.size() == 2
                          && waiter.value().external->analysis.beats.front().positionSec == 0.5
                          && waiter.value().external->analysis.beatGridInfo.origin
                              == TrackData::AnalysisOrigin::RekordboxDevice,
                      "external exact beatgrid snapshot was not preserved");
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
        payload.totalExpected = 100000;
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
        ok &= require(info.cacheVersion == WaveformCache::kRenderCacheVersion
                          && info.lodLevelCount == 4,
                      "render cache V2 must advertise its complete LOD pyramid");

        std::vector<int> streamedFirstLines;
        std::vector<int> batchSizes;
        bool restoredProbe = false;
        bool seekDemandWonSecondBatch = false;
        int publishedBatch = 0;
        auto liveDemand = waveform::makeViewportDemand(
            85.0, 1200.0, 100.0, true, false, false, 0, 1);
        ok &= require(WaveformCache::streamRenderCache(
                          monoPath, payload.pointsPerSecond,
                          [] { return false; },
                          [&liveDemand] { return liveDemand; },
                          [&](int totalLines, WaveformLineBatch chunks) {
                              ++publishedBatch;
                              batchSizes.push_back(static_cast<int>(chunks.size()));
                              for (const auto& chunk : chunks) {
                                  streamedFirstLines.push_back(chunk.firstLine);
                                  if (chunk.firstLine <= 8500
                                      && 8500 < chunk.firstLine
                                          + static_cast<int>(chunk.lines->size())) {
                                      const auto& line = (*chunk.lines)[static_cast<size_t>(
                                          8500 - chunk.firstLine)];
                                      restoredProbe = line.maximum > 0 && line.red > 0;
                                  }
                                  if (publishedBatch == 2
                                      && chunk.firstLine <= 90000
                                      && 90000 < chunk.firstLine
                                          + static_cast<int>(chunk.lines->size())) {
                                      seekDemandWonSecondBatch = true;
                                  }
                              }
                              if (publishedBatch == 1) {
                                  liveDemand = waveform::makeViewportDemand(
                                      900.0, 1200.0, 100.0,
                                      true, false, false, 0, 2);
                              }
                              ok &= require(totalLines == payload.totalExpected,
                                            "every render batch retains total line count");
                          }),
                      "compact render cache must stream every immutable chunk");
        ok &= require(!streamedFirstLines.empty()
                          && streamedFirstLines.front() == 8192,
                      "render-cache restore must start at the playhead chunk");
        ok &= require(!batchSizes.empty() && batchSizes.front() == 1,
                      "first cache publication must contain only the playhead chunk");
        ok &= require(seekDemandWonSecondBatch,
                      "new seek demand must outrank queued background cache work");
        ok &= require(restoredProbe,
                      "streamed render lines must preserve amplitude and colour");
        bool restoredLodProbe = false;
        int restoredLodTiles = 0;
        ok &= require(WaveformCache::streamRenderLodCache(
                          monoPath, payload.pointsPerSecond, 4,
                          [] { return false; },
                          [&](WaveformCache::LodTile tile) {
                              ++restoredLodTiles;
                              const int probe = 8500 / tile.canonicalLineStride;
                              if (tile.firstSample <= probe
                                  && probe < tile.firstSample
                                      + static_cast<int>(tile.lines->size())) {
                                  restoredLodProbe = (*tile.lines)[static_cast<std::size_t>(
                                      probe - tile.firstSample)].maximum > 0;
                              }
                          }),
                      "render cache V2 must stream persisted LOD tiles");
        ok &= require(restoredLodTiles > 0 && restoredLodProbe,
                      "persisted 75-lines-per-second LOD lost its probe amplitude");

        // Force the loader onto the compact deferred path and verify that its
        // warm-load stream publishes canonical chunks directly. Persisted LOD
        // levels are no longer preloaded wholesale before the visible source.
        QFile::remove(WaveformCache::cachePathFor(
            monoPath, payload.pointsPerSecond));
        struct DeferredRestoreState {
            std::mutex mutex;
            std::condition_variable condition;
            int canonicalLines = 0;
        } restoreState;
        ResultWaiter deferredWaiter;
        const auto deferredGeneration = loader.loadTrack(
            monoPath, deferredWaiter.callback(),
            [&restoreState](std::uint64_t, int, int,
                            WaveformLineBatch chunks) {
                {
                    std::lock_guard lock(restoreState.mutex);
                    for (const auto& chunk : chunks)
                        restoreState.canonicalLines += static_cast<int>(
                            chunk.lines ? chunk.lines->size() : 0);
                }
                restoreState.condition.notify_all();
            });
        ok &= require(deferredWaiter.wait(),
                      "deferred V2 render-cache load must publish audio readiness");
        ok &= require(deferredWaiter.value().generation == deferredGeneration
                          && deferredWaiter.value().waveformRenderCacheDeferred,
                      "loader did not select the deferred V2 render cache");
        {
            std::unique_lock lock(restoreState.mutex);
            ok &= require(restoreState.condition.wait_for(
                              lock, std::chrono::seconds(5), [&] {
                                  return restoreState.canonicalLines
                                      == payload.totalExpected;
                              }),
                          "loader did not publish demand-ordered canonical data");
        }
        cache.releaseTrack(deferredWaiter.value().cacheHandle);

        // V1 files end immediately after canonical lines. Keep accepting that
        // exact historical layout while exposing no persisted LOD levels.
        const QString renderPath = WaveformCache::renderCachePathFor(
            monoPath, payload.pointsPerSecond);
        QFile legacyFile(renderPath);
        ok &= require(legacyFile.open(QIODevice::ReadWrite),
                      "render cache must open for legacy compatibility fixture");
        if (legacyFile.isOpen()) {
            ok &= require(legacyFile.seek(sizeof(quint32)),
                          "legacy fixture version field must be seekable");
            QDataStream versionStream(&legacyFile);
            versionStream << static_cast<qint32>(1);
            const qint64 legacySize = 6 * static_cast<qint64>(sizeof(qint32))
                + static_cast<qint64>(info.overview.size()) * 5
                + static_cast<qint64>(payload.totalExpected) * 8;
            ok &= require(versionStream.status() == QDataStream::Ok
                              && legacyFile.resize(legacySize),
                          "legacy render-cache fixture must be truncated exactly");
            legacyFile.close();
        }
        WaveformCache::RenderInfo legacyInfo;
        ok &= require(WaveformCache::inspectRenderCache(
                          monoPath, payload.pointsPerSecond, &legacyInfo)
                          && legacyInfo.cacheVersion == 1
                          && legacyInfo.lodLevelCount == 0,
                      "render cache V1 compatibility was broken by V2");
        ok &= require(!WaveformCache::streamRenderLodCache(
                          monoPath, payload.pointsPerSecond, 4,
                          [] { return false; }, [](WaveformCache::LodTile) {}),
                      "legacy cache must not pretend to contain persisted LOD");
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
        // A >10 minute render cache can be written directly from immutable
        // line chunks. It must not require the duplicate legacy float payload.
        WaveformCache::Payload payload;
        payload.pointsPerSecond = 100;
        payload.totalExpected = 60'001;
        payload.globalMaxPeak = 0.8f;
        payload.overview.resize(512);
        for (auto& frame : payload.overview) {
            frame.rms = 0.4f;
            frame.low = 0.7f;
            frame.mid = 0.25f;
        }
        auto prepared = std::make_shared<waveform::PreparedWaveformLines>();
        prepared->totalLineCount = payload.totalExpected;
        for (int first = 0; first < payload.totalExpected;
             first += static_cast<int>(WaveformLineStore::kChunkSize)) {
            const int count = std::min(
                static_cast<int>(WaveformLineStore::kChunkSize),
                payload.totalExpected - first);
            auto lines = std::make_shared<std::vector<WaveformLine>>(count);
            for (auto& line : *lines) {
                line.minimum = -12'000;
                line.maximum = 14'000;
                line.red = 220;
                line.green = 90;
                line.blue = 45;
                line.flags = waveform_line_flags::kAvailable
                    | waveform_line_flags::kFinal;
            }
            prepared->chunks.push_back(std::move(lines));
        }
        payload.preparedLines = std::move(prepared);
        ok &= require(WaveformCache::saveForFile(monoPath, payload),
                      "prepared-only long render cache must save");
        ok &= require(!QFile::exists(WaveformCache::cachePathFor(
                          monoPath, payload.pointsPerSecond)),
                      "long track unexpectedly retained legacy float cache");
        WaveformCache::RenderInfo info;
        ok &= require(WaveformCache::inspectRenderCache(
                          monoPath, payload.pointsPerSecond, &info)
                          && info.totalLines == payload.totalExpected
                          && info.overview.size() == payload.overview.size(),
                      "prepared-only render cache lost overview or timeline");
        bool sawPreparedLine = false;
        ok &= require(WaveformCache::streamRenderCache(
                          monoPath, payload.pointsPerSecond,
                          [] { return false; }, {},
                          [&](int, WaveformLineBatch chunks) {
                              for (const auto& chunk : chunks) {
                                  if (chunk.lines && !chunk.lines->empty()
                                      && chunk.lines->front().maximum == 14'000) {
                                      sawPreparedLine = true;
                                  }
                              }
                          })
                          && sawPreparedLine,
                      "prepared-only canonical chunks were not random-readable");
        QFile::remove(WaveformCache::renderCachePathFor(
            monoPath, payload.pointsPerSecond));
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
