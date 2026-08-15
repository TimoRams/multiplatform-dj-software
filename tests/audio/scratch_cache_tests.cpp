#include "audio/cache/AudioPageCache.h"
#include "audio/internal/ScratchResampler.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace {
bool require(bool value, const char* message) { if (!value) std::cerr << "FAIL: " << message << '\n'; return value; }
bool writeWave(const QString& path, double sr, int channels, int samples)
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
            data.setSample(ch, i, static_cast<float>(0.25 * std::sin(2.0 * juce::MathConstants<double>::pi * (330 + ch * 110) * i / sr)));
    return writer->writeFromAudioSampleBuffer(data, 0, samples);
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
    ok &= require(writeWave(stereo, 48000, 2, 70000), "stereo fixture");
    ok &= require(writeWave(mono, 44100, 1, 20000), "mono fixture");
    ok &= require(writeWave(highRate, 96000, 2, 20000), "96 kHz fixture");
    ok &= require(writeWave(ultraRate, 192000, 2, 20000), "192 kHz fixture");
    AudioPageCache cache(8 * 1024 * 1024);
    auto handle = cache.openTrack({stereo});
    ok &= require(waitResident(cache, handle, 0, 4), "scratch pages resident");

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
    ok &= require(forwardAfterOne < forwardLead,
                  "stale forward velocity is pulled back toward the hand target");
    scratch.processScratchTracking(10'512, 0.0, 8.0, {&loopOut, 0, 512});
    ok &= require(scratch.readPosition() - 10'512 < forwardAfterOne * 0.5,
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
    ok &= require(reverseAfterOne < reverseLead,
                  "stale reverse velocity is pulled back toward the hand target");
    scratch.processScratchTracking(19'488, 0.0, 8.0, {&loopOut, 0, 512});
    ok &= require(19'488 - scratch.readPosition() < reverseAfterOne * 0.5,
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
