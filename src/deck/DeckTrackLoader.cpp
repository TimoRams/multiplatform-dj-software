#include "DeckTrackLoader.h"

#include "audio/cache/AudioPageCache.h"
#include "library/CoverArtExtractor.h"
#include "MetadataUtils.h"

#include <QFileInfo>
#include <QFile>
#include <QImage>
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

// Loading and converting a large immutable waveform before publishing the
// audio handle makes long mixes appear unavailable for seconds. Above this
// bounded budget the normal analyzer rebuilds the display progressively after
// playback is already ready. At 600 pps this keeps ordinary tracks on the fast
// cached path while hour-long timelines never gate transport startup.
constexpr qint64 kImmediateWaveformCacheBudgetBytes = 8 * 1024 * 1024;
// Even compact cache files can describe very long timelines where restoring
// every detail before publishing would still delay deck readiness.
constexpr double kImmediateWaveformCacheMaxDurationSeconds = 15.0 * 60.0;
// Cover extraction may require scanning large files; keep the critical load
// path bounded for oversized recordings and leave artwork empty in that case.
constexpr qint64 kInlineCoverExtractionMaxFileBytes = 256ll * 1024ll * 1024ll;

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

std::uint64_t DeckTrackLoader::loadTrack(QString path,
                                         CompletionCallback completion,
                                         RenderChunkCallback renderChunk)
{
    if (m_shuttingDown.load(std::memory_order_acquire)) return currentGeneration();
    const auto generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_mutex);
        m_pending = Request{std::move(path), std::nullopt, generation,
                            std::move(completion), std::move(renderChunk)};
        m_state.store(TrackLoadState::Queued, std::memory_order_release);
    }
    m_waveformSeekHintSec.store(0.0, std::memory_order_relaxed);
    m_condition.notify_one();
    return generation;
}

std::uint64_t DeckTrackLoader::loadExternalTrack(
    QString path, ExternalTrackLoadSnapshot external,
    CompletionCallback completion, RenderChunkCallback renderChunk)
{
    if (m_shuttingDown.load(std::memory_order_acquire)) return currentGeneration();
    const auto generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_mutex);
        m_pending = Request{std::move(path), std::move(external), generation,
                            std::move(completion), std::move(renderChunk)};
        m_state.store(TrackLoadState::Queued, std::memory_order_release);
    }
    m_waveformSeekHintSec.store(0.0, std::memory_order_relaxed);
    m_condition.notify_one();
    return generation;
}

void DeckTrackLoader::setWaveformSeekHint(double positionSec) noexcept
{
    if (std::isfinite(positionSec))
        m_waveformSeekHintSec.store(std::max(0.0, positionSec),
                                    std::memory_order_relaxed);
}

void DeckTrackLoader::setWaveformDemand(
    const waveform::WaveformDemand& demand) noexcept
{
    if (!demand.valid())
        return;
    {
        std::lock_guard lock(m_demandMutex);
        m_waveformDemand = demand;
    }
    setWaveformSeekHint(demand.playheadSec);
}

waveform::WaveformDemand DeckTrackLoader::waveformDemandSnapshot() const noexcept
{
    std::lock_guard lock(m_demandMutex);
    return m_waveformDemand;
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
        const bool restoreRenderCache = result.succeeded()
            && result.waveformRenderCacheDeferred
            && static_cast<bool>(request.renderChunk);
        const QString renderCachePath = result.canonicalPath;
        const int renderLinesPerSecond = result.waveformRenderLinesPerSecond;
        if (request.completion) request.completion(std::move(result));

        if (restoreRenderCache && isCurrent(request.generation)) {
            const auto generation = request.generation;
            WaveformCache::streamRenderCache(
                renderCachePath, renderLinesPerSecond,
                [this, generation]() { return !isCurrent(generation); },
                [this]() {
                    return waveformDemandSnapshot();
                },
                [this, generation, renderLinesPerSecond, &request](
                    int totalLines, WaveformLineBatch chunks) {
                    if (isCurrent(generation) && request.renderChunk) {
                        request.renderChunk(generation, totalLines,
                                            renderLinesPerSecond, std::move(chunks));
                    }
                });
        }
    }
}

TrackLoadResult DeckTrackLoader::prepare(const Request& request)
{
    TrackLoadResult result;
    result.generation = request.generation;
    result.external = request.external;
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
    if (request.external) {
        const auto& supplied = request.external->metadata;
        if (!supplied.title.isEmpty()) result.metadata.title = supplied.title;
        if (!supplied.artist.isEmpty()) result.metadata.artist = supplied.artist;
        if (!supplied.album.isEmpty()) result.metadata.album = supplied.album;
        if (!supplied.genre.isEmpty()) result.metadata.genre = supplied.genre;
        if (!supplied.comment.isEmpty()) result.metadata.comment = supplied.comment;
        if (!supplied.key.isEmpty()) result.metadata.key = supplied.key;
        if (supplied.tagBpm > 0.0) result.metadata.tagBpm = supplied.tagBpm;
        if (result.metadata.durationSec <= 0.0 && supplied.durationSec > 0.0)
            result.metadata.durationSec = supplied.durationSec;
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));

    // Transfer the metadata reader into the playback cache. Opening a long
    // compressed file twice can scan its headers/index twice and used to make
    // long mixes wait before they could even publish an audio handle.
    result.cacheHandle = m_audioPageCache.openTrack(
        {result.canonicalPath}, std::move(reader));
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
        (void)m_audioPageCache.waitForPageRange(
            result.cacheHandle, 0, requiredPages - 1,
            std::chrono::milliseconds(120),
            [this, generation = request.generation] {
                return !isCurrent(generation);
            });
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));

    // TrackLoadResult has always carried coverBytes/coverImage and the deck
    // publishes them to the cover provider, but nothing ever filled them in:
    // the extractor was only wired into the library, never into the deck load.
    // hasCoverArt() was therefore false for every track, so neither the deck
    // nor the controller jog screens could show artwork. Decoding here keeps
    // it off the GUI thread, where this load already runs.
    QByteArray coverBytes;
    if (request.external && !request.external->artworkPath.isEmpty()) {
        QFile artwork(request.external->artworkPath);
        if (artwork.open(QIODevice::ReadOnly))
            coverBytes = artwork.readAll();
    }
    const bool inlineCoverExtractionAllowed = result.metadata.fileSize <= kInlineCoverExtractionMaxFileBytes;
    if (coverBytes.isEmpty() && inlineCoverExtractionAllowed) {
        auto extracted = CoverArtExtractor::extractCoverArt(result.canonicalPath);
        coverBytes = std::move(extracted.first);
    }
    if (!coverBytes.isEmpty()) {
        QImage cover;
        if (cover.loadFromData(coverBytes)) {
            result.coverBytes = std::move(coverBytes);
            result.coverImage = std::move(cover);
        }
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));

    // Small immutable waveforms can be restored before publishing without a
    // visible delay. Large caches must never gate audio readiness: the analyzer
    // will fill those timelines progressively in cursor-priority chunks after
    // this result has already installed the page-backed transport.
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));
    const QFileInfo waveformCacheInfo(
        WaveformCache::cachePathFor(result.canonicalPath, m_waveformPointsPerSecond));
    const bool timelineFitsImmediateBudget = result.metadata.durationSec <= 0.0
        || result.metadata.durationSec <= kImmediateWaveformCacheMaxDurationSeconds;
    const bool cacheFitsImmediateBudget = timelineFitsImmediateBudget
        && (!waveformCacheInfo.exists()
            || waveformCacheInfo.size() <= kImmediateWaveformCacheBudgetBytes);
    result.waveformCacheLoaded = cacheFitsImmediateBudget
        && WaveformCache::loadForFile(
            result.canonicalPath, m_waveformPointsPerSecond, &result.waveformCache);
    if (result.waveformCacheLoaded) {
        result.instantOverviewExpected = result.waveformCache.totalExpected;
        result.instantOverview = TrackData::downsampleOverview(result.waveformCache.rgb);
        // Canonical render lines are CPU-heavy for long tracks. Build them on
        // the loader thread so installing a cached track is pointer publication
        // rather than a full-timeline UI-thread conversion.
        result.waveformCache.preparedLines = waveform::prepareWaveformLines(
            result.waveformCache.rgb);
    } else {
        WaveformCache::RenderInfo renderInfo;
        if (WaveformCache::inspectRenderCache(
                result.canonicalPath, m_waveformPointsPerSecond, &renderInfo)) {
            result.waveformRenderCacheAvailable = true;
            result.waveformRenderCacheDeferred = true;
            result.waveformRenderLinesPerSecond = renderInfo.pointsPerSecond;
            result.waveformRenderTotalLines = renderInfo.totalLines;
            result.instantOverviewExpected = renderInfo.totalLines;
            result.instantOverview = std::move(renderInfo.overview);
        }
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));
    return result;
}
