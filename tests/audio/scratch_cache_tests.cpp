#include "audio/cache/AudioPageCache.h"
#include "audio/internal/ScratchResampler.h"
#include "StemAudioFixtures.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

namespace {
bool require(bool value, const char* message) { if (!value) std::cerr << "FAIL: " << message << '\n'; return value; }
bool writeWave(const QString& path, double sr, int channels, int samples,
               double frequencyHz = 330.0)
{
    juce::WavAudioFormat format;
    auto fs = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fs->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fs);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(sr).withNumChannels(channels).withBitsPerSample(16));
    if (!writer) return false;
    juce::AudioBuffer<float> data(channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            data.setSample(ch, i, static_cast<float>(0.25 * std::sin(
                2.0 * juce::MathConstants<double>::pi * frequencyHz * i / sr)));
    return writer->writeFromAudioSampleBuffer(data, 0, samples);
}

bool writeLoopEdgeWave(const QString& path)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream =
        std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(16));
    if (!writer)
        return false;
    juce::AudioBuffer<float> data(2, 70000);
    for (int channel = 0; channel < data.getNumChannels(); ++channel)
        for (int i = 0; i < data.getNumSamples(); ++i)
            data.setSample(channel, i, i >= 10000 && i < 58000 ? 0.25f : -0.25f);
    return writer->writeFromAudioSampleBuffer(data, 0, data.getNumSamples());
}

bool waitResident(AudioPageCache& cache, const AudioCacheHandle& h, int first, int last)
{
    cache.requestRange(h, first, last, AudioCachePriority::ScratchNearPlayhead);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        bool ready = true;
        for (int p = first; p <= last; ++p) ready = static_cast<bool>(cache.tryGetPage(h, p)) && ready;
        if (ready) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
bool finiteBlock(const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i) if (!std::isfinite(b.getSample(ch, i))) return false;
    return true;
}

bool checkStemScratch(AudioPageCache& cache, const AudioCacheHandle& handle)
{
    using namespace brockdj::stems;
    bool ok = true;
    auto data = makeHalfVocalStemData(cache, handle);
    if (!require(static_cast<bool>(data), "stem scratch fixture is resident"))
        return false;
    StemMixState state;
    state.publish(data);
    engine::audio::ScratchResampler reference, stemmed;
    for (auto* scratch : {&reference, &stemmed}) {
        scratch->prepare(2, 512, 48000);
        scratch->setTrackLengthSamples(handle.lengthInSamples());
        scratch->setTrackCacheSource(&cache, handle);
        scratch->reset(12000);
        scratch->snapSmoothedRate(1.0);
    }
    stemmed.setStemMixState(&state);
    juce::AudioBuffer<float> original(2, 512), output(2, 512);
    const auto render = [&](double rate) {
        reference.processBlock(rate, {&original, 0, 512});
        stemmed.processBlock(rate, {&output, 0, 512});
    };
    const auto differsFrom = [&](float ratio, float tolerance) {
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < 512; ++i)
                if (std::abs(output.getSample(channel, i)
                             - ratio * original.getSample(channel, i)) > tolerance)
                    return true;
        return false;
    };
    render(1.0);
    render(1.0);
    ok &= require(!differsFrom(1.0f, 0.0f), "unity stems preserve the scratch signal");
    state.setGain(Stem::Vocals, 0.0f);
    render(1.0);
    ok &= require(std::abs(output.getSample(0, 0) - original.getSample(0, 0)) < 0.0001f,
                  "scratch stem changes start with the previous signal");
    render(1.0);
    render(1.0);
    ok &= require(!differsFrom(0.5f, 0.00005f), "vocal mute follows the scratch playhead");

    for (auto* scratch : {&reference, &stemmed}) {
        scratch->setLoopRange(15000, 17000, true);
        scratch->setReadPositionSamples(15010);
        scratch->snapSmoothedRate(-1.0);
    }
    render(-1.0);
    ok &= require(!differsFrom(0.5f, 0.00005f), "scratch stem mute follows reverse loops");

    if (std::getenv("BROCKDJ_STEM_BENCHMARK")) {
        stemmed.setLoopRange(0, 0, false);
        stemmed.reset(3000);
        stemmed.snapSmoothedRate(1.0);
        double totalUs = 0.0;
        double worstUs = 0.0;
        for (int i = 0; i < 100; ++i) {
            state.setGain(Stem::Vocals, i % 2 == 0 ? 1.0f : 0.0f);
            const auto start = std::chrono::steady_clock::now();
            stemmed.processBlock(1.0, {&output, 0, 512});
            const double elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
            totalUs += elapsed;
            worstUs = std::max(worstUs, elapsed);
        }
        std::cout << "stem scratch 512-frame average us=" << totalUs / 100.0
                  << " worst us=" << worstUs << '\n';
    }
    return ok;
}

struct LoopReaderGate {
    std::atomic<bool> entered {false};
    std::atomic<bool> released {false};
};

class GatedLoopReader final : public juce::AudioFormatReader {
public:
    explicit GatedLoopReader(std::shared_ptr<LoopReaderGate> gate)
        : juce::AudioFormatReader(nullptr, "gated-loop"), m_gate(std::move(gate))
    {
        sampleRate = 48000.0;
        lengthInSamples = 70000;
        numChannels = 2;
        bitsPerSample = 32;
        usesFloatingPointData = true;
    }

    bool readSamples(int* const* destinations, int channels, int offset,
                     juce::int64 sourceStart, int count) override
    {
        if (sourceStart < AudioPage::kSamplesPerChannel) {
            m_gate->entered.store(true, std::memory_order_release);
            while (!m_gate->released.load(std::memory_order_acquire))
                m_gate->released.wait(false, std::memory_order_acquire);
        }
        for (int channel = 0; channel < channels; ++channel) {
            if (destinations[channel])
                std::fill_n(reinterpret_cast<float*>(destinations[channel]) + offset,
                            count, 0.25f);
        }
        return true;
    }

private:
    std::shared_ptr<LoopReaderGate> m_gate;
};

bool checkLoopCacheMiss(const QString& path)
{
    auto gate = std::make_shared<LoopReaderGate>();
    AudioPageCache cache(8 * 1024 * 1024);
    const auto handle = cache.openTrack({path}, std::make_unique<GatedLoopReader>(gate));
    bool ok = require(waitResident(cache, handle, 2, 3),
                      "loop cache miss fixture's current pages resident");
    engine::audio::ScratchResampler scratch;
    scratch.prepare(2, 128, 48000.0);
    scratch.setTrackLengthSamples(handle.lengthInSamples());
    scratch.setTrackCacheSource(&cache, handle);
    scratch.setLoopRange(10000.0, 58000.0, true);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gate->entered.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ok &= require(gate->entered.load(std::memory_order_acquire),
                  "loop destination prefetch reaches the blocked worker");

    scratch.reset(45000.0);
    scratch.snapSmoothedRate(1.0);
    juce::AudioBuffer<float> output(2, 128);
    for (int block = 0; block < 3; ++block)
        scratch.processBlock(1.0, {&output, 0, 128});
    const auto before = scratch.cacheStats();
    scratch.setReadPositionSamples(54580.0);
    std::atomic<bool> callbackDone {false};
    std::thread callback([&] {
        scratch.processBlock(1.0, {&output, 0, 128});
        callbackDone.store(true, std::memory_order_release);
    });
    const auto callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!callbackDone.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < callbackDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool returnedWhileBlocked = callbackDone.load(std::memory_order_acquire);
    gate->released.store(true, std::memory_order_release);
    gate->released.notify_all();
    callback.join();
    const auto after = scratch.cacheStats();
    double maximumError = 0.0;
    for (int sample = 0; sample < 128; ++sample)
        maximumError = std::max(maximumError,
            std::abs(static_cast<double>(output.getSample(0, sample)) - 0.25));
    ok &= require(returnedWhileBlocked && after.pageMisses > before.pageMisses,
                  "loop cache miss requests work without waiting for the decoder");
    ok &= require(after.starvationBlocks == before.starvationBlocks && maximumError < 0.001,
                  "missing loop destination keeps the still-resident window audible");
    ok &= require(waitResident(cache, handle, 0, 4), "loop destination eventually becomes resident");
    scratch.processBlock(1.0, {&output, 0, 128});
    ok &= require(finiteBlock(output) && scratch.cacheStats().diskReadsFromAudioThread == 0,
                  "loop cache recovery stays finite and callback-safe");
    return ok;
}

double renderRms(engine::audio::ScratchResampler& scratch, double rate)
{
    juce::AudioBuffer<float> output(2, 512);
    for (int block = 0; block < 10; ++block)
        scratch.processBlock(rate, {&output, 0, output.getNumSamples()});

    double sumSquares = 0.0;
    for (int ch = 0; ch < output.getNumChannels(); ++ch)
        for (int i = 0; i < output.getNumSamples(); ++i) {
            const double sample = output.getSample(ch, i);
            sumSquares += sample * sample;
        }
    return std::sqrt(sumSquares
        / static_cast<double>(output.getNumChannels() * output.getNumSamples()));
}

double wrapLoop(double position, double in, double out)
{
    double offset = std::fmod(position - in, out - in);
    if (offset < 0.0)
        offset += out - in;
    return in + offset;
}

bool checkLoopScratch(AudioPageCache& cache,
                      const AudioCacheHandle& tone,
                      const AudioCacheHandle& edges)
{
    using engine::audio::ScratchResampler;
    constexpr double loopIn = 10000.0;
    constexpr double loopOut = 58000.0;
    bool ok = true;
    for (const int blockSize : {64, 128, 512}) {
        for (const double rate : {-1.0, 1.0}) {
            ScratchResampler scratch;
            scratch.prepare(2, blockSize, 48000.0);
            scratch.setTrackLengthSamples(tone.lengthInSamples());
            scratch.setTrackCacheSource(&cache, tone);
            scratch.setLoopRange(loopIn, loopOut, true);
            const double start = rate > 0.0
                ? loopOut - 2.5 * blockSize : loopIn + 2.5 * blockSize;
            scratch.reset(start);
            scratch.snapSmoothedRate(rate);
            juce::AudioBuffer<float> output(2, blockSize);
            double maximumError = 0.0;
            for (int block = 0; block < 6; ++block) {
                scratch.processBlock(rate, {&output, 0, blockSize});
                if (block < 2)
                    continue;
                for (int sample = 0; sample < blockSize; ++sample) {
                    const double position = start
                        + rate * (block * blockSize + sample);
                    const double expected = 0.25 * std::sin(
                        2.0 * juce::MathConstants<double>::pi * 330.0
                        * position / 48000.0);
                    maximumError = std::max(maximumError, std::abs(
                        static_cast<double>(output.getSample(0, sample)) - expected));
                }
            }
            if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE"))
                std::cout << "long-loop waveform block=" << blockSize
                          << " rate=" << rate << " error=" << maximumError << '\n';
            ok &= require(maximumError < 0.001,
                          "resident long loop stays audible and phase-correct across the wrap");
            ok &= require(scratch.cacheStats().starvationBlocks == 0,
                          "resident long loop does not starve at the wrap");
        }
    }

    for (const double rate : {-1.0, 1.0}) {
        ScratchResampler scratch;
        scratch.prepare(2, 128, 48000.0);
        scratch.setTrackLengthSamples(tone.lengthInSamples());
        scratch.setTrackCacheSource(&cache, tone);
        scratch.setLoopRange(loopIn, loopOut, true);
        const double start = rate > 0.0 ? loopOut - 64.0 : loopIn + 64.0;
        scratch.reset(start);
        scratch.snapSmoothedRate(rate);
        scratch.primeTrackerVelocity(rate);
        juce::AudioBuffer<float> output(2, 128);
        double maximumError = 0.0;
        double maximumRateError = 0.0;
        for (int block = 0; block < 8; ++block) {
            const double target = start + rate * block * 128.0;
            scratch.processScratchTracking(target, rate, 10.0, {&output, 0, 128}, 0.0);
            const double expected = wrapLoop(target + rate * 128.0, loopIn, loopOut);
            const double error = std::remainder(scratch.readPosition() - expected,
                                                loopOut - loopIn);
            maximumError = std::max(maximumError, std::abs(error));
            const auto stats = scratch.motionStats();
            maximumRateError = std::max({maximumRateError,
                                         std::abs(stats.minRate - rate),
                                         std::abs(stats.maxRate - rate)});
        }
        if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE"))
            std::cout << "loop tracker rate=" << rate << " position-error="
                      << maximumError << " rate-error=" << maximumRateError << '\n';
        ok &= require(maximumError < 2.0 && maximumRateError < 0.02,
                      "position tracker retains hand travel and speed across the loop");

        constexpr double editedIn = 9000.0;
        constexpr double editedOut = 59000.0;
        const double cursorBeforeEdit = scratch.readPosition();
        scratch.setLoopRange(editedIn, editedOut, true);
        const double editedAnchor = wrapLoop(cursorBeforeEdit, editedIn, editedOut);
        for (int block = 0; block < 4; ++block) {
            const double target = start + rate * (8 + block) * 128.0;
            scratch.processScratchTracking(target, rate, 10.0, {&output, 0, 128}, 0.0);
        }
        ok &= require(std::abs(std::remainder(
                          scratch.readPosition()
                              - wrapLoop(editedAnchor + rate * 4.0 * 128.0,
                                         editedIn, editedOut),
                          editedOut - editedIn)) < 2.0,
                      "editing an active loop preserves the continuous hand trajectory");

        const double cursorBeforeClear = scratch.readPosition();
        scratch.setLoopRange(0.0, 0.0, false);
        for (int block = 0; block < 4; ++block) {
            const double target = start + rate * (12 + block) * 128.0;
            scratch.processScratchTracking(target, rate, 10.0, {&output, 0, 128}, 0.0);
        }
        ok &= require(std::abs(scratch.readPosition()
                               - (cursorBeforeClear + rate * 4.0 * 128.0)) < 2.0,
                      "clearing a loop preserves the current scratch cursor and hand motion");

        constexpr double newIn = 12000.0;
        constexpr double newOut = 57000.0;
        const double cursorBeforeNewLoop = scratch.readPosition();
        scratch.setLoopRange(newIn, newOut, true);
        const double newAnchor = wrapLoop(cursorBeforeNewLoop, newIn, newOut);
        for (int block = 0; block < 4; ++block) {
            const double target = start + rate * (16 + block) * 128.0;
            scratch.processScratchTracking(target, rate, 10.0, {&output, 0, 128}, 0.0);
        }
        ok &= require(std::abs(std::remainder(
                          scratch.readPosition()
                              - wrapLoop(newAnchor + rate * 4.0 * 128.0, newIn, newOut),
                          newOut - newIn)) < 2.0,
                      "changing a loop rebases the hand trajectory without a speed spike");
    }

    for (const double rate : {-8.0, -0.37, 0.37, 8.0}) {
        ScratchResampler scratch;
        scratch.prepare(2, 512, 48000.0);
        scratch.setTrackLengthSamples(edges.lengthInSamples());
        scratch.setTrackCacheSource(&cache, edges);
        scratch.setLoopRange(20000.49, 20002.49, true);
        scratch.reset(20001.0);
        scratch.snapSmoothedRate(rate);
        scratch.primeTrackerVelocity(rate);
        juce::AudioBuffer<float> output(2, 512);
        double maximumError = 0.0;
        for (int block = 0; block < 8; ++block) {
            scratch.processScratchTracking(20001.0 + rate * block * 512.0,
                                            rate, 10.0, {&output, 0, 512}, 0.0);
            if (block == 0)
                continue;
            for (int sample = 0; sample < 512; ++sample)
                maximumError = std::max(maximumError,
                    std::abs(static_cast<double>(output.getSample(0, sample)) - 0.25));
        }
        const double expected = wrapLoop(20001.0 + rate * 8.0 * 512.0, 20000.0, 20002.0);
        ok &= require(maximumError < 0.001
                          && std::abs(std::remainder(scratch.readPosition() - expected, 2.0)) < 0.1
                          && scratch.cacheStats().starvationBlocks == 0,
                      "tiny loops repeat safely over many turns and negative hand coordinates");
    }

    if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_BENCHMARK")) {
        ScratchResampler scratch;
        scratch.prepare(2, 128, 48000.0);
        scratch.setTrackLengthSamples(edges.lengthInSamples());
        scratch.setTrackCacheSource(&cache, edges);
        scratch.setLoopRange(20000.0, 20002.0, true);
        scratch.reset(20001.0);
        scratch.snapSmoothedRate(8.0);
        juce::AudioBuffer<float> output(2, 128);
        constexpr int iterations = 2000;
        double totalMicros = 0.0;
        double worstMicros = 0.0;
        for (int block = 0; block < iterations; ++block) {
            const auto start = std::chrono::steady_clock::now();
            scratch.processBlock(8.0, {&output, 0, 128});
            const double elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
            totalMicros += elapsed;
            worstMicros = std::max(worstMicros, elapsed);
        }
        std::cout << "two-sample loop 8x, 128-frame callback: average-us="
                  << totalMicros / iterations << " worst-us=" << worstMicros << '\n';
    }

    {
        ScratchResampler looped, direct;
        for (auto* reader : {&looped, &direct}) {
            reader->prepare(2, 128, 48000.0);
            reader->setTrackLengthSamples(tone.lengthInSamples());
            reader->setTrackCacheSource(&cache, tone);
        }
        looped.setLoopRange(loopIn, loopOut, true);
        constexpr double start = loopOut - 128.0;
        for (auto* reader : {&looped, &direct}) {
            reader->reset(start);
            reader->snapSmoothedRate(1.5);
            reader->primeTrackerVelocity(1.5);
        }
        juce::AudioBuffer<float> loopOutput(2, 128), directOutput(2, 128);
        double target = start;
        double maxPositionDifference = 0.0;
        double maxRateDifference = 0.0;
        for (int block = 0; block < 16; ++block) {
            const double rate = block < 4 ? 1.5 : -1.5;
            const double loopRate = looped.processScratchTracking(
                target, rate, 10.0, {&loopOutput, 0, 128}, 0.0);
            const double directRate = direct.processScratchTracking(
                target, rate, 10.0, {&directOutput, 0, 128}, 0.0);
            target += rate * 128.0;
            maxPositionDifference = std::max(maxPositionDifference, std::abs(
                std::remainder(looped.readPosition()
                    - wrapLoop(direct.readPosition(), loopIn, loopOut), loopOut - loopIn)));
            maxRateDifference = std::max(maxRateDifference, std::abs(loopRate - directRate));
        }
        ok &= require(maxPositionDifference < 0.01 && maxRateDifference < 0.0001,
                      "loop crossing and reversal keep the same continuous scratch trajectory");
    }

    for (const double rate : {-1.0, 1.0}) {
        ScratchResampler scratch;
        scratch.prepare(2, 128, 48000.0);
        scratch.setTrackLengthSamples(edges.lengthInSamples());
        scratch.setTrackCacheSource(&cache, edges);
        scratch.setLoopRange(loopIn, loopOut, true);
        const double start = rate > 0.0 ? loopOut - 2.25 : loopIn + 2.25;
        scratch.reset(start - rate * 256.0);
        scratch.snapSmoothedRate(rate);
        juce::AudioBuffer<float> output(2, 128);
        double maximumError = 0.0;
        for (int block = 0; block < 4; ++block) {
            scratch.processBlock(rate, {&output, 0, 128});
            if (block < 2)
                continue;
            for (int sample = 0; sample < 128; ++sample)
                maximumError = std::max(maximumError,
                    std::abs(static_cast<double>(output.getSample(0, sample)) - 0.25));
        }
        ok &= require(maximumError < 0.001,
                      "sinc interpolation uses loop samples rather than audio outside the loop");
    }

    for (const bool loopActive : {false, true}) {
        ScratchResampler scratch;
        scratch.prepare(2, 128, 48000.0);
        scratch.setTrackLengthSamples(edges.lengthInSamples());
        scratch.setTrackCacheSource(&cache, edges);
        scratch.setLoopRange(loopIn, loopOut, loopActive);
        scratch.reset(20000.0);
        scratch.snapSmoothedRate(1.0);
        juce::AudioBuffer<float> output(2, 128);
        for (int block = 0; block < 2; ++block)
            scratch.processBlock(1.0, {&output, 0, 128});
        const float lastMovingSample = output.getSample(0, 127);
        const double cursor = scratch.readPosition();
        scratch.reset(cursor);
        scratch.processScratchTracking(cursor, 0.0, 10.0, {&output, 0, 128}, 0.0);
        const float firstStoppedSample = output.getSample(0, 0);
        scratch.processScratchTracking(cursor, 0.0, 10.0, {&output, 0, 128}, 0.0);
        ok &= require(std::abs(firstStoppedSample - lastMovingSample) < 0.005
                          && output.getRMSLevel(0, 0, 128) < 1.0e-6,
                      "a held stationary platter fades smoothly to silence");
        scratch.reset(cursor);
        scratch.snapSmoothedRate(1.0);
        scratch.primeTrackerVelocity(1.0);
        scratch.processScratchTracking(cursor, 1.0, 10.0, {&output, 0, 128}, 0.0);
        ok &= require(std::abs(output.getSample(0, 0)) < 0.005
                          && std::abs(output.getSample(0, 127) - 0.25f) < 0.001,
                      "motion after a stationary hold fades back in without an abrupt onset");
    }
    return ok;
}

bool checkHighRateLoop(AudioPageCache& cache, const AudioCacheHandle& handle)
{
    using engine::audio::ScratchResampler;
    constexpr double loopIn = 24000.0;
    constexpr double loopOut = 72000.0;
    bool ok = true;
    for (const auto [blockSize, normalizedSpeed] : {
             std::pair{128, 8.0}, std::pair{512, 2.0}, std::pair{2048, 1.0}}) {
        for (const double direction : {-1.0, 1.0}) {
            ScratchResampler looped, direct;
            for (auto* reader : {&looped, &direct}) {
                reader->prepare(2, blockSize, 48000.0);
                reader->setTrackLengthSamples(handle.lengthInSamples());
                reader->setTrackCacheSource(&cache, handle);
            }
            looped.setLoopRange(loopIn, loopOut, true);
            const double rate = direction * normalizedSpeed * 4.0;
            const double start = rate > 0.0
                ? loopOut - 2.5 * blockSize * rate
                : loopIn - 2.5 * blockSize * rate;
            for (auto* reader : {&looped, &direct}) {
                reader->reset(start);
                reader->snapSmoothedRate(rate);
            }
            juce::AudioBuffer<float> loopOutput(2, blockSize), directOutput(2, blockSize);
            double maximumDifference = 0.0;
            for (int block = 0; block < 4; ++block) {
                looped.processBlock(rate, {&loopOutput, 0, blockSize});
                direct.processBlock(rate, {&directOutput, 0, blockSize});
                if (block < 2)
                    continue;
                for (int sample = 0; sample < blockSize; ++sample) {
                    maximumDifference = std::max(maximumDifference, std::abs(
                        static_cast<double>(loopOutput.getSample(0, sample))
                            - directOutput.getSample(0, sample)));
                }
            }
            if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE"))
                std::cout << "192/48 loop block=" << blockSize << " rate=" << rate
                          << " waveform-difference=" << maximumDifference << '\n';
            ok &= require(maximumDifference < 0.001
                              && looped.cacheStats().starvationBlocks == 0,
                          "192/48 kHz loop wrap preserves high-speed audio in both directions");
        }
    }
    return ok;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir dir;
    const QString stereo = dir.filePath("scratch.wav");
    const QString mono = dir.filePath("mono.wav");
    const QString highRate = dir.filePath("high-rate.wav");
    const QString ultraRate = dir.filePath("ultra-rate.wav");
    const QString ultraRatePass = dir.filePath("ultra-rate-pass.wav");
    const QString ultraRateAlias = dir.filePath("ultra-rate-alias.wav");
    const QString loopEdges = dir.filePath("loop-edges.wav");
    const QString highRateLoop = dir.filePath("high-rate-loop.wav");
    ok &= require(writeWave(stereo, 48000, 2, 70000), "stereo fixture");
    ok &= require(writeWave(mono, 44100, 1, 20000), "mono fixture");
    ok &= require(writeWave(highRate, 96000, 2, 20000), "96 kHz fixture");
    ok &= require(writeWave(ultraRate, 192000, 2, 20000), "192 kHz fixture");
    ok &= require(writeWave(ultraRatePass, 192000, 2, 96000, 10000.0),
                  "192 kHz passband fixture");
    ok &= require(writeWave(ultraRateAlias, 192000, 2, 96000, 60000.0),
                  "192 kHz stopband fixture");
    ok &= require(writeLoopEdgeWave(loopEdges), "loop-edge fixture");
    ok &= require(writeWave(highRateLoop, 192000, 2, 120000, 320.0),
                  "high-rate loop fixture");
    ok &= checkLoopCacheMiss(stereo);
    AudioPageCache cache(8 * 1024 * 1024);
    auto handle = cache.openTrack({stereo});
    ok &= require(waitResident(cache, handle, 0, 4), "scratch pages resident");
    ok &= checkStemScratch(cache, handle);
    auto edgeHandle = cache.openTrack({loopEdges});
    ok &= require(waitResident(cache, edgeHandle, 0, 4), "loop-edge pages resident");
    ok &= checkLoopScratch(cache, handle, edgeHandle);
    cache.releaseTrack(edgeHandle);
    auto highRateLoopHandle = cache.openTrack({highRateLoop});
    ok &= require(waitResident(cache, highRateLoopHandle, 0,
                               static_cast<int>(highRateLoopHandle.pageCount() - 1)),
                  "high-rate loop pages resident");
    ok &= checkHighRateLoop(cache, highRateLoopHandle);
    cache.releaseTrack(highRateLoopHandle);

    engine::audio::ScratchResampler scratch;
    scratch.prepare(2, 4096, 48000);
    scratch.setTrackLengthSamples(handle.lengthInSamples());
    scratch.setTrackCacheSource(&cache, handle);
    scratch.reset(16382.5); // Hermite neighborhood crosses a page boundary.

    for (int blockSize : {64, 128, 256, 512, 1024, 2048, 4096, 8192}) {
        juce::AudioBuffer<float> output(2, blockSize);
        juce::AudioSourceChannelInfo info(&output, 0, blockSize);
        scratch.processBlock(0.35, info);
        ok &= require(finiteBlock(output), "forward block finite across sizes");
        scratch.processBlock(-2.5, info);
        ok &= require(finiteBlock(output), "reverse/backspin block finite");
    }

    scratch.setLoopRange(16000, 17000, true);
    scratch.setReadPositionSamples(16999);
    juce::AudioBuffer<float> loopOut(2, 512);
    scratch.processBlock(1.0, {&loopOut, 0, 512});
    ok &= require(scratch.readPosition() >= 16000 && scratch.readPosition() <= 17001,
                  "loop wraps in cache-backed scratch");
    scratch.setLoopRange(0, 0, false);
    scratch.setReadPositionSamples(-1000);
    scratch.processBlock(-1.0, {&loopOut, 0, 512});
    ok &= require(scratch.readPosition() >= 0, "negative pre-roll never requests negative samples");

    auto monoHandle = cache.openTrack({mono});
    ok &= require(waitResident(cache, monoHandle, 0, 1), "mono pages resident");
    scratch.setTrackCacheSource(&cache, monoHandle);
    scratch.setTrackLengthSamples(monoHandle.lengthInSamples());
    scratch.reset(100);
    scratch.processBlock(0.1, {&loopOut, 0, 512});
    ok &= require(finiteBlock(loopOut), "mono duplicates safely to stereo");

    for (const auto& path : {highRate, ultraRate}) {
        auto rateHandle = cache.openTrack({path});
        ok &= require(waitResident(cache, rateHandle, 0, 1), "high sample-rate pages resident");
        scratch.setTrackCacheSource(&cache, rateHandle);
        scratch.setTrackLengthSamples(rateHandle.lengthInSamples());
        scratch.prepare(2, 512, rateHandle.sampleRate());
        scratch.reset(1000);
        scratch.processBlock(1.0, {&loopOut, 0, 512});
        ok &= require(finiteBlock(loopOut), "96/192 kHz scratch remains finite");
        cache.releaseTrack(rateHandle);
    }
    scratch.prepare(2, 512, 48000);

    // A 192 kHz source advances four source samples per 48 kHz output sample
    // even at normal 1x playback. The scratch anti-alias table must use that
    // absolute source/output ratio; clamping its cutoff model at normalized 8x
    // folds ultrasonic source content into the audible band.
    double highRatePassRms = 0.0;
    double highRateAliasRms = 1.0;
    double highRateTrackingNormalized = 0.0;
    double highRateTrackingErrorSamples = 1.0e9;
    std::uint64_t highRateTrackingStarvation = 1;
    for (const auto& qualityCase : {
             std::pair<QString, bool> {ultraRatePass, true},
             std::pair<QString, bool> {ultraRateAlias, false}}) {
        auto qualityHandle = cache.openTrack({qualityCase.first});
        ok &= require(waitResident(cache, qualityHandle, 0,
                                   static_cast<int>(qualityHandle.pageCount() - 1)),
                      "192 kHz quality pages resident");
        scratch.setTrackCacheSource(&cache, qualityHandle);
        scratch.setTrackLengthSamples(qualityHandle.lengthInSamples());
        scratch.reset(20000);
        scratch.prefetchAround(scratch.readPosition());
        const double rms = renderRms(scratch, 192000.0 / 48000.0);
        if (qualityCase.second) {
            highRatePassRms = rms;

            // Motion limits are specified in normalized platter rates, while
            // the resampler advances source samples. At 192/48 kHz every
            // acceleration, jerk and cache bound must therefore scale by four.
            constexpr int trackingBlocks = 4;
            constexpr double normalizedCommand = 8.0;
            constexpr double sourcePerOutput = 192000.0 / 48000.0;
            constexpr double commandedRate = normalizedCommand * sourcePerOutput;
            const auto starvationBefore = scratch.cacheStats().starvationBlocks;
            const double trackingStart = 20000.0;
            double target = trackingStart;
            scratch.reset(trackingStart);
            scratch.snapSmoothedRate(commandedRate);
            scratch.primeTrackerVelocity(commandedRate);
            juce::AudioBuffer<float> trackingOutput(2, 512);
            for (int block = 0; block < trackingBlocks; ++block) {
                scratch.processScratchTracking(
                    target, commandedRate,
                    engine::audio::ScratchResampler::kMaximumTrackingRate
                        * sourcePerOutput,
                    {&trackingOutput, 0, trackingOutput.getNumSamples()}, 0.0);
                target += commandedRate * trackingOutput.getNumSamples();
            }
            const double achieved = scratch.readPosition() - trackingStart;
            highRateTrackingNormalized = achieved
                / (trackingBlocks * trackingOutput.getNumSamples() * sourcePerOutput);
            highRateTrackingErrorSamples = std::abs(scratch.readPosition() - target);
            highRateTrackingStarvation = scratch.cacheStats().starvationBlocks
                - starvationBefore;
        } else {
            highRateAliasRms = rms;
        }
        cache.releaseTrack(qualityHandle);
    }
    ok &= require(highRatePassRms > 0.1,
                  "192 kHz source keeps its legal 48 kHz-device passband");
    ok &= require(highRateAliasRms < highRatePassRms * 0.01,
                  "192 kHz source rejects ultrasonic fold-back on a 48 kHz device");
    ok &= require(std::abs(highRateTrackingNormalized - 8.0) < 0.02
                      && highRateTrackingErrorSamples < 2.0,
                  "192/48 kHz tracker preserves normalized 8x speed and position");
    ok &= require(highRateTrackingStarvation == 0,
                  "192/48 kHz 8x tracking stays inside its source-rate window");
    if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE"))
        std::cout << "192/48 kHz scratch RMS: pass=" << highRatePassRms
                  << " alias=" << highRateAliasRms
                  << " tracking=" << highRateTrackingNormalized
                  << "x error=" << highRateTrackingErrorSamples << " samples\n";

    AudioPageCache starvedCache(8 * 1024 * 1024);
    auto starvedHandle = starvedCache.openTrack({stereo});
    scratch.setTrackCacheSource(&starvedCache, starvedHandle);
    scratch.setTrackLengthSamples(starvedHandle.lengthInSamples());
    scratch.reset(33000);
    loopOut.clear();
    scratch.processBlock(1.0, {&loopOut, 0, 512});
    ok &= require(finiteBlock(loopOut), "cache miss fade remains finite");
    ok &= require(scratch.cacheStats().starvationBlocks > 0, "starvation counted");
    ok &= require(waitResident(starvedCache, starvedHandle, 0, 4), "miss requests eventually decode");
    scratch.processBlock(1.0, {&loopOut, 0, 512});
    ok &= require(scratch.cacheStats().recoveryEvents > 0, "recovery fade counted");

    // The next speculative page is absent, but the previously decoded local
    // window still covers this block. A failed refill must neither overwrite
    // that window nor fade a playable scratch to silence.
    AudioPageCache boundaryCache(8 * 1024 * 1024);
    auto boundaryHandle = boundaryCache.openTrack({stereo});
    ok &= require(waitResident(boundaryCache, boundaryHandle, 1, 1),
                  "boundary fixture's current page resident");
    engine::audio::ScratchResampler boundaryScratch;
    boundaryScratch.prepare(2, 128, 48000);
    boundaryScratch.setTrackLengthSamples(boundaryHandle.lengthInSamples());
    boundaryScratch.setTrackCacheSource(&boundaryCache, boundaryHandle);
    boundaryScratch.reset(24'000);
    juce::AudioBuffer<float> boundaryOut(2, 128);
    for (int block = 0; block < 3; ++block)
        boundaryScratch.processScratchTracking(
            24'000, 0.0, 8.0, {&boundaryOut, 0, 128}, 0.0);
    boundaryScratch.snapSmoothedRate(0.05);
    boundaryScratch.processBlock(0.05, {&boundaryOut, 0, 128});
    const auto starvationBeforeBoundary = boundaryScratch.cacheStats().starvationBlocks;
    const auto missesBeforeBoundary = boundaryScratch.cacheStats().pageMisses;
    boundaryScratch.setReadPositionSamples(28'870);
    boundaryScratch.processBlock(0.05, {&boundaryOut, 0, 128});
    double boundaryEnergy = 0.0;
    for (int i = 0; i < 128; ++i)
        boundaryEnergy += std::abs(boundaryOut.getSample(0, i));
    ok &= require(boundaryScratch.cacheStats().pageMisses > missesBeforeBoundary,
                  "boundary refill observes the missing speculative page");
    ok &= require(boundaryScratch.cacheStats().starvationBlocks == starvationBeforeBoundary
                      && boundaryEnergy > 1.0,
                  "missing speculative page keeps the current scratch window audible");

    cache.releaseTrack(monoHandle);
    scratch.setTrackCacheSource(&cache, handle);
    scratch.reset(1000);
    scratch.processScratchTracking(2000, 0.0, 8.0, {&loopOut, 0, 512});
    ok &= require(finiteBlock(loopOut), "position tracker remains finite after generation switch");

    // Sparse jog ticks describe a moving platter, not a sequence of stops. The
    // target given to a callback is a snapshot from before it started, so the
    // hand keeps moving for the whole block it renders: the reader is allowed
    // to lead by that block plus the age the event already had, and no further.
    // Once the command goes stale the absolute target pulls it back.
    constexpr double kBlockLeadSamples = 512.0 + 0.002 * 48'000.0;  // block + input lead
    scratch.reset(10'000);
    scratch.processScratchTracking(10'512, 1.0, 8.0, {&loopOut, 0, 512});
    const double forwardLead = scratch.readPosition() - 10'512;
    ok &= require(forwardLead > 0.0,
                  "moving-reference tracker bridges a sparse forward jog tick");
    ok &= require(forwardLead <= kBlockLeadSamples + 1.0,
                  "forward jog prediction stays within one callback of the hand");
    // The hand estimate is carried across blocks and corrected rather than
    // rebuilt, so a command that goes stale is walked back over the next couple
    // of blocks instead of snapping. It must still converge, and monotonically.
    scratch.processScratchTracking(10'512, 0.0, 8.0, {&loopOut, 0, 512});
    const double forwardAfterOne = scratch.readPosition() - 10'512;
    ok &= require(std::abs(forwardAfterOne) < std::abs(forwardLead),
                  "stale forward velocity is pulled back toward the hand target");
    scratch.processScratchTracking(10'512, 0.0, 8.0, {&loopOut, 0, 512});
    const double forwardAfterTwo = scratch.readPosition() - 10'512;
    if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE"))
        std::cout << "stale forward lead: " << forwardLead << " -> "
                  << forwardAfterOne << " -> " << forwardAfterTwo << '\n';
    ok &= require(std::abs(forwardAfterTwo) < std::abs(forwardAfterOne) * 0.5,
                  "stale forward command keeps converging on the hand target");

    scratch.reset(20'000);
    scratch.processScratchTracking(19'488, -1.0, 8.0, {&loopOut, 0, 512});
    const double reverseLead = 19'488 - scratch.readPosition();
    ok &= require(reverseLead > 0.0,
                  "moving-reference tracker bridges a sparse reverse jog tick");
    ok &= require(reverseLead <= kBlockLeadSamples + 1.0,
                  "reverse jog prediction stays within one callback of the hand");
    scratch.processScratchTracking(19'488, 0.0, 8.0, {&loopOut, 0, 512});
    const double reverseAfterOne = 19'488 - scratch.readPosition();
    ok &= require(std::abs(reverseAfterOne) < std::abs(reverseLead),
                  "stale reverse velocity is pulled back toward the hand target");
    scratch.processScratchTracking(19'488, 0.0, 8.0, {&loopOut, 0, 512});
    const double reverseAfterTwo = 19'488 - scratch.readPosition();
    if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE"))
        std::cout << "stale reverse lead: " << reverseLead << " -> "
                  << reverseAfterOne << " -> " << reverseAfterTwo << '\n';
    ok &= require(std::abs(reverseAfterTwo) < std::abs(reverseAfterOne) * 0.5,
                  "stale reverse command keeps converging on the hand target");

    const auto stats = scratch.cacheStats();
    ok &= require(stats.diskReadsFromAudioThread == 0, "diskReadsFromAudioThread must remain zero");

    if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_BENCHMARK")) {
        constexpr int iterations = 2000;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i)
            scratch.processBlock((i & 1) ? 1.0 : -1.0, {&loopOut, 0, 512});
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count() / iterations;
        std::cout << "scratch cache benchmark: 512-sample block us=" << elapsed << '\n';
    }

    for (int i = 0; i < 2000; ++i) {
        scratch.setReadPositionSamples((i * 7919) % 69000);
        scratch.processBlock((i & 1) ? 5.0 : -5.0, {&loopOut, 0, 512});
        ok &= require(finiteBlock(loopOut), "deterministic scratch stress finite");
    }
    ok &= require(scratch.cacheStats().diskReadsFromAudioThread == 0, "stress performs no audio-thread disk reads");
    return ok ? 0 : 1;
}
