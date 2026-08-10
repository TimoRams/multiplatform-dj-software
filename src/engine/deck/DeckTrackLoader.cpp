#include "DeckTrackLoader.h"

#include "audio/cache/AudioPageCache.h"
#include "MetadataUtils.h"

#include <QFileInfo>
#include <QSemaphore>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
void lowerCurrentThreadPriority()
{
#ifdef __linux__
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 10);
#endif
}

int maxConcurrentLoads()
{
    const unsigned cores = std::thread::hardware_concurrency();
    return cores == 0 ? 2 : std::clamp(static_cast<int>(cores) / 2, 1, 6);
}

QSemaphore& loadGate()
{
    static QSemaphore gate(maxConcurrentLoads());
    return gate;
}

TrackMetadataSnapshot readMetadata(const juce::AudioFormatReader& reader,
                                   const QString& path,
                                   const juce::File& file)
{
    TrackMetadataSnapshot result;
    const auto values = metadata::buildMetadataLookup(reader.metadataValues);
    result.title = metadata::metaValue(values, {"title", "id3title", "tit2", "tt2", "name", "tracktitle", "song"});
    result.artist = metadata::metaValue(values, {"artist", "id3artist", "tpe1", "albumartist", "tpe2", "band", "performer", "leadartist"});
    result.album = metadata::metaValue(values, {"album", "id3album", "talb", "record", "albumtitle"});
    result.genre = metadata::metaValue(values, {"genre", "tcon", "contenttype"});
    result.comment = metadata::metaValue(values, {"comment", "comm", "description"});
    result.key = metadata::metaValue(values, {"key", "tkey", "initialkey", "musickey", "keysig"});
    result.year = metadata::metaValue(values, {"year", "date", "tyer", "tdrc"});
    result.trackNumber = metadata::metaValue(values, {"track", "tracknumber", "trck"});
    result.tagBpm = metadata::parseBpmString(
        metadata::metaValue(values, {"bpm", "tbpm", "tmpo", "tempo", "beatsperminute"}));

    const QString baseName = metadata::cleanup(
        QString::fromStdString(file.getFileNameWithoutExtension().toStdString()));
    metadata::filenameHeuristic(baseName, result.title, result.artist);
    result.sampleRate = reader.sampleRate;
    result.lengthInSamples = reader.lengthInSamples;
    result.channelCount = reader.numChannels;
    result.durationSec = reader.sampleRate > 0.0
        ? static_cast<double>(reader.lengthInSamples) / reader.sampleRate : 0.0;
    result.fileSize = file.getSize();
    return result;
}
}

DeckTrackLoader::DeckTrackLoader(AudioPageCache& audioPageCache, int waveformPointsPerSecond)
    : m_waveformPointsPerSecond(waveformPointsPerSecond)
    , m_audioPageCache(audioPageCache)
{
    m_formatManager.registerBasicFormats();
    m_worker = std::thread([this] { workerLoop(); });
}

DeckTrackLoader::~DeckTrackLoader()
{
    shutdownAndJoin();
}

std::uint64_t DeckTrackLoader::loadTrack(QString path, CompletionCallback completion)
{
    if (m_shuttingDown.load(std::memory_order_acquire)) return currentGeneration();
    const auto generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_mutex);
        m_pending = Request{std::move(path), generation, std::move(completion)};
        m_state.store(TrackLoadState::Queued, std::memory_order_release);
    }
    m_condition.notify_one();
    return generation;
}

void DeckTrackLoader::requestCancel() noexcept
{
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(m_mutex);
        m_pending.reset();
    }
    m_state.store(TrackLoadState::CancelRequested, std::memory_order_release);
    m_condition.notify_one();
}

void DeckTrackLoader::shutdownAndJoin() noexcept
{
    if (m_shuttingDown.exchange(true, std::memory_order_acq_rel)) return;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(m_mutex);
        m_pending.reset();
        m_state.store(TrackLoadState::ShuttingDown, std::memory_order_release);
    }
    m_condition.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::uint64_t DeckTrackLoader::currentGeneration() const noexcept
{
    return m_generation.load(std::memory_order_acquire);
}

TrackLoadState DeckTrackLoader::state() const noexcept
{
    return m_state.load(std::memory_order_acquire);
}

bool DeckTrackLoader::isCurrent(std::uint64_t generation) const noexcept
{
    return !m_shuttingDown.load(std::memory_order_acquire)
        && generation == currentGeneration();
}

void DeckTrackLoader::publishState(std::uint64_t generation, TrackLoadState state) noexcept
{
    if (isCurrent(generation)) m_state.store(state, std::memory_order_release);
}

void DeckTrackLoader::workerLoop()
{
    lowerCurrentThreadPriority();
    for (;;) {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] { return m_shuttingDown.load() || m_pending.has_value(); });
            if (m_shuttingDown.load()) return;
            request = std::move(*m_pending);
            m_pending.reset();
        }

        publishState(request.generation, TrackLoadState::Loading);
        auto result = prepare(request);
        if (!isCurrent(request.generation)) {
            m_audioPageCache.releaseTrack(result.cacheHandle);
            auto expected = TrackLoadState::CancelRequested;
            m_state.compare_exchange_strong(expected, TrackLoadState::Cancelled,
                                            std::memory_order_acq_rel);
            continue;
        }
        publishState(request.generation,
                     result.succeeded() ? TrackLoadState::Ready : TrackLoadState::Failed);
        if (request.completion) request.completion(std::move(result));
    }
}

TrackLoadResult DeckTrackLoader::prepare(const Request& request)
{
    TrackLoadResult result;
    result.generation = request.generation;
    auto fail = [&result](TrackLoadError error, QString message) {
        result.error = error;
        result.errorMessage = std::move(message);
        return std::move(result);
    };

    if (request.path.trimmed().isEmpty())
        return fail(TrackLoadError::EmptyPath, QStringLiteral("Track path is empty"));

    const QFileInfo info(request.path);
    if (!info.exists() || !info.isFile())
        return fail(TrackLoadError::FileNotFound, QStringLiteral("Track file does not exist"));
    result.canonicalPath = info.canonicalFilePath();
    if (result.canonicalPath.isEmpty()) result.canonicalPath = info.absoluteFilePath();
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));

    while (!loadGate().tryAcquire(1, 50)) {
        if (!isCurrent(request.generation))
            return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));
    }
    const QSemaphoreReleaser gateRelease(loadGate());

    const juce::File file(result.canonicalPath.toStdString());
    // Loader metadata/previews share one bounded, non-playback reader.  The
    // AudioPageCache opens the sole long-lived playback decoder after install.
    std::unique_ptr<juce::AudioFormatReader> reader(m_formatManager.createReaderFor(file));
    if (!reader)
        return fail(TrackLoadError::UnsupportedFormat, QStringLiteral("Unsupported or damaged audio file"));

    result.metadata = readMetadata(*reader, result.canonicalPath, file);
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));

    // Playback decoder open/page table creation stays off the Qt thread.  The
    // returned handle is installed immediately by the owner thread.
    result.cacheHandle = m_audioPageCache.openTrack({result.canonicalPath});
    if (!result.cacheHandle.isValid())
        return fail(TrackLoadError::DecoderCreationFailed, QStringLiteral("Could not open playback cache"));

    // A successful load means "ready to play/scratch", not merely "decoder
    // opened". Prime a small window before publishing the track so the first
    // audio callback never has to begin from a completely cold cache. This
    // bounded wait happens only on the low-priority loader thread.
    const auto pageCount = result.cacheHandle.pageCount();
    if (pageCount > 0) {
        const auto lastWarmPage = std::min<std::int64_t>(3, pageCount - 1);
        (void)m_audioPageCache.requestPage(result.cacheHandle, 0,
                                           AudioCachePriority::RealtimeCritical);
        (void)m_audioPageCache.requestRange(result.cacheHandle, 0, lastWarmPage,
                                            AudioCachePriority::ScratchNearPlayhead);

        const auto requiredPages = std::min<std::int64_t>(2, pageCount);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
        while (isCurrent(request.generation) && std::chrono::steady_clock::now() < deadline) {
            bool ready = true;
            for (std::int64_t page = 0; page < requiredPages; ++page) {
                if (!m_audioPageCache.tryGetPage(result.cacheHandle, page)) {
                    ready = false;
                    break;
                }
            }
            if (ready)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Restore the immutable waveform before publishing the prepared track.
    // These fields were previously present in TrackLoadResult but never filled,
    // so every reload unnecessarily started a new analysis and visibly changed
    // the waveform again.
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));
    result.waveformCacheLoaded = WaveformCache::loadForFile(
        result.canonicalPath, m_waveformPointsPerSecond, &result.waveformCache);
    if (result.waveformCacheLoaded) {
        // Canonical render lines are CPU-heavy for long tracks. Build them on
        // the loader thread so installing a cached track is pointer publication
        // rather than a full-timeline UI-thread conversion.
        result.waveformCache.preparedLines = waveform::prepareWaveformLines(
            result.waveformCache.rgb, result.waveformCache.peakMip);
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));
    return result;
}
