#include "stems/StemChunkPlan.h"
#include "stems/StemMixState.h"
#include "stems/StemSeparationService.h"
#include "stems/StemWaveformOverlay.h"
#include "stems/Utf8Path.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#endif

using namespace brockdj::stems;

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

// Linear fake model: each stem is a fixed fraction of the mix, so separated
// output is position invariant and window blending must reproduce it exactly.
constexpr float kDrums = 0.1f, kBass = 0.2f, kOther = 0.3f, kVocals = 0.4f;

class FakeInference final : public StemInference
{
public:
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<int> calls { 0 };
        int allowed = 1 << 30;
        bool aborted = false;
        float lastInputTail = 0.0f;
    };
    explicit FakeInference(std::shared_ptr<Shared> shared) : m_shared(std::move(shared)) {}

    bool separate(const float* left, const float* right, float* output, std::string& error) override
    {
        {
            std::unique_lock lock(m_shared->mutex);
            m_shared->cv.wait(lock, [this] { return m_shared->calls < m_shared->allowed || m_shared->aborted; });
            if (m_shared->aborted) {
                m_shared->aborted = false;
                error = "aborted";
                return false;
            }
            ++m_shared->calls;
            m_shared->lastInputTail = left[kWindow - 1];
        }
        const float fractions[kModelStems] = { kDrums, kBass, kOther, kVocals };
        for (int stem = 0; stem < kModelStems; ++stem)
            for (int ch = 0; ch < 2; ++ch) {
                const float* in = ch == 0 ? left : right;
                float* out = output + (static_cast<std::int64_t>(stem) * 2 + ch) * kWindow;
                for (std::int64_t i = 0; i < kWindow; ++i)
                    out[i] = in[i] * fractions[stem];
            }
        m_shared->cv.notify_all();
        return true;
    }
    void abort() noexcept override
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->aborted = true;
        m_shared->cv.notify_all();
    }

private:
    std::shared_ptr<Shared> m_shared;
};

float signalAt(std::int64_t i, int ch, double rate)
{
    const double t = static_cast<double>(i) / rate;
    return static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * (ch == 0 ? 110.0 : 220.0) * t));
}

std::string utf8Path(const std::filesystem::path& path)
{
    return pathToUtf8(path);
}

std::filesystem::path writeWav(const std::filesystem::path& dir, const char* name, double rate, std::int64_t length,
                               int channels = 2)
{
    const auto path = dir / pathFromUtf8(name);
    juce::File file(juce::String::fromUTF8(utf8Path(path).c_str()));
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                  .withSampleRate(rate)
                                                  .withNumChannels(channels)
                                                  .withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    juce::AudioBuffer<float> buffer(channels, static_cast<int>(length));
    for (int ch = 0; ch < channels; ++ch)
        for (std::int64_t i = 0; i < length; ++i)
            buffer.setSample(ch, static_cast<int>(i), signalAt(i, ch, rate));
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    writer.reset();
    return path;
}

template <typename Predicate>
bool waitFor(Predicate predicate, double seconds = 20.0)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

bool testChunkPlan()
{
    bool ok = true;
    const auto plan = makeStemChunkPlan(44100 * 60, 44100.0);
    ok &= require(plan.window == 343976 && plan.overlap == plan.window / 8, "44.1 kHz window geometry");
    ok &= require(plan.count() >= 8, "a minute needs several windows");
    // Owned ranges tile the track exactly.
    std::int64_t covered = 0;
    for (int i = 0; i < plan.count(); ++i) {
        ok &= require(i == 0 ? plan.start(i) == 0 : plan.ownedEnd(i - 1) == plan.start(i), "owned ranges are contiguous");
        ok &= require(plan.start(i) + plan.window >= plan.ownedEnd(i), "window covers its owned range");
        covered += plan.ownedEnd(i) - plan.start(i);
    }
    ok &= require(covered == plan.length, "owned ranges cover the track");
    const int last = plan.count() - 1;
    ok &= require(plan.start(last) + plan.window >= plan.length, "last window reaches the end");
    ok &= require(plan.chunkForSample(plan.start(3) + 5) == 3, "sample maps to its window");

    const auto plan48 = makeStemChunkPlan(48000 * 10, 48000.0);
    ok &= require(plan48.window > 343980 && plan48.window < 374400, "48 kHz window is scaled");
    const auto shortPlan = makeStemChunkPlan(1000, 44100.0);
    ok &= require(shortPlan.count() == 1 && shortPlan.ownedEnd(0) == 1000, "short track is one window");
    const StemChunkPlan excessive {std::numeric_limits<std::int64_t>::max(), 8, 1};
    ok &= require(excessive.count() == 0, "unrepresentable window counts are rejected");
    ok &= require(makeStemChunkPlan(1000, std::numeric_limits<double>::max()).count() == 0,
                  "unrepresentable sample rates are rejected");
    ok &= require(plan.chunkForSample(std::numeric_limits<std::int64_t>::max()) == plan.count() - 1,
                  "extreme playhead positions clamp without integer overflow");

    std::vector<bool> done { true, false, true, false, false };
    ok &= require(nextStemChunk(done, 2) == 3, "next window is ahead of the playhead");
    done[3] = done[4] = true;
    ok &= require(nextStemChunk(done, 3) == 1, "falls back behind the playhead");
    done[1] = true;
    ok &= require(!nextStemChunk(done, 0), "nothing left");

    done.assign(static_cast<std::size_t>(plan.count()), false);
    const auto overlapPlayhead = plan.start(3) + plan.overlap / 2;
    ok &= require(nextStemChunk(plan, done, overlapPlayhead) == 3,
                  "separation starts in the playhead window");
    done[3] = true;
    ok &= require(nextStemChunk(plan, done, overlapPlayhead) == 2,
                  "an unfinished playhead overlap prioritizes the preceding window");
    done[2] = true;
    ok &= require(nextStemChunk(plan, done, overlapPlayhead) == 4,
                  "separation continues ahead once the overlap is complete");
    return ok;
}

bool testRemixAndReadScope()
{
    bool ok = true;
    const std::array<float, kStemCount> unity { 1, 1, 1, 1 };
    ok &= require(remixSample(0.7f, 0.1f, 0.2f, 0.3f, unity) == 0.7f, "unity gains are bit exact");
    // vocals muted -> mix - vocals
    const std::array<float, kStemCount> noVocals { 0, 1, 1, 1 };
    ok &= require(std::abs(remixSample(0.7f, 0.1f, 0.2f, 0.3f, noVocals) - 0.6f) < 1e-6f, "vocal mute subtracts vocals");
    const std::array<float, kStemCount> onlyDrums { 0, 0, 0, 1 };
    ok &= require(std::abs(remixSample(0.7f, 0.1f, 0.2f, 0.3f, onlyDrums) - 0.3f) < 1e-6f, "solo drums");
    const std::array<float, kStemCount> onlyMelody { 0, 1, 0, 0 };
    ok &= require(std::abs(remixSample(0.7f, 0.1f, 0.2f, 0.3f, onlyMelody) - 0.1f) < 1e-6f, "melody is the remainder");

    auto data = std::make_shared<StemTrackData>(7, 44100.0, 10000);
    for (std::int64_t i = 0; i < 10000; ++i)
        data->write(StoredStem::Vocals, 0, i, 0.25f);
    data->markFinal(0, kReadyBlockSamples);
    ok &= require(data->isReady(10) && !data->isReady(kReadyBlockSamples), "readiness is per block");
    ok &= require(!data->complete(), "partial is not complete");
    data->markFinal(kReadyBlockSamples, 10000);
    ok &= require(data->complete() && data->isReady(9999), "complete after all ranges");

    StemMixState state;
    StemGainSmoother smoother;
    smoother.prepare(44100.0);
    state.publish(data);
    {
        StemReadScope scope(&state, smoother, 7);
        ok &= require(!scope.active(), "unity gains keep the scope inactive");
        ok &= require(scope.apply(5, 0, 0.5f) == 0.5f, "inactive scope passes the mix");
    }
    state.setGain(Stem::Vocals, 0.0f);
    {
        StemReadScope scope(&state, smoother, 7);
        ok &= require(scope.active(), "muted stem activates the scope");
        scope.snapToTargets();
        ok &= require(std::abs(scope.apply(5, 0, 0.5f) - 0.25f) < 1e-4f, "vocal mute removes the stored vocal");
    }
    {
        StemReadScope scope(&state, smoother, 8);
        ok &= require(!scope.active() && scope.apply(5, 0, 0.5f) == 0.5f, "foreign track id is ignored");
    }
    // Smoother ramps instead of jumping.
    StemGainSmoother ramp;
    ramp.prepare(44100.0);
    state.resetGains();
    state.setGain(Stem::Drums, 0.0f);
    {
        StemReadScope scope(&state, ramp, 7);
        scope.advance(5);
        ok &= require(ramp.gains[3] < 1.0f && ramp.gains[3] > 0.99f, "gain ramps over several samples");
    }

    auto partial = std::make_shared<StemTrackData>(9, 44100.0, 3 * kReadyBlockSamples);
    for (std::int64_t i = 0; i < partial->length(); ++i)
        partial->write(StoredStem::Vocals, 0, i, 0.25f);
    partial->markFinal(0, kReadyBlockSamples);
    state.publish(partial);
    state.resetGains();
    state.setGain(Stem::Vocals, 0.0f);
    StemGainSmoother availability;
    availability.prepare(44100.0);
    {
        StemReadScope scope(&state, availability, 9);
        scope.snapToTargets();
        float previous = scope.apply(kReadyBlockSamples - 800, 0, 0.5f);
        float largestStep = 0.0f;
        for (auto sample = kReadyBlockSamples - 799; sample < kReadyBlockSamples + 800; ++sample) {
            scope.advance(sample);
            const float current = scope.apply(sample, 0, 0.5f);
            largestStep = std::max(largestStep, std::abs(current - previous));
            previous = current;
        }
        ok &= require(largestStep < 0.001f && previous == 0.5f,
                      "ready-to-unready boundaries fade smoothly to the original mix");

        const auto interior = kReadyBlockSamples + 1900;
        scope.advance(interior);
        previous = scope.apply(interior, 0, 0.5f);
        partial->markFinal(kReadyBlockSamples, 2 * kReadyBlockSamples);
        largestStep = 0.0f;
        for (int i = 0; i < 800; ++i) {
            scope.advance(interior);
            const float current = scope.apply(interior, 0, 0.5f);
            largestStep = std::max(largestStep, std::abs(current - previous));
            previous = current;
        }
        ok &= require(largestStep < 0.001f && std::abs(previous - 0.25f) < 1e-4f,
                      "newly separated audio fades in smoothly at the playhead");
    }
    state.publish(nullptr);
    ok &= require(state.current() == nullptr, "publish(nullptr) clears");
    return ok;
}

struct ServiceFixture {
    std::filesystem::path dir;
    std::shared_ptr<FakeInference::Shared> shared = std::make_shared<FakeInference::Shared>();
    std::unique_ptr<StemSeparationService> service;

    ServiceFixture()
    {
        dir = std::filesystem::temp_directory_path()
            / ("brockdj_stem_tests_" + juce::Uuid().toString().toStdString());
        const auto cacheDir = dir / pathFromUtf8("cache-\xC3\xA4");
        std::filesystem::create_directories(cacheDir);
        StemSeparationService::Config config;
        config.cacheDir = cacheDir;
        auto shared = this->shared;
        service = std::make_unique<StemSeparationService>(config, [shared](std::string&) {
            return std::make_unique<FakeInference>(shared);
        });
    }
    ~ServiceFixture()
    {
        service->shutdown();
        std::filesystem::remove_all(dir);
    }
};

bool testCacheIdentity()
{
    ServiceFixture fx;
    const auto file = writeWav(fx.dir, "identity.wav", 44100.0, 1000);
    const auto originalTime = std::filesystem::last_write_time(file);
    const auto originalKey = fx.service->cacheFileFor(utf8Path(file));
    bool ok = require(originalKey == fx.service->cacheFileFor(utf8Path(file)),
                      "unchanged source retains its cache key");
    std::filesystem::last_write_time(file, originalTime + std::chrono::seconds(1));
    ok &= require(originalKey != fx.service->cacheFileFor(utf8Path(file)),
                  "source timestamp changes invalidate the cache key");
    std::filesystem::last_write_time(file, originalTime);
    ok &= require(originalKey == fx.service->cacheFileFor(utf8Path(file)),
                  "restoring the timestamp restores the cache key");
    {
        std::ofstream append(file, std::ios::binary | std::ios::app);
        append.put('\0');
    }
    std::filesystem::last_write_time(file, originalTime);
    ok &= require(originalKey != fx.service->cacheFileFor(utf8Path(file)),
                  "source size changes invalidate the cache key even with the same timestamp");
    return ok;
}

bool checkStoredMatches(const StemTrackData& data, double rate, float tolerance)
{
    float worst = 0.0f;
    float edgeError = 0.0f;
    for (std::int64_t i = 0; i < data.length(); ++i)
        for (int ch = 0; ch < 2; ++ch) {
            const float mix = signalAt(i, ch, rate);
            auto& error = i >= data.length() - 8 ? edgeError : worst;
            error = std::max(error, std::abs(data.stored(StoredStem::Vocals, ch, i) - kVocals * mix));
            error = std::max(error, std::abs(data.stored(StoredStem::Bass, ch, i) - kBass * mix));
            error = std::max(error, std::abs(data.stored(StoredStem::Drums, ch, i) - kDrums * mix));
        }
    if (worst > tolerance)
        std::cerr << "  worst stem error " << worst << '\n';
    // Resampling a signal abruptly truncated at EOF is necessarily affected by
    // the zero padding around that boundary.
    return worst <= tolerance && edgeError < 0.02f;
}

// The fake model splits the mix into fixed fractions; per-stem envelope energy
// must follow the squared fractions (Melody = Other in the fake model).
bool envelopeMatchesFakeModel(const StemTrackData& data)
{
    std::array<float, kStemCount> energy {};
    if (!data.envelopeComplete() || !data.envelopeEnergy(0, data.length(), energy))
        return false;
    const std::array<float, kStemCount> expected { kVocals * kVocals, kOther * kOther,
                                                   kBass * kBass, kDrums * kDrums };
    float total = 0.0f, expectedTotal = 0.0f;
    for (std::size_t i = 0; i < kStemCount; ++i) {
        total += energy[i];
        expectedTotal += expected[i];
    }
    if (!(total > 0.0f))
        return false;
    for (std::size_t i = 0; i < kStemCount; ++i)
        if (std::abs(energy[i] / total - expected[i] / expectedTotal) > 0.01f)
            return false;
    return true;
}

bool testServiceSeparatesAndCaches(double rate, float tolerance)
{
    bool ok = true;
    ServiceFixture fx;
    const std::int64_t length = static_cast<std::int64_t>(rate * 25.0) + 123;
    const auto file = writeWav(fx.dir, "track.wav", rate, length);
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    fx.service->submit(0, { utf8Path(file), 42, rate, length }, status, mix);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Ready; }), "service finishes the track");
    auto data = mix->current();
    ok &= require(data && data->complete() && data->trackId() == 42, "published data is complete");
    if (data)
        ok &= require(checkStoredMatches(*data, rate, tolerance), "blended windows reproduce the stems seamlessly");
    if (data)
        ok &= require(waitFor([&] { return data->envelopeComplete(); }) && envelopeMatchesFakeModel(*data),
                      "separation builds the per-stem waveform envelope");
    ok &= require(!status->fromCache.load(), "first run separates");
    ok &= require(waitFor([&] { return std::filesystem::exists(fx.service->cacheFileFor(utf8Path(file))); }),
                  "result is cached on disk");

    const int callsBefore = fx.shared->calls;
    auto status2 = std::make_shared<StemJobStatus>();
    auto mix2 = std::make_shared<StemMixState>();
    fx.service->submit(1, { utf8Path(file), 43, rate, length }, status2, mix2);
    ok &= require(waitFor([&] { return status2->state.load() == StemJobState::Ready; }), "cached track is ready");
    ok &= require(status2->fromCache.load() && fx.shared->calls == callsBefore, "cache hit skips inference");
    auto cached = mix2->current();
    ok &= require(cached && cached->trackId() == 43 && cached->complete(), "cached data carries the new track id");
    if (cached && data)
        ok &= require(cached->rawSamples() == data->rawSamples(), "cache round trip is lossless");
    if (cached)
        ok &= require(data && cached->envelopeComplete() && envelopeMatchesFakeModel(*cached)
                          && cached->rawEnvelope() == data->rawEnvelope(),
                      "a cache hit publishes the saved envelope together with ready audio");
    return ok;
}

bool testCorruptCacheIsRebuilt()
{
    bool ok = true;
    ServiceFixture fx;
    constexpr double rate = 44100.0;
    constexpr std::int64_t length = 44100;
    const auto file = writeWav(fx.dir, "track-\xC3\xA4-\xE6\x97\xA5.wav", rate, length);
    auto first = std::make_shared<StemJobStatus>();
    auto firstMix = std::make_shared<StemMixState>();
    fx.service->submit(0, { utf8Path(file), 1, rate, length }, first, firstMix);
    ok &= require(waitFor([&] { return first->state.load() == StemJobState::Ready; }),
                  "track is separated before cache corruption");
    const auto cache = fx.service->cacheFileFor(utf8Path(file));
    ok &= require(waitFor([&] { return std::filesystem::exists(cache); }), "cache exists before corruption");
    {
        std::fstream bytes(cache, std::ios::in | std::ios::out | std::ios::binary);
        char value = 0;
        bytes.seekg(-1, std::ios::end);
        bytes.read(&value, 1);
        value ^= 0x40;
        bytes.seekp(-1, std::ios::end);
        bytes.write(&value, 1);
        ok &= require(static_cast<bool>(bytes), "cache payload can be corrupted for the test");
    }

    const int callsBefore = fx.shared->calls.load();
    auto second = std::make_shared<StemJobStatus>();
    auto secondMix = std::make_shared<StemMixState>();
    fx.service->submit(1, { utf8Path(file), 2, rate, length }, second, secondMix);
    ok &= require(waitFor([&] { return second->state.load() == StemJobState::Ready; }),
                  "corrupt cache is re-separated");
    ok &= require(!second->fromCache.load() && fx.shared->calls.load() > callsBefore,
                  "corrupt samples are never published from cache");
    ok &= require(firstMix->current() && secondMix->current()
                      && firstMix->current()->rawSamples() == secondMix->current()->rawSamples(),
                  "regenerated stems match the original separation");
    ok &= require(waitFor([&] { return std::filesystem::exists(cache); }),
                  "regenerated cache is installed");

    const int callsAfter = fx.shared->calls.load();
    auto third = std::make_shared<StemJobStatus>();
    auto thirdMix = std::make_shared<StemMixState>();
    fx.service->submit(2, { utf8Path(file), 3, rate, length }, third, thirdMix);
    ok &= require(waitFor([&] { return third->state.load() == StemJobState::Ready; }),
                  "regenerated cache can be loaded");
    ok &= require(third->fromCache.load() && fx.shared->calls.load() == callsAfter,
                  "regenerated cache skips inference");
    return ok;
}

bool testPlayheadPriorityAndCancel()
{
    bool ok = true;
    ServiceFixture fx;
    const double rate = 44100.0;
    const std::int64_t length = static_cast<std::int64_t>(rate * 60.0);
    const auto file = writeWav(fx.dir, "long.wav", rate, length, 1);
    const auto plan = makeStemChunkPlan(length, rate);
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 1;
    }
    auto status = std::make_shared<StemJobStatus>();
    const auto playhead = plan.start(5) + plan.overlap + 2 * kReadyBlockSamples;
    status->playheadSample.store(playhead);
    status->playing.store(true);
    auto mix = std::make_shared<StemMixState>();
    fx.service->submit(0, { utf8Path(file), 9, rate, length }, status, mix);
    ok &= require(waitFor([&] {
                      auto d = mix->current();
                      return d && d->isReady(playhead);
                  }),
                  "window under the playhead is separated first");
    auto data = mix->current();
    ok &= require(data && !data->isReady(0), "start of the track is not done yet");
    ok &= require(status->state.load() == StemJobState::Separating, "job reports separating");
    if (data)
        ok &= require(std::abs(data->stored(StoredStem::Vocals, 1, playhead) - kVocals * signalAt(playhead, 0, rate)) < 1e-3f,
                      "mono source is duplicated to both channels");

    // Cancel while the worker is blocked in the next window.
    fx.service->cancel(0);
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 1 << 30;
        fx.shared->cv.notify_all();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ok &= require(status->state.load() != StemJobState::Ready, "cancelled job never completes");

    // Service keeps working for the next track after a cancel.
    auto status2 = std::make_shared<StemJobStatus>();
    auto mix2 = std::make_shared<StemMixState>();
    const auto file2 = writeWav(fx.dir, "short.wav", rate, 44100 * 3);
    fx.service->submit(0, { utf8Path(file2), 10, rate, 44100 * 3 }, status2, mix2);
    ok &= require(waitFor([&] { return status2->state.load() == StemJobState::Ready; }), "next job runs after cancel");

    // Mismatched metadata fails safely instead of publishing garbage.
    auto status3 = std::make_shared<StemJobStatus>();
    auto mix3 = std::make_shared<StemMixState>();
    fx.service->submit(2, { utf8Path(fx.dir / "missing.wav"), 11, rate, 1000 }, status3, mix3);
    ok &= require(waitFor([&] { return status3->state.load() == StemJobState::Failed; }), "missing file fails");
    ok &= require(mix3->current() == nullptr, "failed job publishes nothing");
    return ok;
}

bool testPlayheadOverlapPriority()
{
    bool ok = true;
    ServiceFixture fx;
    constexpr double rate = 44100.0;
    constexpr std::int64_t length = 44100 * 60;
    const auto file = writeWav(fx.dir, "overlap.wav", rate, length);
    const auto plan = makeStemChunkPlan(length, rate);
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 2;
    }
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    const auto playhead = plan.start(5) + plan.overlap / 2;
    status->playheadSample.store(playhead);
    status->playing.store(true);
    fx.service->submit(0, { utf8Path(file), 9, rate, length }, status, mix);
    ok &= require(waitFor([&] {
                      auto data = mix->current();
                      return data && data->isReady(playhead);
                  }),
                  "a playhead in an overlap becomes ready after the two adjacent windows");
    const auto data = mix->current();
    ok &= require(data && !data->isReady(plan.start(6) + plan.overlap + kReadyBlockSamples),
                  "unrelated future windows do not delay the playhead overlap");
    return ok;
}

bool testCrossDeckUrgency()
{
    bool ok = true;
    for (const bool playheadInOverlap : { false, true }) {
        ServiceFixture fx;
        constexpr double rate = 44100.0;
        constexpr std::int64_t length = 44100 * 20;
        const auto longFile = writeWav(fx.dir, "playing.wav", rate, length);
        const auto shortFile = writeWav(fx.dir, "queued.wav", rate, 44100 * 2);
        const auto plan = makeStemChunkPlan(length, rate);
        const int lastChunk = plan.count() - 1;
        const auto playhead = plan.start(lastChunk)
            + (playheadInOverlap ? plan.overlap / 2 : plan.overlap + 2 * kReadyBlockSamples);
        {
            std::lock_guard lock(fx.shared->mutex);
            fx.shared->allowed = 0;
        }
        auto playing = std::make_shared<StemJobStatus>();
        auto playingMix = std::make_shared<StemMixState>();
        playing->playing.store(true);
        playing->playheadSample.store(playhead);
        fx.service->submit(0, { utf8Path(longFile), 1, rate, length }, playing, playingMix);
        ok &= require(waitFor([&] { return playingMix->current() != nullptr; }),
                      "playing deck begins its playhead window");
        auto queued = std::make_shared<StemJobStatus>();
        auto queuedMix = std::make_shared<StemMixState>();
        fx.service->submit(1, { utf8Path(shortFile), 2, rate, 44100 * 2 }, queued, queuedMix);
        {
            std::lock_guard lock(fx.shared->mutex);
            fx.shared->allowed = 2;
            fx.shared->cv.notify_all();
        }
        if (playheadInOverlap) {
            ok &= require(waitFor([&] {
                              const auto data = playingMix->current();
                              return data && data->isReady(playhead);
                          }, 10.0),
                          "a preceding window needed at the playhead outranks another queued deck");
            ok &= require(queued->state.load() != StemJobState::Ready,
                          "the second inference completed the playhead overlap");
        } else {
            ok &= require(waitFor([&] { return queued->state.load() == StemJobState::Ready; }, 10.0),
                          "old windows behind the playhead do not starve a queued deck");
        }
    }
    return ok;
}

bool testShortResampledTrackUsesSilencePadding()
{
    bool ok = true;
    ServiceFixture fx;
    constexpr double rate = 48000.0;
    constexpr std::int64_t length = 24123;
    const auto file = writeWav(fx.dir, "short-padded.wav", rate, length);
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    fx.service->submit(0, { utf8Path(file), 12, rate, length }, status, mix);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Ready; }),
                  "short 48 kHz track is separated");
    {
        std::lock_guard lock(fx.shared->mutex);
        ok &= require(std::abs(fx.shared->lastInputTail) < 1.0e-7f,
                      "resampling pads the model input with silence beyond the track");
    }
    return ok;
}

bool testCacheBudget()
{
    bool ok = true;
    const auto dir = std::filesystem::temp_directory_path()
        / ("brockdj_stem_budget_" + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(dir);
    const auto file = writeWav(dir, "track.wav", 44100.0, 44100);
    const auto shared = std::make_shared<FakeInference::Shared>();
    StemSeparationService::Config config { dir / "cache", 1 };
    StemSeparationService service(config, [shared](std::string&) {
        return std::make_unique<FakeInference>(shared);
    });
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    service.submit(0, { utf8Path(file), 1, 44100.0, 44100 }, status, mix);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Ready; }),
                  "track exceeding the cache budget is still usable in memory");
    auto preanalysis = std::make_shared<StemJobStatus>();
    service.submitPreanalysis(utf8Path(file), preanalysis);
    ok &= require(waitFor([&] { return preanalysis->state.load() == StemJobState::Failed; }),
                  "preanalysis fails if its result cannot be retained within the cache budget");
    ok &= require(!preanalysis->cacheStored.load() && !preanalysis->error().empty()
                      && service.cachedFileNames().empty(),
                  "pruned preanalysis is not advertised as cached");
    service.shutdown();
    ok &= require(!std::filesystem::exists(service.cacheFileFor(utf8Path(file))),
                  "even a single oversized cache entry is pruned");
    std::filesystem::remove_all(dir);
    return ok;
}

bool testPreanalysisCacheAndMetadata()
{
    bool ok = true;
    ServiceFixture fx;
    constexpr double rate = 48000.0;
    constexpr std::int64_t length = 48000 * 2 + 123;
    const auto file = writeWav(fx.dir, "preanalyse-\xC3\xA4.wav", rate, length);
    auto status = std::make_shared<StemJobStatus>();
    fx.service->submitPreanalysis(utf8Path(file), status);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Ready; }),
                  "background preanalysis decodes its own sample rate and length");
    ok &= require(status->cacheStored.load() && status->progress.load() == 1.0f,
                  "preanalysis is ready only after its cache has been installed");
    const auto cache = fx.service->cacheFileFor(utf8Path(file));
    const auto names = fx.service->cachedFileNames();
    ok &= require(names == std::vector<std::string> { utf8Path(cache.filename()) }
                      && fx.service->cacheRevision() > 0,
                  "cache inventory and revision include preanalysed Unicode tracks");
    {
        std::ofstream junk(cache.parent_path() / "junk.bstem");
        junk << "invalid header";
    }
    ok &= require(fx.service->cachedFileNames() == names, "cache inventory excludes invalid headers");
    const auto calls = fx.shared->calls.load();
    auto deck = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    fx.service->submit(0, { utf8Path(file), 99, rate, length }, deck, mix);
    ok &= require(waitFor([&] { return deck->state.load() == StemJobState::Ready; })
                      && deck->fromCache.load() && fx.shared->calls.load() == calls,
                  "a deck uses preanalysed stems without running inference again");
    const auto data = mix->current();
    ok &= require(data && data->trackId() == 99 && data->sampleRate() == rate
                      && data->length() == length && checkStoredMatches(*data, rate, 2.0e-3f),
                  "preanalysed stems retain the correct audio metadata and samples");
    {
        std::fstream bytes(cache, std::ios::in | std::ios::out | std::ios::binary);
        bytes.seekp(-1, std::ios::end);
        bytes.put('\x7f');
    }
    auto repair = std::make_shared<StemJobStatus>();
    fx.service->submitPreanalysis(utf8Path(file), repair);
    ok &= require(waitFor([&] { return repair->state.load() == StemJobState::Ready; })
                      && !repair->fromCache.load() && fx.shared->calls.load() > calls,
                  "preanalysis verifies cached payloads and repairs corrupt data");
    { std::ofstream extra(cache, std::ios::binary | std::ios::app); extra.put('x'); }
    ok &= require(fx.service->cachedFileNames().empty(),
                  "cache inventory excludes files with trailing payload bytes");
    auto repairLength = std::make_shared<StemJobStatus>();
    fx.service->submitPreanalysis(utf8Path(file), repairLength);
    ok &= require(waitFor([&] { return repairLength->state.load() == StemJobState::Ready; })
                      && !repairLength->fromCache.load()
                      && fx.service->cachedFileNames() == names,
                  "preanalysis repairs invalid cache lengths consistently with the ready badge");
    std::filesystem::remove(cache);
    ok &= require(fx.service->cachedFileNames().empty(), "removed cache files leave the inventory");
    return ok;
}

bool testPreanalysisPriorityAndCancel()
{
    bool ok = true;
    ServiceFixture fx;
    const auto backgroundFile = writeWav(fx.dir, "background.wav", 44100.0, 44100 * 20);
    const auto liveFile = writeWav(fx.dir, "live.wav", 44100.0, 44100 * 2);
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 0;
    }
    auto background = std::make_shared<StemJobStatus>();
    fx.service->submitPreanalysis(utf8Path(backgroundFile), background);
    ok &= require(waitFor([&] { return fx.service->inferenceLoaded(); }),
                  "background window begins before a live deck requests separation");
    auto live = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    fx.service->submit(0, { utf8Path(liveFile), 1, 44100.0, 44100 * 2 }, live, mix);
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 2;
        fx.shared->cv.notify_all();
    }
    ok &= require(waitFor([&] { return live->state.load() == StemJobState::Ready; }, 10.0),
                  "the live deck outranks the background backlog after its current window");
    ok &= require(background->state.load() != StemJobState::Ready,
                  "background track remains incomplete while the live deck becomes ready");
    fx.service->cancelPreanalysis();
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 1 << 30;
        fx.shared->cv.notify_all();
    }
    auto next = std::make_shared<StemJobStatus>();
    fx.service->submit(1, { utf8Path(liveFile), 2, 44100.0, 44100 * 2 }, next,
                       std::make_shared<StemMixState>());
    ok &= require(waitFor([&] { return next->state.load() == StemJobState::Ready; })
                      && next->fromCache.load() && mix->current() && mix->current()->complete(),
                  "stopping preanalysis does not cancel or clear live deck stems");
    ok &= require(background->state.load() != StemJobState::Ready
                      && !std::filesystem::exists(fx.service->cacheFileFor(utf8Path(backgroundFile))),
                  "cancelled partial background data is not saved as a ready cache");
    return ok;
}

bool testPreanalysisWriteFailure()
{
    bool ok = true;
    ServiceFixture fx;
    const auto file = writeWav(fx.dir, "cannot-cache.wav", 44100.0, 44100);
    const auto blockedCache = fx.dir / "not-a-directory";
    { std::ofstream blocked(blockedCache); blocked << "occupied"; }
    StemSeparationService service({ blockedCache }, [](std::string&) {
        return std::make_unique<FakeInference>(std::make_shared<FakeInference::Shared>());
    });
    auto status = std::make_shared<StemJobStatus>();
    service.submitPreanalysis(utf8Path(file), status);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Failed; })
                      && !status->cacheStored.load() && !status->error().empty(),
                  "a preanalysis cache write failure is explicit, not success-shaped");
    auto missing = std::make_shared<StemJobStatus>();
    service.submitPreanalysis(utf8Path(fx.dir / "missing.wav"), missing);
    ok &= require(waitFor([&] { return missing->state.load() == StemJobState::Failed; })
                      && !missing->error().empty(),
                  "missing preanalysis audio fails without inventing metadata");
    service.shutdown();
    return ok;
}

bool testPreanalysisReusesLiveCache()
{
    bool ok = true;
    ServiceFixture fx;
    constexpr std::int64_t length = 44100 * 20;
    const auto file = writeWav(fx.dir, "shared.wav", 44100.0, length);
    const auto plan = makeStemChunkPlan(length, 44100.0);
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = 0;
    }
    auto background = std::make_shared<StemJobStatus>();
    fx.service->submitPreanalysis(utf8Path(file), background);
    ok &= require(waitFor([&] { return fx.service->inferenceLoaded(); }),
                  "shared preanalysis begins its first window");
    auto deck = std::make_shared<StemJobStatus>();
    fx.service->submit(0, { utf8Path(file), 4, 44100.0, length }, deck,
                       std::make_shared<StemMixState>());
    {
        std::lock_guard lock(fx.shared->mutex);
        fx.shared->allowed = plan.count() + 1;
        fx.shared->cv.notify_all();
    }
    ok &= require(waitFor([&] { return deck->state.load() == StemJobState::Ready; }),
                  "loaded deck completes separation before the background backlog");
    ok &= require(waitFor([&] { return background->state.load() == StemJobState::Ready; }, 3.0)
                      && background->fromCache.load() && background->cacheStored.load()
                      && fx.shared->calls.load() == plan.count() + 1,
                  "resuming preanalysis reuses the deck cache instead of repeating remaining inference");
    return ok;
}

bool testMissingModelFails()
{
    bool ok = true;
    const auto dir = std::filesystem::temp_directory_path()
        / ("brockdj_stem_nomodel_" + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(dir);
    const auto file = writeWav(dir, "t.wav", 44100.0, 44100);
    StemSeparationService service({ dir / "cache" }, [](std::string& error) -> std::unique_ptr<StemInference> {
        error = "no model";
        return nullptr;
    });
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    service.submit(0, { utf8Path(file), 1, 44100.0, 44100 }, status, mix);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Failed; }), "missing model fails the job");
    ok &= require(service.lastError() == "no model", "error is reported");
    ok &= require(status->error() == "no model", "the failure is reported on the affected deck's job");
    ok &= require(mix->current() == nullptr, "nothing is published without a model");
    service.shutdown();
    std::filesystem::remove_all(dir);
    return ok;
}

// Opt-in end-to-end check with the real HTDemucs model (BROCKDJ_STEM_MODEL).
bool testRealModel()
{
#if BROCKDJ_HAS_ONNXRUNTIME
    const char* model = std::getenv("BROCKDJ_STEM_MODEL");
    if (!model || !*model)
        return true;
    bool ok = true;
    const auto dir = std::filesystem::temp_directory_path()
        / ("brockdj_stem_real_" + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(dir / "cache");
    const double rate = 48000.0;
    const std::int64_t length = static_cast<std::int64_t>(rate * 30.0);
    const auto file = dir / "bass.wav";
    {
        juce::File out(juce::String::fromUTF8(utf8Path(file).c_str()));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(out);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions {}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(16));
        juce::AudioBuffer<float> buffer(2, static_cast<int>(length));
        for (std::int64_t i = 0; i < length; ++i) {
            const double t = static_cast<double>(i) / rate;
            const float v = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 55.0 * t));
            buffer.setSample(0, static_cast<int>(i), v);
            buffer.setSample(1, static_cast<int>(i), v);
        }
        writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    }
    StemSeparationService::Config config;
    config.cacheDir = dir / "cache";
    const std::string modelPath = model;
    StemInferenceOptions options;
    options.preferGpu = std::getenv("BROCKDJ_STEM_TEST_GPU") != nullptr;
    if (const auto* library = std::getenv("BROCKDJ_STEM_EP_LIBRARY"))
        options.openVinoLibraryPath = library;
    StemSeparationService service(config, [modelPath, options](std::string& error) {
        return createOnnxStemInference(modelPath, std::clamp<int>(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4), error, options);
    });
    const auto loadStart = std::chrono::steady_clock::now();
    service.preload();
    ok &= require(waitFor([&] { return service.inferenceLoaded(); }, 60.0), "real model loads");
    const auto submitted = std::chrono::steady_clock::now();
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    status->playing.store(true);
    service.submit(0, { utf8Path(file), 7, rate, length }, status, mix);
    ok &= require(waitFor([&] { auto d = mix->current(); return d && d->isReady(0); }, 60.0), "first block becomes ready");
    const auto firstReady = std::chrono::steady_clock::now();
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Ready; }, 180.0), "real model separates the track");
    const auto done = std::chrono::steady_clock::now();
    std::cout << "  backend " << service.inferenceBackend() << ", warning " << service.acceleratorWarning() << '\n';
    if (options.preferGpu)
        ok &= require(service.inferenceBackend() != "CPU" || !service.acceleratorWarning().empty(),
                      "GPU requests report the active accelerator or an explicit CPU fallback reason");
    const auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    std::cout << "  real model: load " << secs(loadStart, submitted) << " s, first audio " << secs(submitted, firstReady)
              << " s, 30 s track " << secs(submitted, done) << " s\n";
    if (auto data = mix->current()) {
        double bass = 0.0, other = 0.0;
        for (std::int64_t i = rate; i < length - static_cast<std::int64_t>(rate); i += 7) {
            const float b = data->stored(StoredStem::Bass, 0, i);
            const float v = data->stored(StoredStem::Vocals, 0, i) + data->stored(StoredStem::Drums, 0, i);
            ok &= require(std::isfinite(b) && std::isfinite(v), "real stems are finite");
            if (!std::isfinite(b) || !std::isfinite(v))
                break;
            bass += b * b;
            other += v * v;
        }
        std::cout << "  bass energy " << bass << ", vocals+drums energy " << other << '\n';
        ok &= require(bass > other, "a pure 55 Hz tone lands in the bass stem");
    }
#if defined(__linux__)
    struct rusage usage {};
    const bool memoryMeasured = getrusage(RUSAGE_SELF, &usage) == 0;
    ok &= require(memoryMeasured, "real-model peak memory is measurable");
    if (memoryMeasured) {
        std::cout << "  peak RSS " << (static_cast<double>(usage.ru_maxrss) / 1024.0) << " MiB\n";
        ok &= require(usage.ru_maxrss < 4L * 1024 * 1024,
                      "real-model peak RSS stays below 4 GiB on Linux");
    }
#endif
    service.shutdown();
    std::filesystem::remove_all(dir);
    return ok;
#else
    const char* model = std::getenv("BROCKDJ_STEM_MODEL");
    return require(!model || !*model, "real-model test requires ONNX Runtime in this build");
#endif
}

} // namespace

bool testWaveformEnvelope()
{
    bool ok = true;
    // Two full blocks plus a short tail whose last bin is incomplete.
    const std::int64_t length = 2 * kReadyBlockSamples + kEnvelopeBinSamples + 17;
    StemTrackData data(7, 44100.0, length);
    std::array<float, kStemCount> energy {};
    ok &= require(!data.envelopeComplete() && !data.envelopeEnergy(0, length, energy),
                  "an empty envelope reports no energy");
    const auto squaresAt = [](std::int64_t sample) {
        const float scale = sample < kReadyBlockSamples ? 1.0f : 0.5f;
        return std::array<float, kStemCount> { 0.16f * scale, 0.09f * scale, 0.04f * scale, 0.0f };
    };
    const auto epoch0 = data.envelopeEpoch();
    for (std::int64_t s = 0; s < kReadyBlockSamples; ++s)
        data.accumulateEnvelope(s, squaresAt(s));
    ok &= require(data.envelopeReady(0) && !data.envelopeReady(kReadyBlockSamples)
                      && data.envelopeEpoch() != epoch0,
                  "a finished block publishes its envelope independently");
    ok &= require(data.envelopeEnergy(0, kReadyBlockSamples, energy)
                      && std::abs(energy[0] - 0.16f) < 1e-3f && std::abs(energy[1] - 0.09f) < 1e-3f
                      && std::abs(energy[2] - 0.04f) < 1e-3f && energy[3] < 1e-6f,
                  "envelope energy returns the mean per-stem energy");
    ok &= require(!data.envelopeEnergy(kReadyBlockSamples - 10, kReadyBlockSamples + 10, energy),
                  "ranges touching unanalysed blocks are not reported");
    for (std::int64_t s = kReadyBlockSamples; s < length; ++s)
        data.accumulateEnvelope(s, squaresAt(s));
    ok &= require(data.envelopeComplete() && data.envelopeEnergy(length - 5, length, energy)
                      && std::abs(energy[0] - 0.08f) < 1e-3f,
                  "short final bins and blocks complete the envelope");

    const std::array<float, kStemCount> muteVocals { 0.0f, 1.0f, 1.0f, 1.0f };
    const std::array<float, kStemCount> unity { 1.0f, 1.0f, 1.0f, 1.0f };
    const std::array<float, kStemCount> mix { 0.16f, 0.09f, 0.04f, 0.01f };
    ok &= require(std::abs(stemAudibleAmplitudeRatio(mix, unity) - 1.0f) < 1e-6f
                      && std::abs(stemAudibleAmplitudeRatio(mix, muteVocals) - std::sqrt(0.14f / 0.30f)) < 1e-4f
                      && stemAudibleAmplitudeRatio(mix, { 0.0f, 0.0f, 0.0f, 0.0f }) == 0.0f
                      && stemAudibleAmplitudeRatio({}, muteVocals) == 1.0f,
                  "audible ratio follows the energy of unmuted stems");
    const auto revision = stemWaveformOverlayRevision(7, muteVocals, 3);
    ok &= require(revision != 0 && revision != stemWaveformOverlayRevision(7, unity, 3)
                      && revision != stemWaveformOverlayRevision(7, muteVocals, 4)
                      && revision != stemWaveformOverlayRevision(8, muteVocals, 3)
                      && revision == stemWaveformOverlayRevision(7, muteVocals, 3),
                  "overlay revision tracks track, gains and envelope progress");
    return ok;
}

bool testWaveformTransition()
{
    bool ok = true;
    StemWaveformTransition transition;
    const StemWaveformTransition::Gains unity {1, 1, 1, 1};
    const StemWaveformTransition::Gains muted {0, 1, 1, 1};
    transition.retarget(muted, 0);
    ok &= require(transition.value(0) == unity && transition.value(16)[0] < 0.8f,
                  "the animated mute responds on its first frame");
    auto previous = transition.value(0);
    for (int time = 1; time <= 180; ++time) {
        const auto current = transition.value(time);
        ok &= require(current[0] <= previous[0] && current[0] >= 0 && current[1] == 1,
                      "mute transition is monotonic and does not affect other stems");
        previous = current;
    }
    ok &= require(transition.value(180) == muted && !transition.active(180),
                  "mute reaches its exact target within 180 ms");
    for (int toggle = 0; toggle < 100; ++toggle) {
        const double time = 200 + toggle * 12;
        const auto before = transition.value(time);
        transition.retarget(toggle % 2 == 0 ? unity : muted, time);
        ok &= require(before == transition.value(time), "rapid retargeting never jumps or queues transitions");
        const auto after = transition.value(time + 8);
        ok &= require(toggle % 2 == 0 ? after[0] >= before[0] : after[0] <= before[0],
                      "rapid retargeting immediately follows the newest direction");
    }
    transition.retarget(unity, 2000);
    ok &= require(transition.value(2180) == unity, "the latest toggle wins after a rapid sequence");
    return ok;
}

bool testLegacyEnvelopeUpgrade()
{
    bool ok = true;
    ServiceFixture fx;
    constexpr double rate = 44100;
    constexpr std::int64_t length = 44100 + 17;
    const auto file = writeWav(fx.dir, "legacy.wav", rate, length);
    auto status = std::make_shared<StemJobStatus>();
    auto mix = std::make_shared<StemMixState>();
    fx.service->submit(0, {utf8Path(file), 10, rate, length}, status, mix);
    ok &= require(waitFor([&] { return status->state.load() == StemJobState::Ready; }),
                  "legacy fixture finishes separation");
    const auto data = mix->current();
    if (!data)
        return false;
    // Recreate the v2 format: same audio identity/header, no envelope payload.
    struct LegacyHeader {
        char magic[4];
        std::uint32_t version;
        double sampleRate;
        std::int64_t length;
        std::int32_t stems, channels;
        std::uint64_t checksum;
    };
    std::uint64_t checksum = 14695981039346656037ull;
    const auto* raw = reinterpret_cast<const unsigned char*>(data->rawSamples().data());
    const auto bytes = data->rawSamples().size() * sizeof(std::int16_t);
    for (std::size_t i = 0; i < bytes; ++i)
        checksum = (checksum ^ raw[i]) * 1099511628211ull;
    const auto cache = fx.service->cacheFileFor(utf8Path(file));
    {
        std::ofstream out(cache, std::ios::binary | std::ios::trunc);
        const LegacyHeader header {{'B', 'S', 'T', 'M'}, 2, rate, length,
                                   kStoredStemCount, kStemChannels, checksum};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(raw), static_cast<std::streamsize>(bytes));
        ok &= require(static_cast<bool>(out), "legacy cache fixture is written");
    }
    const auto calls = fx.shared->calls.load();
    auto upgraded = std::make_shared<StemJobStatus>();
    fx.service->submitPreanalysis(utf8Path(file), upgraded);
    ok &= require(waitFor([&] { return upgraded->state.load() == StemJobState::Ready; })
                      && upgraded->fromCache.load() && fx.shared->calls.load() == calls,
                  "preanalysis upgrades legacy envelopes without rerunning inference");
    auto loaded = std::make_shared<StemJobStatus>();
    auto loadedMix = std::make_shared<StemMixState>();
    fx.service->submit(1, {utf8Path(file), 11, rate, length}, loaded, loadedMix);
    ok &= require(waitFor([&] { return loaded->state.load() == StemJobState::Ready; }),
                  "upgraded cache loads");
    const auto cached = loadedMix->current();
    ok &= require(cached && cached->envelopeComplete() && envelopeMatchesFakeModel(*cached)
                      && cached->rawEnvelope() == data->rawEnvelope() && fx.shared->calls.load() == calls,
                  "upgraded waveform is fully prepared when ready is published");
    return ok;
}

int main()
{
    bool ok = true;
    ok &= testChunkPlan();
    ok &= testCacheIdentity();
    ok &= testRemixAndReadScope();
    ok &= testWaveformEnvelope();
    ok &= testWaveformTransition();
    ok &= testLegacyEnvelopeUpgrade();
    ok &= testServiceSeparatesAndCaches(44100.0, 2.0e-4f);
    ok &= testServiceSeparatesAndCaches(48000.0, 2.0e-3f);
    ok &= testCorruptCacheIsRebuilt();
    ok &= testPlayheadPriorityAndCancel();
    ok &= testPlayheadOverlapPriority();
    ok &= testCrossDeckUrgency();
    ok &= testShortResampledTrackUsesSilencePadding();
    ok &= testCacheBudget();
    ok &= testPreanalysisCacheAndMetadata();
    ok &= testPreanalysisPriorityAndCancel();
    ok &= testPreanalysisWriteFailure();
    ok &= testPreanalysisReusesLiveCache();
    ok &= testMissingModelFails();
    ok &= testRealModel();
    if (!ok)
        return 1;
    std::cout << "stem core tests passed\n";
    return 0;
}
