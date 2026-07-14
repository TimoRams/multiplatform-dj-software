#include "DeckTrackLoader.h"

#include "engine/audio/MetadataUtils.h"
#include "library/CoverArtExtractor.h"
#include "rendering/WaveformAnalyzer.h"

#include <QFileInfo>
#include <QSemaphore>
#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <algorithm>
#include <cmath>
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

    TagLib::FileRef tagFile(path.toLocal8Bit().constData());
    if (!tagFile.isNull() && tagFile.tag()) {
        const auto* tag = tagFile.tag();
        const auto text = [](const TagLib::String& value) {
            return metadata::cleanup(QString::fromStdString(value.to8Bit(true)));
        };
        if (result.title.isEmpty()) result.title = text(tag->title());
        if (result.artist.isEmpty()) result.artist = text(tag->artist());
        if (result.album.isEmpty()) result.album = text(tag->album());
        if (result.genre.isEmpty()) result.genre = text(tag->genre());
        if (result.comment.isEmpty()) result.comment = text(tag->comment());
        if (result.year.isEmpty() && tag->year() > 0) result.year = QString::number(tag->year());
        if (result.trackNumber.isEmpty() && tag->track() > 0) result.trackNumber = QString::number(tag->track());
    }

    if (const auto v1 = metadata::readId3v1(path)) {
        if (result.title.isEmpty()) result.title = v1->title;
        if (result.artist.isEmpty()) result.artist = v1->artist;
        if (result.album.isEmpty()) result.album = v1->album;
        if (result.year.isEmpty()) result.year = v1->year;
    }

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

DeckTrackLoader::DeckTrackLoader(int waveformPointsPerSecond)
    : m_waveformPointsPerSecond(waveformPointsPerSecond)
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

    publishState(request.generation, TrackLoadState::Preparing);
    result.waveformCacheLoaded = WaveformCache::loadForFile(
        result.canonicalPath, m_waveformPointsPerSecond, &result.waveformCache)
        && !result.waveformCache.waveform.isEmpty() && !result.waveformCache.rgb.isEmpty();
    if (result.waveformCacheLoaded) {
        const int expected = result.waveformCache.totalExpected > 0
            ? result.waveformCache.totalExpected : result.waveformCache.waveform.size();
        result.waveformCacheLoaded = expected > 0
            && result.waveformCache.waveform.size() >= static_cast<int>(expected * 0.98)
            && result.waveformCache.rgb.size() >= static_cast<int>(expected * 0.98);
    }
    if (!result.waveformCacheLoaded) {
        result.instantOverview = WaveformAnalyzer::buildInstantOverview(reader.get());
        result.instantOverviewExpected = static_cast<int>(
            result.metadata.durationSec * m_waveformPointsPerSecond);
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));

    result.coverBytes = CoverArtExtractor::extractCoverArt(result.canonicalPath).first;
    if (!result.coverBytes.isEmpty()) result.coverImage.loadFromData(result.coverBytes);

    if (reader->sampleRate > 0.0) {
        constexpr double maxScanSec = 10.0;
        constexpr float silenceThreshold = 0.001f;
        constexpr int blockSize = 1024;
        const auto maxScan = std::min<juce::int64>(reader->lengthInSamples,
            static_cast<juce::int64>(reader->sampleRate * maxScanSec));
        const int channels = static_cast<int>(std::max(reader->numChannels, 1u));
        juce::AudioBuffer<float> buffer(channels, blockSize);
        juce::int64 audible = -1;
        for (juce::int64 pos = 0; pos < maxScan && audible < 0 && isCurrent(request.generation); pos += blockSize) {
            const int count = static_cast<int>(std::min<juce::int64>(blockSize, maxScan - pos));
            buffer.clear();
            reader->read(&buffer, 0, count, pos, true, true);
            for (int i = 0; i < count && audible < 0; ++i)
                for (int channel = 0; channel < channels; ++channel)
                    if (std::abs(buffer.getSample(channel, i)) >= silenceThreshold) {
                        audible = pos + i;
                        break;
                    }
        }
        if (audible > 0) result.autoCueSec = static_cast<double>(audible) / reader->sampleRate;
    }
    if (!isCurrent(request.generation))
        return fail(TrackLoadError::Superseded, QStringLiteral("Load was superseded"));
    return result;
}
