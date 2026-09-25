#include "DeckTrackLoader.h"

#include "audio/cache/AudioPageCache.h"
#include "library/CoverArtExtractor.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QSemaphore>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

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
QString fromJuce(const juce::String& value)
{
    return QString::fromUtf8(value.toRawUTF8());
}

QString cleanupMetadata(QString text)
{
    if (text.isEmpty())
        return text;
    text.replace(QRegularExpression(QStringLiteral("[\\x00\\r\\n\\t]+")),
                 QStringLiteral(" "));
    return text.simplified().trimmed();
}

QString normaliseMetadataKey(const QString& key)
{
    QString result;
    result.reserve(key.size());
    for (const QChar ch : key.trimmed().toLower()) {
        if (ch.isLetterOrNumber())
            result.append(ch);
    }
    return result;
}

QHash<QString, QString> buildMetadataLookup(
    const juce::StringPairArray& metadata)
{
    QHash<QString, QString> map;
    const auto keys = metadata.getAllKeys();
    const auto values = metadata.getAllValues();
    for (int index = 0; index < metadata.size(); ++index) {
        const QString value = cleanupMetadata(fromJuce(values[index]));
        if (value.isEmpty())
            continue;
        const QString normalizedKey =
            normaliseMetadataKey(fromJuce(keys[index]));
        if (!normalizedKey.isEmpty() && !map.contains(normalizedKey))
            map.insert(normalizedKey, value);

        const QString rawKey = cleanupMetadata(fromJuce(keys[index]));
        if (!rawKey.contains(QLatin1Char(':')))
            continue;
        for (const auto& part :
             rawKey.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
            const QString alternate = normaliseMetadataKey(part);
            if (!alternate.isEmpty() && !map.contains(alternate))
                map.insert(alternate, value);
        }
    }
    return map;
}

QString metadataValue(const QHash<QString, QString>& map,
                      std::initializer_list<const char*> candidates)
{
    for (const char* candidate : candidates) {
        const auto found = map.constFind(
            normaliseMetadataKey(QString::fromUtf8(candidate)));
        if (found != map.cend())
            return found.value();
    }
    return {};
}

void applyFilenameHeuristic(
    const QString& baseName,
    QString& title,
    QString& artist)
{
    if (title.isEmpty())
        title = baseName;
    if (!artist.isEmpty())
        return;

    static const QRegularExpression pattern(
        QStringLiteral("^\\s*(.+?)\\s*[-–]\\s*(.+)\\s*$"));
    const auto match = pattern.match(baseName);
    if (!match.hasMatch())
        return;

    const QString candidateArtist = cleanupMetadata(match.captured(1));
    const QString candidateTitle = cleanupMetadata(match.captured(2));
    if (!candidateArtist.isEmpty())
        artist = candidateArtist;
    if (!candidateTitle.isEmpty())
        title = candidateTitle;
}

double parseBpmString(const QString& raw)
{
    if (raw.isEmpty())
        return 0.0;
    const QString cleaned =
        QString(raw).trimmed().replace(QLatin1Char(','), QLatin1Char('.'));
    static const QRegularExpression numberPattern(
        QStringLiteral("([0-9]+(?:\\.[0-9]+)?)"));
    const auto match = numberPattern.match(cleaned);
    if (!match.hasMatch())
        return 0.0;

    bool ok = false;
    const double value = match.captured(1).toDouble(&ok);
    return ok ? value : 0.0;
}

struct TagLibTags {
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    QString year;
    QString trackNumber;
    double bpm = 0.0;
};

std::optional<TagLibTags> readTagLibTags(const QString& path)
{
    TagLib::FileRef file(path.toUtf8().constData());
    if (file.isNull() || file.tag() == nullptr)
        return std::nullopt;

    const TagLib::Tag* tag = file.tag();
    TagLibTags result;
    result.title =
        cleanupMetadata(QString::fromStdWString(tag->title().toWString()));
    result.artist =
        cleanupMetadata(QString::fromStdWString(tag->artist().toWString()));
    result.album =
        cleanupMetadata(QString::fromStdWString(tag->album().toWString()));
    result.genre =
        cleanupMetadata(QString::fromStdWString(tag->genre().toWString()));
    result.comment =
        cleanupMetadata(QString::fromStdWString(tag->comment().toWString()));
    if (tag->year() > 0)
        result.year = QString::number(tag->year());
    if (tag->track() > 0)
        result.trackNumber = QString::number(tag->track());

    if (file.file() != nullptr) {
        const TagLib::PropertyMap properties = file.file()->properties();
        for (const char* key : {"BPM", "TBPM"}) {
            const auto found = properties.find(TagLib::String(key));
            if (found == properties.end() || found->second.isEmpty())
                continue;
            result.bpm = parseBpmString(
                QString::fromStdWString(found->second.front().toWString()));
            if (result.bpm > 0.0)
                break;
        }
    }
    return result;
}

void lowerCurrentThreadPriority()
{
#ifdef __linux__
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 15);
#endif
}

QSemaphore& loadGate()
{
    // Decoder startup competes for the same storage needed by live playback.
    // Keep the audio-critical preparation phase predictable across all decks.
    static QSemaphore gate(1);
    return gate;
}

QSemaphore& visualLoadGate()
{
    static QSemaphore gate(1);
    return gate;
}

std::mutex g_audioLoadStateMutex;
std::condition_variable g_audioLoadStateChanged;
int g_pendingAudioLoads = 0;

void beginAudioLoad()
{
    std::lock_guard lock(g_audioLoadStateMutex);
    ++g_pendingAudioLoads;
}

void finishAudioLoad() noexcept
{
    {
        std::lock_guard lock(g_audioLoadStateMutex);
        --g_pendingAudioLoads;
    }
    g_audioLoadStateChanged.notify_all();
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
    const auto values = buildMetadataLookup(reader.metadataValues);
    result.title = metadataValue(
        values, {"title", "id3title", "tit2", "tt2", "name", "tracktitle", "song"});
    result.artist = metadataValue(
        values, {"artist", "id3artist", "tpe1", "albumartist", "tpe2",
                 "band", "performer", "leadartist"});
    result.album = metadataValue(
        values, {"album", "id3album", "talb", "record", "albumtitle"});
    result.genre = metadataValue(
        values, {"genre", "tcon", "contenttype"});
    result.comment = metadataValue(
        values, {"comment", "comm", "description"});
    result.key = metadataValue(
        values, {"key", "tkey", "initialkey", "musickey", "keysig"});
    result.year = metadataValue(
        values, {"year", "date", "tyer", "tdrc"});
    result.trackNumber = metadataValue(
        values, {"track", "tracknumber", "trck"});
    result.tagBpm = parseBpmString(metadataValue(
        values, {"bpm", "tbpm", "tmpo", "tempo", "beatsperminute"}));

    // JUCE's decoders don't agree on tag parsing across platforms: on macOS,
    // CoreAudioFormat is registered ahead of the format-specific readers and
    // silently exposes no ID3 metadata at all for many files (see
    // readTagLibTags for details), which previously left
    // title/artist to fall through to the raw filename-split heuristic below
    // and produced swapped/garbled results. TagLib parses tags identically on
    // every platform, so its values take priority whenever present.
    if (const auto tagLibTags = readTagLibTags(path)) {
        if (!tagLibTags->title.isEmpty()) result.title = tagLibTags->title;
        if (!tagLibTags->artist.isEmpty()) result.artist = tagLibTags->artist;
        if (!tagLibTags->album.isEmpty()) result.album = tagLibTags->album;
        if (!tagLibTags->genre.isEmpty()) result.genre = tagLibTags->genre;
        if (!tagLibTags->comment.isEmpty()) result.comment = tagLibTags->comment;
        if (!tagLibTags->year.isEmpty()) result.year = tagLibTags->year;
        if (!tagLibTags->trackNumber.isEmpty()) result.trackNumber = tagLibTags->trackNumber;
        if (tagLibTags->bpm > 0.0) result.tagBpm = tagLibTags->bpm;
    }

    const QString baseName = cleanupMetadata(
        QString::fromStdString(file.getFileNameWithoutExtension().toStdString()));
    applyFilenameHeuristic(baseName, result.title, result.artist);
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
                                         RenderChunkCallback renderChunk,
                                         VisualCompletionCallback visualCompletion)
{
    if (m_shuttingDown.load(std::memory_order_acquire)) return currentGeneration();
    const auto generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_mutex);
        if (!m_pending)
            beginAudioLoad();
        m_pending = Request{std::move(path), std::nullopt, generation,
                            std::move(completion), std::move(renderChunk),
                            std::move(visualCompletion)};
        m_state.store(TrackLoadState::Queued, std::memory_order_release);
    }
    m_waveformSeekHintSec.store(0.0, std::memory_order_relaxed);
    m_condition.notify_one();
    return generation;
}

std::uint64_t DeckTrackLoader::loadExternalTrack(
    QString path, ExternalTrackLoadSnapshot external,
    CompletionCallback completion, RenderChunkCallback renderChunk,
    VisualCompletionCallback visualCompletion)
{
    if (m_shuttingDown.load(std::memory_order_acquire)) return currentGeneration();
    const auto generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_mutex);
        if (!m_pending)
            beginAudioLoad();
        m_pending = Request{std::move(path), std::move(external), generation,
                            std::move(completion), std::move(renderChunk),
                            std::move(visualCompletion)};
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
    bool removedPending = false;
    {
        std::lock_guard lock(m_mutex);
        removedPending = m_pending.has_value();
        m_pending.reset();
    }
    if (removedPending)
        finishAudioLoad();
    else
        g_audioLoadStateChanged.notify_all();
    m_state.store(TrackLoadState::CancelRequested, std::memory_order_release);
    m_condition.notify_one();
}

void DeckTrackLoader::shutdownAndJoin() noexcept
{
    if (m_shuttingDown.exchange(true, std::memory_order_acq_rel)) return;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    bool removedPending = false;
    {
        std::lock_guard lock(m_mutex);
        removedPending = m_pending.has_value();
        m_pending.reset();
        m_state.store(TrackLoadState::ShuttingDown, std::memory_order_release);
    }
    if (removedPending)
        finishAudioLoad();
    else
        g_audioLoadStateChanged.notify_all();
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
        finishAudioLoad();
        if (!isCurrent(request.generation)) {
            m_audioPageCache.releaseTrack(result.cacheHandle);
            auto expected = TrackLoadState::CancelRequested;
            m_state.compare_exchange_strong(expected, TrackLoadState::Cancelled,
                                            std::memory_order_acq_rel);
            continue;
        }
        TrackVisualResult visuals;
        bool restoreRenderCache = false;
        int renderLinesPerSecond = 0;
        if (result.succeeded() && !request.visualCompletion) {
            visuals = prepareVisuals(request, result.canonicalPath, result.metadata);
            restoreRenderCache = visuals.waveformRenderCacheDeferred;
            renderLinesPerSecond = visuals.waveformRenderLinesPerSecond;
            result.waveformCache = std::move(visuals.waveformCache);
            result.instantOverview = std::move(visuals.instantOverview);
            result.instantOverviewExpected = visuals.instantOverviewExpected;
            result.waveformCacheLoaded = visuals.waveformCacheLoaded;
            result.waveformRenderCacheAvailable = visuals.waveformRenderCacheAvailable;
            result.waveformRenderCacheDeferred = visuals.waveformRenderCacheDeferred;
            result.waveformRenderLinesPerSecond = visuals.waveformRenderLinesPerSecond;
            result.waveformRenderTotalLines = visuals.waveformRenderTotalLines;
            result.coverBytes = std::move(visuals.coverBytes);
            result.coverImage = std::move(visuals.coverImage);
        }

        publishState(request.generation,
                     result.succeeded() ? TrackLoadState::Ready : TrackLoadState::Failed);
        const bool succeeded = result.succeeded();
        const QString canonicalPath = result.canonicalPath;
        const TrackMetadataSnapshot metadata = result.metadata;
        if (request.completion) request.completion(std::move(result));

        if (succeeded && request.visualCompletion && isCurrent(request.generation)) {
            visuals = prepareVisuals(request, canonicalPath, metadata);
            restoreRenderCache = visuals.waveformRenderCacheDeferred;
            renderLinesPerSecond = visuals.waveformRenderLinesPerSecond;
            if (isCurrent(request.generation))
                request.visualCompletion(std::move(visuals));
        }

        if (succeeded && restoreRenderCache
            && request.renderChunk && isCurrent(request.generation)) {
            const auto generation = request.generation;
            WaveformCache::streamRenderCache(
                canonicalPath, renderLinesPerSecond,
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
                                            AudioCachePriority::PlaybackReadAhead);

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

    return result;
}

TrackVisualResult DeckTrackLoader::prepareVisuals(
    const Request& request,
    const QString& canonicalPath,
    const TrackMetadataSnapshot& metadata)
{
    TrackVisualResult result;
    result.generation = request.generation;
    result.canonicalPath = canonicalPath;

    while (!visualLoadGate().tryAcquire(1, 20)) {
        if (!isCurrent(request.generation))
            return result;
    }
    const QSemaphoreReleaser visualGateRelease(visualLoadGate());
    {
        std::unique_lock lock(g_audioLoadStateMutex);
        g_audioLoadStateChanged.wait(lock, [this, generation = request.generation] {
            return g_pendingAudioLoads == 0
                || !isCurrent(generation);
        });
    }
    if (!isCurrent(request.generation))
        return result;

    QByteArray coverBytes;
    if (request.external && !request.external->artworkPath.isEmpty()) {
        QFile artwork(request.external->artworkPath);
        if (artwork.open(QIODevice::ReadOnly))
            coverBytes = artwork.readAll();
    }
    const bool inlineCoverExtractionAllowed = metadata.fileSize <= kInlineCoverExtractionMaxFileBytes;
    if (coverBytes.isEmpty() && inlineCoverExtractionAllowed) {
        auto extracted = CoverArtExtractor::extractCoverArt(canonicalPath);
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
        return result;

    // Small immutable waveforms are restored before visual publication. Large
    // caches use the progressive path and never gate audio readiness.
    const QFileInfo waveformCacheInfo(
        WaveformCache::cachePathFor(canonicalPath, m_waveformPointsPerSecond));
    const bool timelineFitsImmediateBudget = metadata.durationSec <= 0.0
        || metadata.durationSec <= kImmediateWaveformCacheMaxDurationSeconds;
    const bool cacheFitsImmediateBudget = timelineFitsImmediateBudget
        && (!waveformCacheInfo.exists()
            || waveformCacheInfo.size() <= kImmediateWaveformCacheBudgetBytes);
    result.waveformCacheLoaded = cacheFitsImmediateBudget
        && WaveformCache::loadForFile(
            canonicalPath, m_waveformPointsPerSecond, &result.waveformCache);
    if (result.waveformCacheLoaded) {
        result.instantOverviewExpected = result.waveformCache.totalExpected;
        result.instantOverview = TrackData::downsampleOverview(result.waveformCache.spectral);
        // Canonical render lines are CPU-heavy for long tracks. Build them on
        // the loader thread so installing a cached track is pointer publication
        // rather than a full-timeline UI-thread conversion.
        result.waveformCache.preparedLines = waveform::prepareWaveformLines(
            result.waveformCache.waveform, result.waveformCache.spectral);
    } else {
        WaveformCache::RenderInfo renderInfo;
        if (WaveformCache::inspectRenderCache(
                canonicalPath, m_waveformPointsPerSecond, &renderInfo)) {
            result.waveformRenderCacheAvailable = true;
            result.waveformRenderCacheDeferred = true;
            result.waveformRenderLinesPerSecond = renderInfo.pointsPerSecond;
            result.waveformRenderTotalLines = renderInfo.totalLines;
            result.instantOverviewExpected = renderInfo.totalLines;
            result.instantOverview = std::move(renderInfo.overview);
        }
    }
    return result;
}
