#include "stems/StemSeparationService.h"

#include "stems/StemChunkPlan.h"
#include "stems/Utf8Path.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>

namespace brockdj::stems {
namespace {

constexpr char kCacheMagic[4] = { 'B', 'S', 'T', 'M' };
constexpr const char* kCacheExtension = ".bstem";
// Chunks closer than this ahead of the playhead are "urgent": the DJ may hear
// them soon, so they beat background work of other decks.
constexpr double kUrgentLookaheadSeconds = 24.0;

// Model stem index for each stored stem.
constexpr std::array<int, kStoredStemCount> kModelIndexForStored {
    StemInference::Vocals, StemInference::Bass, StemInference::Drums
};

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t seed) noexcept
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t fnv1a(const std::string& text, std::uint64_t seed) noexcept
{
    return fnv1a(text.data(), text.size(), seed);
}

void accumulateEnvelopeSample(StemTrackData& data, std::int64_t sample, float mixLeft, float mixRight)
{
    std::array<float, kStemCount> squares {};
    const float mix[2] = { mixLeft, mixRight };
    for (int ch = 0; ch < kStemChannels; ++ch) {
        const float vocals = data.stored(StoredStem::Vocals, ch, sample);
        const float bass = data.stored(StoredStem::Bass, ch, sample);
        const float drums = data.stored(StoredStem::Drums, ch, sample);
        const float melody = mix[ch] - vocals - bass - drums;
        squares[static_cast<int>(Stem::Vocals)] += 0.5f * vocals * vocals;
        squares[static_cast<int>(Stem::Melody)] += 0.5f * melody * melody;
        squares[static_cast<int>(Stem::Bass)] += 0.5f * bass * bass;
        squares[static_cast<int>(Stem::Drums)] += 0.5f * drums * drums;
    }
    data.accumulateEnvelope(sample, squares);
}

// Four-point Hermite read with edge clamping.
float hermiteAt(const float* data, std::int64_t size, double position) noexcept
{
    const auto base = static_cast<std::int64_t>(std::floor(position));
    const auto frac = static_cast<float>(position - static_cast<double>(base));
    const auto at = [data, size](std::int64_t i) {
        return data[std::clamp<std::int64_t>(i, 0, size - 1)];
    };
    const float y0 = at(base - 1), y1 = at(base), y2 = at(base + 1), y3 = at(base + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + y1;
}

} // namespace

struct StemSeparationService::Job {
    int deck = -1;
    std::uint64_t serial = 0;
    StemTrackRequest request;
    std::shared_ptr<StemJobStatus> status;
    std::shared_ptr<StemMixState> mix;
    bool cancelled = false; // guarded by m_mutex
    bool background = false;

    // Worker-only state.
    bool started = false;
    bool finished = false;
    std::shared_ptr<StemTrackData> data;
    std::unique_ptr<juce::AudioFormatReader> reader;
    StemChunkPlan plan;
    std::vector<bool> done;
    // Pending overlap halves keyed by the pair's later chunk index; layout
    // [storedStem][channel][overlapLength].
    std::map<int, std::vector<float>> tails; // from chunk i-1, waiting for chunk i
    std::map<int, std::vector<float>> heads; // from chunk i, waiting for chunk i-1
    std::vector<float> input;  // [2][kWindow] at model rate
    std::vector<float> output; // [4][2][kWindow] at model rate
    std::vector<float> source; // [2][window + guard] at track rate
};

StemSeparationService::StemSeparationService(Config config, StemInferenceFactory factory)
    : m_config(std::move(config))
    , m_factory(std::move(factory))
{
    m_worker = std::thread([this] { workerLoop(); });
}

StemSeparationService::~StemSeparationService() { shutdown(); }

void StemSeparationService::submit(int deck, StemTrackRequest request, std::shared_ptr<StemJobStatus> status,
                                   std::shared_ptr<StemMixState> mix, bool background)
{
    if (!status || !mix)
        return;
    auto job = std::make_shared<Job>();
    job->deck = deck;
    job->request = std::move(request);
    job->status = std::move(status);
    job->mix = std::move(mix);
    job->background = background;
    job->status->state.store(StemJobState::Queued);
    job->status->progress.store(0.0f);
    job->status->fromCache.store(false);
    job->status->cacheStored.store(false);
    cancel(deck);
    {
        std::lock_guard lock(m_mutex);
        if (m_shutdown)
            return;
        job->serial = ++m_serial;
        m_jobs.push_back(std::move(job));
    }
    m_wake.notify_all();
}

void StemSeparationService::submitPreanalysis(const std::string& filePath,
                                             std::shared_ptr<StemJobStatus> status)
{
    submit(-1, { filePath, 1, 0.0, 0 }, std::move(status),
           std::make_shared<StemMixState>(), true);
}

void StemSeparationService::cancelPreanalysis() { cancel(-1); }

void StemSeparationService::cancel(int deck)
{
    std::lock_guard lock(m_mutex);
    for (auto& job : m_jobs) {
        if (job->deck != deck || job->cancelled)
            continue;
        job->cancelled = true;
        if (job == m_running && m_inference)
            m_inference->abort();
    }
    std::erase_if(m_jobs, [this](const auto& job) { return job->cancelled && job != m_running; });
}

void StemSeparationService::preload()
{
    {
        std::lock_guard lock(m_mutex);
        m_preloadRequested = true;
    }
    m_wake.notify_all();
}

void StemSeparationService::resetInference()
{
    {
        std::lock_guard lock(m_mutex);
        m_resetRequested = true;
        m_lastError.clear();
    }
    m_wake.notify_all();
}

void StemSeparationService::shutdown()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_shutdown && !m_worker.joinable())
            return;
        m_shutdown = true;
        for (auto& job : m_jobs)
            job->cancelled = true;
        if (m_inference)
            m_inference->abort();
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    std::lock_guard lock(m_mutex);
    m_jobs.clear();
    m_inference.reset();
    m_inferenceLoaded.store(false);
}

std::string StemSeparationService::lastError() const
{
    std::lock_guard lock(m_mutex);
    return m_lastError;
}

std::string StemSeparationService::inferenceBackend() const
{
    std::lock_guard lock(m_mutex);
    return m_inferenceBackend;
}

std::string StemSeparationService::acceleratorWarning() const
{
    std::lock_guard lock(m_mutex);
    return m_acceleratorWarning;
}

std::filesystem::path StemSeparationService::cacheFileFor(const std::string& filePath) const
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(pathFromUtf8(filePath), ec);
    if (ec)
        canonical = pathFromUtf8(filePath);
    std::string key = pathToUtf8(canonical);
    const auto size = std::filesystem::file_size(canonical, ec);
    key += '|' + std::to_string(ec ? 0 : size);
    const auto mtime = std::filesystem::last_write_time(canonical, ec);
    // libc++ uses a 128-bit file-clock representation; to_string has no such overload.
    key += '|' + std::to_string(ec ? 0 : static_cast<std::int64_t>(mtime.time_since_epoch().count()));
    // Envelope-format upgrades do not invalidate the separated audio identity.
    key += "|v2";
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx%016llx",
                  static_cast<unsigned long long>(fnv1a(key, 14695981039346656037ull)),
                  static_cast<unsigned long long>(fnv1a(key, 0x9e3779b97f4a7c15ull)));
    return m_config.cacheDir / (std::string(name) + kCacheExtension);
}

// ---------------------------------------------------------------------------
// Worker

void StemSeparationService::workerLoop()
{
    for (;;) {
        std::shared_ptr<Job> job;
        bool preload = false;
        bool reset = false;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_shutdown || m_preloadRequested || m_resetRequested
                    || std::any_of(m_jobs.begin(), m_jobs.end(),
                                   [](const auto& j) { return !j->cancelled && !j->finished; });
            });
            if (m_shutdown)
                break;
            reset = std::exchange(m_resetRequested, false);
            preload = std::exchange(m_preloadRequested, false);
        }
        if (reset) {
            std::unique_ptr<StemInference> old;
            {
                std::lock_guard lock(m_mutex);
                old = std::move(m_inference);
            }
            m_inferenceLoaded.store(false);
        }
        if (preload)
            ensureInference();

        {
            std::lock_guard lock(m_mutex);
            job = pickJob();
            m_running = job;
        }
        if (job) {
            try {
                runStep(*job);
            } catch (const std::bad_alloc&) {
                failJob(*job, "Not enough memory to separate this track");
            } catch (const std::length_error&) {
                failJob(*job, "Track is too large to separate");
            }
        }
        {
            std::lock_guard lock(m_mutex);
            m_running.reset();
            if (m_inference) {
                m_inferenceBackend = m_inference->backend();
                m_acceleratorWarning = m_inference->acceleratorWarning();
            }
            std::erase_if(m_jobs, [](const auto& j) { return j->cancelled || j->finished; });
        }
    }
}

std::shared_ptr<StemSeparationService::Job> StemSeparationService::pickJob()
{
    std::shared_ptr<Job> best;
    int bestRank = -2;
    for (const auto& job : m_jobs) {
        if (job->cancelled || job->finished)
            continue;
        const bool playing = job->status->playing.load(std::memory_order_relaxed);
        bool urgent = !job->started;
        if (job->started) {
            const auto playhead = std::clamp<std::int64_t>(
                job->status->playheadSample.load(std::memory_order_relaxed), 0, job->plan.length - 1);
            if (const auto next = nextStemChunk(job->plan, job->done, playhead)) {
                const auto start = job->plan.start(*next);
                const auto end = std::min(job->plan.length, start + job->plan.window);
                const auto ahead = start - playhead;
                urgent = end > playhead
                    && ahead <= static_cast<std::int64_t>(kUrgentLookaheadSeconds * job->request.sampleRate);
            }
        }
        const int rank = job->background ? -1 : (urgent ? 2 : 0) + (playing ? 1 : 0);
        if (rank > bestRank || (rank == bestRank && job->serial > best->serial)) {
            best = job;
            bestRank = rank;
        }
    }
    return best;
}

void StemSeparationService::runStep(Job& job)
{
    if (!job.started) {
        job.started = true;
        if (!startJob(job))
            return;
        if (job.finished)
            return;
    } else if (job.background && loadFromCache(job)) {
        // A live deck may have completed this track while preanalysis yielded.
        publishCachedJob(job);
        return;
    }
    const auto playhead = job.status->playheadSample.load(std::memory_order_relaxed);
    const auto next = nextStemChunk(job.plan, job.done, playhead);
    if (!next) {
        finishJob(job);
        return;
    }
    if (!processChunk(job, *next))
        return;
    job.status->progress.store(static_cast<float>(job.data->readyFraction()), std::memory_order_relaxed);
    if (std::all_of(job.done.begin(), job.done.end(), [](bool d) { return d; }))
        finishJob(job);
}

bool StemSeparationService::startJob(Job& job)
{
    if (job.background) {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        job.reader.reset(formats.createReaderFor(
            juce::File(juce::String::fromUTF8(job.request.filePath.c_str()))));
        if (!job.reader || job.reader->numChannels == 0) {
            failJob(job, "Cannot decode track for stem preanalysis");
            return false;
        }
        job.request.sampleRate = job.reader->sampleRate;
        job.request.lengthSamples = job.reader->lengthInSamples;
    }
    const auto& request = job.request;
    if (!std::isfinite(request.sampleRate) || request.sampleRate <= 0.0
        || request.lengthSamples <= 0
        || static_cast<std::uint64_t>(request.lengthSamples)
            > std::numeric_limits<std::size_t>::max() / (kStoredStemCount * kStemChannels * sizeof(std::int16_t))) {
        failJob(job, "invalid track");
        return false;
    }

    if (loadFromCache(job)) {
        if (!job.data->envelopeComplete()) {
            if (!buildCachedEnvelope(job))
                return false;
            const bool stored = writeCache(job);
            if (job.background && !stored) {
                failJob(job, "Cannot save the prepared stem waveform cache.");
                return false;
            }
        }
        return publishCachedJob(job);
    }

    job.status->state.store(StemJobState::Separating);
    if (!ensureInference()) {
        failJob(job, lastError());
        return false;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    if (!job.reader)
        job.reader.reset(formats.createReaderFor(juce::File(juce::String::fromUTF8(request.filePath.c_str()))));
    if (!job.reader || job.reader->numChannels == 0) {
        failJob(job, "cannot decode track");
        return false;
    }
    if (std::abs(job.reader->sampleRate - request.sampleRate) > 0.5
        || job.reader->lengthInSamples != request.lengthSamples) {
        failJob(job, "track changed on disk");
        return false;
    }

    job.plan = makeStemChunkPlan(request.lengthSamples, request.sampleRate,
                                 StemInference::kWindow, StemInference::kSampleRate);
    if (job.plan.window > std::numeric_limits<int>::max() - 8 || job.plan.count() <= 0) {
        failJob(job, "Track sample rate or length is too large to separate");
        return false;
    }
    job.done.assign(static_cast<std::size_t>(job.plan.count()), false);
    job.data = std::make_shared<StemTrackData>(request.trackId, request.sampleRate, request.lengthSamples);
    job.input.assign(static_cast<std::size_t>(2 * StemInference::kWindow), 0.0f);
    job.output.assign(static_cast<std::size_t>(StemInference::kModelStems * 2 * StemInference::kWindow), 0.0f);

    std::lock_guard lock(m_mutex);
    if (job.cancelled)
        return false;
    job.mix->publish(job.data);
    return true;
}

bool StemSeparationService::processChunk(Job& job, int chunk)
{
    const auto& plan = job.plan;
    const double ratio = job.request.sampleRate / StemInference::kSampleRate;
    const bool resample = std::abs(ratio - 1.0) > 1e-9;
    const auto start = plan.start(chunk);
    const auto windowEnd = std::min(plan.length, start + plan.window);
    const auto window = windowEnd - start;
    constexpr std::int64_t kGuard = 8;
    const auto readLength = std::min(plan.length - start, plan.window + kGuard);
    const auto sourceSize = plan.window + kGuard;

    // Decode the window (zero padded past the end of the track).
    job.source.assign(static_cast<std::size_t>(2 * sourceSize), 0.0f);
    {
        juce::AudioBuffer<float> buffer(2, static_cast<int>(readLength));
        buffer.clear();
        if (!job.reader->read(&buffer, 0, static_cast<int>(readLength), start, true, true)) {
            failJob(job, "Cannot decode audio for stem separation");
            return false;
        }
        if (job.reader->numChannels == 1)
            buffer.copyFrom(1, 0, buffer, 0, 0, static_cast<int>(readLength));
        for (int ch = 0; ch < 2; ++ch)
            std::memcpy(job.source.data() + ch * sourceSize, buffer.getReadPointer(ch),
                        static_cast<std::size_t>(readLength) * sizeof(float));
    }

    // Track rate -> model rate.
    std::fill(job.input.begin(), job.input.end(), 0.0f);
    for (int ch = 0; ch < 2; ++ch) {
        const float* src = job.source.data() + ch * sourceSize;
        float* dst = job.input.data() + ch * StemInference::kWindow;
        if (!resample) {
            std::memcpy(dst, src, static_cast<std::size_t>(std::min<std::int64_t>(sourceSize, StemInference::kWindow))
                                      * sizeof(float));
        } else {
            for (std::int64_t i = 0; i < StemInference::kWindow; ++i)
                dst[i] = hermiteAt(src, sourceSize, static_cast<double>(i) * ratio);
        }
    }

    std::string error;
    {
        std::lock_guard lock(m_mutex);
        if (job.cancelled || m_shutdown)
            return false;
    }
    const bool ok = m_inference->separate(job.input.data(), job.input.data() + StemInference::kWindow,
                                          job.output.data(), error);
    {
        std::lock_guard lock(m_mutex);
        if (job.cancelled || m_shutdown)
            return false;
    }
    if (!ok) {
        // A stale abort aimed at a previously cancelled job: just retry.
        if (error == "aborted")
            return false;
        failJob(job, error);
        return false;
    }

    // Model rate -> track rate, per stored stem: [stored][ch][window].
    std::vector<float> stems(static_cast<std::size_t>(kStoredStemCount * 2 * window));
    const auto at = [&](int stored, int ch, std::int64_t k) -> float& {
        return stems[static_cast<std::size_t>((stored * 2 + ch) * window + k)];
    };
    for (int stored = 0; stored < kStoredStemCount; ++stored) {
        for (int ch = 0; ch < 2; ++ch) {
            const float* src = job.output.data()
                + (static_cast<std::int64_t>(kModelIndexForStored[static_cast<std::size_t>(stored)]) * 2 + ch)
                    * StemInference::kWindow;
            for (std::int64_t k = 0; k < window; ++k)
                at(stored, ch, k) = resample ? hermiteAt(src, StemInference::kWindow, static_cast<double>(k) / ratio)
                                             : src[k];
        }
    }

    auto& data = *job.data;
    // Stems are final here, and the window's mix is still in job.source, so the
    // waveform envelope (including melody = mix − stored stems) costs no decode.
    const auto accumulateEnvelope = [&](std::int64_t first, std::int64_t end) {
        for (auto s = first; s < end; ++s)
            accumulateEnvelopeSample(data, s, job.source[static_cast<std::size_t>(s - start)],
                                     job.source[static_cast<std::size_t>(sourceSize + (s - start))]);
    };
    const auto writeRange = [&](std::int64_t first, std::int64_t end) {
        for (int stored = 0; stored < kStoredStemCount; ++stored)
            for (int ch = 0; ch < 2; ++ch)
                for (auto s = first; s < end; ++s)
                    data.write(static_cast<StoredStem>(stored), ch, s, at(stored, ch, s - start));
        accumulateEnvelope(first, end);
        data.markFinal(first, end);
    };
    const auto copyEdge = [&](std::int64_t first, std::int64_t end) {
        const auto n = end - first;
        std::vector<float> edge(static_cast<std::size_t>(kStoredStemCount * 2 * n));
        for (int stored = 0; stored < kStoredStemCount; ++stored)
            for (int ch = 0; ch < 2; ++ch)
                for (std::int64_t k = 0; k < n; ++k)
                    edge[static_cast<std::size_t>((stored * 2 + ch) * n + k)] = at(stored, ch, first - start + k);
        return edge;
    };
    // earlier = chunk pair's earlier half, later = later half; both laid out
    // [stored][ch][n] for the overlap starting at `first`.
    const auto blend = [&](std::int64_t first, std::int64_t n, const std::vector<float>& earlier,
                           const std::vector<float>& later) {
        for (int stored = 0; stored < kStoredStemCount; ++stored)
            for (int ch = 0; ch < 2; ++ch)
                for (std::int64_t k = 0; k < n; ++k) {
                    const auto idx = static_cast<std::size_t>((stored * 2 + ch) * n + k);
                    const float w = plan.incomingWeight(k);
                    data.write(static_cast<StoredStem>(stored), ch, first + k,
                               earlier[idx] * (1.0f - w) + later[idx] * w);
                }
        accumulateEnvelope(first, first + n);
        data.markFinal(first, first + n);
    };

    // Shared head with chunk-1.
    if (chunk > 0) {
        const auto first = plan.start(chunk);
        const auto end = plan.sharedEnd(chunk);
        if (end > first) {
            auto head = copyEdge(first, end);
            if (job.done[static_cast<std::size_t>(chunk - 1)]) {
                auto tail = std::move(job.tails[chunk]);
                job.tails.erase(chunk);
                blend(first, end - first, tail, head);
            } else {
                job.heads[chunk] = std::move(head);
            }
        }
    }
    // Owned body.
    writeRange(plan.sharedEnd(chunk), plan.ownedEnd(chunk));
    // Shared tail with chunk+1.
    if (chunk + 1 < plan.count()) {
        const auto first = plan.start(chunk + 1);
        const auto end = plan.sharedEnd(chunk + 1);
        if (end > first) {
            auto tail = copyEdge(first, end);
            if (job.done[static_cast<std::size_t>(chunk + 1)]) {
                auto head = std::move(job.heads[chunk + 1]);
                job.heads.erase(chunk + 1);
                blend(first, end - first, tail, head);
            } else {
                job.tails[chunk + 1] = std::move(tail);
            }
        }
    }
    job.done[static_cast<std::size_t>(chunk)] = true;
    return true;
}

void StemSeparationService::finishJob(Job& job)
{
    job.finished = true;
    job.reader.reset();
    job.input = {};
    job.output = {};
    job.source = {};
    if (!job.data || !job.data->complete() || !job.data->envelopeComplete()) {
        failJob(job, "separation incomplete");
        return;
    }
    job.status->progress.store(1.0f);
    const bool stored = writeCache(job);
    pruneCache();
    std::error_code ec;
    const bool retained = stored && std::filesystem::is_regular_file(cacheFileFor(job.request.filePath), ec);
    job.status->cacheStored.store(retained);
    if (job.background && !retained) {
        failJob(job, "Cannot retain the stem cache for this track. Check free disk space and the cache budget.");
        return;
    }
    job.status->state.store(StemJobState::Ready);
}

void StemSeparationService::failJob(Job& job, const std::string& error)
{
    job.finished = true;
    job.reader.reset();
    const auto message = error.empty() ? std::string("Stem separation failed") : error;
    job.status->setError(message);
    {
        std::lock_guard lock(m_mutex);
        m_lastError = message;
    }
    job.status->state.store(StemJobState::Failed, std::memory_order_release);
}

bool StemSeparationService::ensureInference()
{
    if (m_inference)
        return true;
    std::string error;
    auto created = m_factory ? m_factory(error) : nullptr;
    std::lock_guard lock(m_mutex);
    if (!created) {
        m_lastError = error.empty() ? "stem model unavailable" : error;
        return false;
    }
    m_inference = std::move(created);
    m_inferenceBackend = m_inference->backend();
    m_acceleratorWarning = m_inference->acceleratorWarning();
    m_inferenceLoaded.store(true);
    return true;
}

// ---------------------------------------------------------------------------
// Disk cache: header + raw int16 planar stems + uint16 RMS envelope.

struct CacheHeader {
    char magic[4];
    std::uint32_t version;
    double sampleRate;
    std::int64_t length;
    std::int32_t stems;
    std::int32_t channels;
    std::uint64_t checksum;
};

bool StemSeparationService::loadFromCache(Job& job)
{
    if (m_config.cacheDir.empty())
        return false;
    const auto path = cacheFileFor(job.request.filePath);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const auto discardCorruptCache = [&] {
        in.close();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::cerr << "[stems] Discarding invalid cache file: " << pathToUtf8(path);
        if (ec)
            std::cerr << " (cannot remove: " << ec.message() << ')';
        std::cerr << '\n';
        m_cacheRevision.fetch_add(1);
        return false;
    };
    CacheHeader header {};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || std::memcmp(header.magic, kCacheMagic, 4) != 0
        || (header.version != 2 && header.version != kCacheVersion)
        || !std::isfinite(header.sampleRate) || std::abs(header.sampleRate - job.request.sampleRate) > 0.5
        || header.length != job.request.lengthSamples
        || header.stems != kStoredStemCount || header.channels != kStemChannels)
        return discardCorruptCache();
    auto data = std::make_shared<StemTrackData>(job.request.trackId, job.request.sampleRate, job.request.lengthSamples);
    auto& raw = data->rawSamplesForLoad();
    const auto bytes = raw.size() * sizeof(std::int16_t);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytes));
    auto checksum = fnv1a(raw.data(), bytes, 14695981039346656037ull);
    if (header.version == kCacheVersion) {
        auto& envelope = data->rawEnvelopeForLoad();
        const auto envelopeBytes = envelope.size() * sizeof(std::uint16_t);
        in.read(reinterpret_cast<char*>(envelope.data()), static_cast<std::streamsize>(envelopeBytes));
        checksum = fnv1a(envelope.data(), envelopeBytes, checksum);
    }
    if (!in || in.peek() != std::char_traits<char>::eof() || in.bad()
        || checksum != header.checksum)
        return discardCorruptCache();
    if (header.version == kCacheVersion)
        data->markAllEnvelopeFinal();
    data->markAllFinal();
    job.data = std::move(data);
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
    return true;
}

bool StemSeparationService::publishCachedJob(Job& job)
{
    std::lock_guard lock(m_mutex);
    if (job.cancelled)
        return false;
    job.mix->publish(job.data);
    job.status->fromCache.store(true);
    job.status->cacheStored.store(true);
    job.status->progress.store(1.0f);
    job.status->state.store(StemJobState::Ready);
    job.finished = true;
    return true;
}

bool StemSeparationService::buildCachedEnvelope(Job& job)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(juce::String::fromUTF8(job.request.filePath.c_str()))));
    auto& data = *job.data;
    if (!reader || reader->numChannels == 0 || std::abs(reader->sampleRate - data.sampleRate()) > 0.5
        || reader->lengthInSamples != data.length()) {
        failJob(job, "Cannot decode the source for the cached stem waveform.");
        return false;
    }
    constexpr int kBlock = 1 << 16;
    juce::AudioBuffer<float> buffer(2, kBlock);
    for (std::int64_t first = 0; first < data.length(); first += kBlock) {
        {
            std::lock_guard lock(m_mutex);
            if (job.cancelled || m_shutdown)
                return false;
        }
        const auto count = static_cast<int>(std::min<std::int64_t>(kBlock, data.length() - first));
        buffer.clear();
        if (!reader->read(&buffer, 0, count, first, true, true)) {
            failJob(job, "Cannot read the source for the cached stem waveform.");
            return false;
        }
        if (reader->numChannels == 1)
            buffer.copyFrom(1, 0, buffer, 0, 0, count);
        const float* left = buffer.getReadPointer(0);
        const float* right = buffer.getReadPointer(1);
        for (int i = 0; i < count; ++i)
            accumulateEnvelopeSample(data, first + i, left[i], right[i]);
    }
    return true;
}

bool StemSeparationService::writeCache(const Job& job)
{
    if (m_config.cacheDir.empty() || !job.data || !job.data->envelopeComplete())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(m_config.cacheDir, ec);
    if (ec) {
        std::cerr << "[stems] Cannot create cache directory: " << ec.message() << '\n';
        return false;
    }
    const auto path = cacheFileFor(job.request.filePath);
    auto part = path;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "[stems] Cannot open cache file for writing: " << pathToUtf8(part) << '\n';
            return false;
        }
        CacheHeader header {};
        std::memcpy(header.magic, kCacheMagic, 4);
        header.version = kCacheVersion;
        header.sampleRate = job.request.sampleRate;
        header.length = job.request.lengthSamples;
        header.stems = kStoredStemCount;
        header.channels = kStemChannels;
        const auto& raw = job.data->rawSamples();
        const auto& envelope = job.data->rawEnvelope();
        header.checksum = fnv1a(raw.data(), raw.size() * sizeof(std::int16_t), 14695981039346656037ull);
        header.checksum = fnv1a(envelope.data(), envelope.size() * sizeof(std::uint16_t), header.checksum);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(raw.data()),
                  static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
        out.write(reinterpret_cast<const char*>(envelope.data()),
                  static_cast<std::streamsize>(envelope.size() * sizeof(std::uint16_t)));
        out.close();
        if (!out) {
            std::filesystem::remove(part, ec);
            std::cerr << "[stems] Cannot write cache file: " << pathToUtf8(part) << '\n';
            return false;
        }
    }
    std::filesystem::rename(part, path, ec);
    if (ec) {
        std::cerr << "[stems] Cannot install cache file: " << ec.message() << '\n';
        std::filesystem::remove(part, ec);
        return false;
    }
    m_cacheRevision.fetch_add(1);
    return true;
}

std::vector<std::string> StemSeparationService::cachedFileNames() const
{
    std::vector<std::string> files;
    std::error_code ec;
    std::filesystem::directory_iterator it(m_config.cacheDir, ec), end;
    if (ec) {
        if (ec != std::errc::no_such_file_or_directory)
            std::cerr << "[stems] Cannot inspect cache directory: " << ec.message() << '\n';
        return files;
    }
    while (it != end) {
        const auto path = it->path();
        if (path.extension() == kCacheExtension && it->is_regular_file(ec)) {
            std::ifstream input(path, std::ios::binary);
            CacheHeader header {};
            input.read(reinterpret_cast<char*>(&header), sizeof(header));
            const auto size = it->file_size(ec);
            if (input && !ec && std::memcmp(header.magic, kCacheMagic, 4) == 0
                && (header.version == 2 || header.version == kCacheVersion) && std::isfinite(header.sampleRate)
                && header.sampleRate > 0.0 && header.length > 0
                && header.stems == kStoredStemCount && header.channels == kStemChannels
                && static_cast<std::uint64_t>(header.length)
                    <= (std::numeric_limits<std::uint64_t>::max() - sizeof(header)) / 20
                && size == sizeof(header) + static_cast<std::uint64_t>(header.length) * 12
                    + (header.version == kCacheVersion
                        ? ((static_cast<std::uint64_t>(header.length) + kEnvelopeBinSamples - 1)
                            / kEnvelopeBinSamples) * kStemCount * sizeof(std::uint16_t) : 0))
                files.push_back(pathToUtf8(path.filename()));
        }
        it.increment(ec);
        if (ec) {
            std::cerr << "[stems] Cannot inspect cache entries: " << ec.message() << '\n';
            break;
        }
    }
    return files;
}

void StemSeparationService::pruneCache()
{
    if (m_config.cacheDir.empty())
        return;
    struct Entry {
        std::filesystem::path path;
        std::uintmax_t size;
        std::filesystem::file_time_type time;
    };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(m_config.cacheDir, ec)) {
        if (!item.is_regular_file(ec) || item.path().extension() != kCacheExtension)
            continue;
        const auto size = item.file_size(ec);
        if (ec)
            continue;
        entries.push_back({ item.path(), size, item.last_write_time(ec) });
        total += size;
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    for (const auto& entry : entries) {
        if (total <= m_config.cacheBudgetBytes)
            break;
        if (std::filesystem::remove(entry.path, ec)) {
            total -= entry.size;
            m_cacheRevision.fetch_add(1);
        } else if (ec)
            std::cerr << "[stems] Cannot remove old cache file: " << ec.message() << '\n';
    }
}

} // namespace brockdj::stems
