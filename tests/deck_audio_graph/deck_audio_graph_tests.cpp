#include "engine/deck/DeckAudioGraph.h"
#include "engine/DjMasterBus.h"

#include "audio/cache/AudioPageCache.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "engine/dsp/MixerDspSource.h"
#include "engine/dsp/ScratchDeckBridge.hpp"
#include "engine/dsp/TimeStretchAudioSource.h"
#include "engine/scratch/ScratchController.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

bool writeWave(const QString& path, double sampleRate, int channels, double frequency)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream =
        std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions{}
            .withSampleRate(sampleRate)
            .withNumChannels(channels)
            .withBitsPerSample(16));
    if (!writer)
        return false;

    juce::AudioBuffer<float> buffer(channels, 48'000);
    for (int channel = 0; channel < channels; ++channel) {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            const auto phase = juce::MathConstants<double>::twoPi
                * (frequency + channel * 110.0) * sample / sampleRate;
            buffer.setSample(channel, sample, static_cast<float>(0.2 * std::sin(phase)));
        }
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool writeSampleRampWave(const QString& path, double sampleRate, int channels)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream =
        std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions{}
            .withSampleRate(sampleRate)
            .withNumChannels(channels)
            .withBitsPerSample(24));
    if (!writer)
        return false;

    juce::AudioBuffer<float> buffer(channels, 48'000);
    const double denominator = static_cast<double>(buffer.getNumSamples() - 1);
    for (int channel = 0; channel < channels; ++channel) {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            const double encodedPosition = static_cast<double>(sample) / denominator;
            buffer.setSample(channel, sample,
                             static_cast<float>(-0.6 + encodedPosition * 1.2));
        }
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool waitForPage(AudioPageCache& cache, const AudioCacheHandle& handle)
{
    cache.requestRange(handle, 0, handle.pageCount() - 1, AudioCachePriority::PlaybackReadAhead);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cache.tryGetPage(handle, 0))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool isFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (!std::isfinite(buffer.getSample(channel, sample)))
                return false;
    return true;
}

double absolutePeak(const juce::AudioBuffer<float>& buffer)
{
    double peak = 0.0;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            peak = std::max(peak, std::abs(static_cast<double>(buffer.getSample(channel, sample))));
    return peak;
}

bool realtimeCountersAreZero(DeckAudioGraph& graph)
{
    const auto stats = graph.realtimeStats();
    return stats.diskReadsFromAudioThread == 0
        && stats.decoderCallsFromAudioThread == 0
        && stats.prepareCallsFromAudioThread == 0
        && stats.resetCallsFromAudioThread == 0
        && stats.prewarmCallsFromAudioThread == 0
        && stats.coefficientBuildsFromAudioThread == 0
        && stats.bufferGrowthsFromAudioThread == 0
        && stats.blockingLockAttempts == 0
        && stats.objectConstructionsFromAudioThread == 0;
}

bool realtimeCountersAreZero(const DjMasterBus& bus)
{
    const auto stats = bus.realtimeStats();
    return stats.allocationsFromAudioThread == 0
        && stats.bufferGrowthsFromAudioThread == 0
        && stats.blockingLockAttempts == 0
        && stats.invalidEndpointReads == 0
        && stats.staleGenerationReads == 0
        && stats.silentOversizedCallbacks == 0
        && stats.nonFiniteDeckBlocks == 0;
}

struct Timing {
    double averageUs = 0.0;
    double worstUs = 0.0;
};

Timing measure(DeckAudioGraph& graph, int blockSize, int iterations)
{
    juce::AudioBuffer<float> output(2, blockSize);
    double totalUs = 0.0;
    double worstUs = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        graph.getNextAudioBlock({&output, 0, blockSize});
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        totalUs += elapsed;
        worstUs = std::max(worstUs, elapsed);
    }
    return {totalUs / iterations, worstUs};
}

Timing measureDirectMixer(DeckAudioGraph& graph, int blockSize, int iterations)
{
    juce::AudioBuffer<float> output(2, blockSize);
    double totalUs = 0.0;
    double worstUs = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        graph.mixer().getNextAudioBlock({&output, 0, blockSize});
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        totalUs += elapsed;
        worstUs = std::max(worstUs, elapsed);
    }
    return {totalUs / iterations, worstUs};
}

bool waitForTimeStretchGeneration(DeckAudioGraph& graph, std::uint64_t oldGeneration)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    juce::AudioBuffer<float> output(2, 256);
    while (std::chrono::steady_clock::now() < deadline) {
        graph.getNextAudioBlock({&output, 0, 256});
        if (graph.timeStretch().activeConfigurationGeneration() > oldGeneration)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir directory;
    const auto mono44 = directory.filePath("mono-44.wav");
    const auto stereo96 = directory.filePath("stereo-96.wav");
    ok &= require(writeSampleRampWave(mono44, 44'100.0, 1),
                  "sample-coded ramp fixture");
    ok &= require(writeWave(stereo96, 96'000.0, 2, 330.0), "stereo fixture");

    AudioPageCache cache(4 * 1024 * 1024);
    DeckAudioGraph graph(cache);
    std::atomic<double> playhead {0.0};
    graph.setAudioPlayheadSink(&playhead);

    for (const auto [rate, size] : std::array{
             std::pair{44'100.0, 64}, std::pair{48'000.0, 512}, std::pair{96'000.0, 8192}}) {
        graph.prepareToPlay(size, rate);
        juce::AudioBuffer<float> output(2, size);
        output.clear();
        graph.getNextAudioBlock({&output, 0, size});
        ok &= require(isFinite(output), "empty graph output is finite");
        graph.releaseResources();
    }

    graph.prepareToPlay(8192, 48'000.0);
    auto trackA = cache.openTrack({mono44});
    ok &= require(waitForPage(cache, trackA), "track A cached");
    graph.installPreparedTrack({trackA, 44'100.0, 1});
    ok &= require(graph.playback() != nullptr, "track A installed");
    graph.setTransportRunning(true);

    for (const int size : {64, 128, 256, 512, 1024, 2048, 4096, 8192}) {
        juce::AudioBuffer<float> output(2, size);
        graph.getNextAudioBlock({&output, 0, size});
        ok &= require(isFinite(output), "normal playback output is finite");
    }

    juce::AudioBuffer<float> output(2, 512);
    const auto pausedReaderPosition = graph.playback()->getNextReadPosition();
    const auto pauseStart = std::chrono::steady_clock::now();
    graph.setTransportRunning(false);
    const auto pauseUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - pauseStart).count();
    for (int block = 0; block < 8; ++block)
        graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(pauseUs < 50'000.0,
                  "transport pause never waits for an audio callback");
    ok &= require(graph.playback()->getNextReadPosition() == pausedReaderPosition,
                  "paused playback gate does not advance the source reader");
    graph.transport().setPosition(0.2);
    graph.setTransportRunning(true);
    graph.setReverse(true);
    const auto reverseBefore = graph.playback()->getNextReadPosition();
    double reversePeak = 0.0;
    for (int block = 0; block < 32; ++block) {
        output.clear();
        graph.getNextAudioBlock({&output, 0, 512});
        reversePeak = std::max(reversePeak, absolutePeak(output));
    }
    ok &= require(isFinite(output), "pause, seek, reverse and loop output is finite");
    ok &= require(graph.playback()->getNextReadPosition() < reverseBefore,
                  "full graph reverse source position retreats");
    ok &= require(reversePeak > 1.0e-5,
                  "full graph reverse produces audible samples");
    graph.playback()->setLoopRangeSamples(1000, 5000, 44'100.0);

    graph.scratch().configureTrack(44'100.0, 1.0);
    graph.scratch().beginScratch(0.2, 44'100.0, 1.0, true, 1.0);
    graph.scratch().addTargetDeltaSeconds(0.04, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});
    const double scratchReleasePosition = graph.scratch().readPositionSeconds(44'100.0);
    ok &= require(std::abs(scratchReleasePosition - 0.2) > 1.0e-4,
                  "scratch cursor advances away from its grab position");
    (void) graph.scratch().endScratch(false);
    graph.scratch().prepareNormalPlaybackHandoff(scratchReleasePosition, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(isFinite(output), "scratch and playback handoff output is finite");
    ok &= require(std::abs(graph.scratch().readPositionSeconds(44'100.0) - scratchReleasePosition)
                      < 1.0e-6,
                  "scratch handoff keeps the final audio cursor");

    // FLX10 queues relative platter CCs and touch-up on the UI thread. Touch-up
    // can therefore be handled before the callback has rendered the final CCs.
    // The callback must render one final tracking block and capture its end cursor.
    graph.setReverse(false);
    graph.scratch().configureTrack(44'100.0, 1.0);
    graph.scratch().beginScratch(0.2, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    const double flx10CursorBeforeQueuedTicks = graph.scratch().readPositionSeconds(44'100.0);
    graph.scratch().submitHandDeltaSeconds(0.2, 0.016);
    graph.scratch().prepareNormalPlaybackHandoffFromScratchCursor(44'100.0);
    ok &= require(graph.scratch().normalPlaybackHandoffPending(),
                  "FLX10 release waits for the audio-thread cursor");
    graph.getNextAudioBlock({&output, 0, 512});
    const double flx10ReleaseCursor = graph.scratch().readPositionSeconds(44'100.0);
    ok &= require(flx10ReleaseCursor > flx10CursorBeforeQueuedTicks + 0.001,
                  "FLX10 release renders queued platter ticks before handoff");
    ok &= require(!graph.scratch().normalPlaybackHandoffPending(),
                  "FLX10 cursor handoff completes at the block boundary");
    ok &= require(std::abs(static_cast<double>(graph.playback()->getNextReadPosition()) / 44'100.0
                           - flx10ReleaseCursor) < 2.0 / 44'100.0,
                  "normal reader starts at the final FLX10 scratch sample");

    // The hardware path publishes a release command rather than a legacy
    // immediate handoff. The callback must render the queued touch block,
    // finish any coast, and seek the normal reader to that exact final cursor.
    const auto exerciseHardwareRelease = [&](double anchor,
                                             double releaseSpeed,
                                             engine::scratch::ScratchReleaseDisposition expected,
                                             const char* phaseMessage,
                                             const char* cursorMessage,
                                             bool wasPlaying = true) {
        graph.setReverse(false);
        graph.setPlaybackRate(1.0);
        graph.setKeylockEnabled(false);
        if (wasPlaying)
            graph.setTransportRunning(true);
        else
            graph.setTransportRunning(false);
        graph.scratch().configureTrack(44'100.0, 1.0);
        graph.scratch().beginScratch(anchor, 44'100.0, 1.0, wasPlaying, 1.0);
        graph.scratch().submitHandDeltaSeconds(
            releaseSpeed < 0.0 ? -0.025 : 0.025, 0.004);

        const auto releaseGeneration = graph.scratch().requestScratchRelease(
            releaseSpeed, true);
        ok &= require(releaseGeneration != 0,
                      "hardware release publishes a generation");
        ok &= require(graph.scratch().normalPlaybackHandoffPending(),
                      "hardware release keeps the normal reader masked");

        juce::AudioBuffer<float> transition(2, 64);
        transition.clear();
        graph.getNextAudioBlock({&transition, 0, 64});
        auto release = graph.scratch().scratchReleaseSnapshot();
        ok &= require(release.generation == releaseGeneration,
                      "audio callback acknowledges the hardware release generation");
        ok &= require(release.disposition == expected, phaseMessage);

        for (int block = 0;
             block < 4000 && !graph.scratch().scratchReleaseComplete(releaseGeneration);
             ++block) {
            transition.clear();
            graph.getNextAudioBlock({&transition, 0, 64});
        }

        release = graph.scratch().scratchReleaseSnapshot();
        ok &= require(graph.scratch().scratchReleaseComplete(releaseGeneration),
                      "hardware release completes on the audio thread");
        ok &= require(release.generation == releaseGeneration
                          && release.phase == engine::audio::ScratchReleasePhase::TailSuppression,
                      "reader handoff acknowledges its tail-suppression phase");
        ok &= require(std::abs(release.finalCursorSeconds
                               - graph.scratch().readPositionSeconds(44'100.0))
                          < 2.0 / 44'100.0,
                      cursorMessage);
        ok &= require(std::abs(static_cast<double>(graph.playback()->getNextReadPosition())
                                   / 44'100.0
                               - release.finalCursorSeconds)
                          < 2.0 / 44'100.0,
                      "hardware release directly seeks the cache reader");

        const std::array<float, 2> lastScratchSample {
            transition.getSample(0, transition.getNumSamples() - 1),
            transition.getSample(1, transition.getNumSamples() - 1)
        };
        transition.clear();
        graph.getNextAudioBlock({&transition, 0, 64});
        ok &= require(isFinite(transition),
                      "first normal block after hardware release is finite");
        ok &= require(absolutePeak(transition) <= 1.0,
                      "first normal block after hardware release is bounded");
        ok &= require(graph.transportSnapshot().running == wasPlaying,
                      "hardware release preserves the deck play state");
        for (int channel = 0; channel < 2; ++channel) {
            ok &= require(std::abs(transition.getSample(channel, 0)
                                   - lastScratchSample[static_cast<std::size_t>(channel)])
                              < 0.05f,
                          "first normal sample stays continuous with the scratch tail");
        }
    };

    exerciseHardwareRelease(
        0.30,
        1.5,
        engine::scratch::ScratchReleaseDisposition::CoastToDeckRate,
        "faster hardware release coasts to deck rate",
        "forward coast publishes its actual final cursor");
    exerciseHardwareRelease(
        0.40,
        0.7,
        engine::scratch::ScratchReleaseDisposition::HandoffNow,
        "slower hardware release hands off after its final tracking block",
        "immediate hardware handoff publishes its actual final cursor");
    exerciseHardwareRelease(
        0.60,
        -1.2,
        engine::scratch::ScratchReleaseDisposition::CoastToStop,
        "reverse hardware release coasts to stop",
        "reverse coast publishes its actual final cursor");
    exerciseHardwareRelease(
        0.50,
        1.4,
        engine::scratch::ScratchReleaseDisposition::CoastToStop,
        "paused hardware throw coasts to stop",
        "paused coast publishes its actual final cursor",
        false);

    graph.scratch().beginScratch(0.3, 44'100.0, 1.0, true, 1.0);
    const auto cancelledRelease = graph.scratch().requestScratchRelease(1.5, true);
    graph.scratch().beginScratch(0.4, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(graph.scratch().isScratching(),
                  "new grab supersedes a callback-owned hardware release");
    ok &= require(graph.scratch().scratchReleaseComplete(cancelledRelease),
                  "superseded hardware release is acknowledged as cancelled");
    ok &= require(std::abs(graph.scratch().readPositionSeconds(44'100.0) - 0.4) < 0.01,
                  "cancelled hardware release cannot seek away from the new grab");
    (void) graph.scratch().endScratch(false);
    graph.scratch().prepareNormalPlaybackHandoff(0.4, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});

    graph.scratch().beginScratch(0.3, 44'100.0, 1.0, true, 1.0);
    graph.scratch().prepareNormalPlaybackHandoffFromScratchCursor(44'100.0);
    graph.scratch().beginScratch(0.4, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(graph.scratch().isScratching(),
                  "rapid FLX10 re-grab cancels the pending release");
    ok &= require(std::abs(graph.scratch().readPositionSeconds(44'100.0) - 0.4) < 0.01,
                  "rapid FLX10 re-grab keeps the new grab cursor");
    (void) graph.scratch().endScratch(false);
    graph.scratch().prepareNormalPlaybackHandoff(0.4, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});

    {
        using engine::scratch::ScratchController;
        using engine::scratch::ScratchPhase;
        using engine::scratch::ScratchReleaseDisposition;
        ScratchController controller;
        controller.startScratch(0.0, true, 1.0);
        controller.setMeasuredNormalizedSpeed(1.4);
        ok &= require(controller.releaseScratch() == ScratchReleaseDisposition::CoastToDeckRate,
                      "faster forward release coasts to the deck rate");

        controller.startScratch(0.0, true, 1.0);
        controller.setMeasuredNormalizedSpeed(1.0);
        ok &= require(controller.releaseScratch() == ScratchReleaseDisposition::HandoffNow,
                      "matching deck speed hands off immediately");

        controller.startScratch(0.0, true, 1.0);
        controller.setMeasuredNormalizedSpeed(0.6);
        ok &= require(controller.releaseScratch() == ScratchReleaseDisposition::HandoffNow,
                      "slower forward release does not accelerate scratch audio");

        controller.startScratch(0.0, true, 1.0);
        controller.setMeasuredNormalizedSpeed(-0.8);
        ok &= require(controller.releaseScratch() == ScratchReleaseDisposition::CoastToStop,
                      "reverse release coasts to a stop before normal playback");

        controller.startScratch(0.0, true, 1.0);
        controller.setMeasuredNormalizedSpeed(1.5);
        ok &= require(controller.requestRelease(true),
                      "touch-up publishes a pending audio-thread release");
        ok &= require(controller.phase() == ScratchPhase::ReleasePending,
                      "touch-up keeps scratch active until the callback resolves it");
        ok &= require(controller.releaseScratchWithSpeed(1.5, true, 1.0, true)
                          == ScratchReleaseDisposition::CoastToDeckRate,
                      "audio-thread release uses the final rendered speed and deck snapshot");
        controller.submitReleaseSpeed(0.7);
        (void) controller.processAudioBlock(64, 48'000.0, 44'100.0);
        ok &= require(controller.phase() == ScratchPhase::HandoffPending,
                      "slower wheel tail hands ownership back on the audio thread");
        ok &= require(controller.isActive(),
                      "controller stays active until the reader handoff is consumed");
        ok &= require(controller.completeHandoff(),
                      "matching reader handoff completes the pending release");
        ok &= require(controller.phase() == ScratchPhase::Idle,
                      "completed reader handoff returns the controller to idle");
        controller.startScratch(0.0, true, 1.0);
        ok &= require(!controller.completeHandoff()
                          && controller.phase() == ScratchPhase::TouchTracking,
                      "an old handoff cannot stop a concurrent re-grab");
        controller.stopScratch();
    }

    // When inertia finishes, the normal reader is still at its pre-scratch
    // position until the facade publishes the blockwise handoff. The bridge
    // must keep serving the scratch path during that short interval.
    graph.setReverse(false);
    graph.setKeylockEnabled(false);
    graph.playback()->clearLoopRangeSamples();
    graph.scratch().configureTrack(44'100.0, 1.0);
    graph.scratch().beginScratch(0.2, 44'100.0, 1.0, true, 1.0);
    graph.scratch().addTargetDeltaSeconds(0.2, 44'100.0);
    juce::AudioBuffer<float> releaseTransition(2, 64);
    graph.getNextAudioBlock({&releaseTransition, 0, 64});
    const auto releaseDisposition = graph.scratch().endScratch(true);
    ok &= require(releaseDisposition == engine::scratch::ScratchReleaseDisposition::CoastToDeckRate,
                  "fast scratch release starts inertia");
    for (int block = 0; block < 2000 && graph.scratch().isInertiaActive(); ++block)
        graph.getNextAudioBlock({&releaseTransition, 0, 64});
    ok &= require(!graph.scratch().isInertiaActive(), "scratch inertia reaches deck rate");
    const auto normalReaderBeforeHandoff = graph.playback()->getNextReadPosition();
    graph.getNextAudioBlock({&releaseTransition, 0, 64});
    ok &= require(graph.playback()->getNextReadPosition() == normalReaderBeforeHandoff,
                  "completed inertia cannot expose the stale normal reader");
    const double pendingHandoffPosition = graph.scratch().readPositionSeconds(44'100.0);
    graph.scratch().prepareNormalPlaybackHandoff(pendingHandoffPosition, 44'100.0);
    graph.getNextAudioBlock({&releaseTransition, 0, 64});
    ok &= require(isFinite(releaseTransition), "deferred scratch handoff output is finite");

    // Exercise callback-owned scratch entry/exit commands across the device
    // block sizes we support. A full cache is already present, so starvation
    // here would indicate a handoff/read-head regression rather than loading.
    const auto starvationBeforeScratchStress = graph.scratch().scratchCacheStats().starvationBlocks;
    for (const bool reverse : {false, true}) {
        graph.setReverse(reverse);
        for (const bool keylock : {false, true}) {
            graph.setKeylockEnabled(keylock);
            for (const int size : {64, 128, 256, 512, 1024, 2048, 4096, 8192}) {
                juce::AudioBuffer<float> transition(2, size);
                for (int cycle = 0; cycle < 3; ++cycle) {
                    const double anchor = 0.25 + 0.03 * cycle;
                    graph.scratch().configureTrack(44'100.0, 1.0);
                    graph.scratch().beginScratch(anchor, 44'100.0, 1.0, true,
                                                 reverse ? -1.0 : 1.0);
                    graph.scratch().addTargetDeltaSeconds(reverse ? -0.015 : 0.015, 44'100.0);
                    graph.getNextAudioBlock({&transition, 0, size});
                    ok &= require(isFinite(transition), "scratch stress output is finite");
                    (void) graph.scratch().endScratch(false);
                    graph.scratch().prepareNormalPlaybackHandoff(anchor, 44'100.0);
                    graph.getNextAudioBlock({&transition, 0, size});
                    ok &= require(isFinite(transition), "scratch stress handoff is finite");
                    ok &= require(absolutePeak(transition) <= 1.0,
                                  "scratch handoff transition remains bounded");
                }
            }
        }
    }
    graph.setReverse(false);
    graph.setKeylockEnabled(false);
    ok &= require(graph.scratch().scratchCacheStats().starvationBlocks == starvationBeforeScratchStress,
                  "scratch handoff has no cache starvation after preload");

    graph.timeStretch().setPitchLockEnabled(true);
    graph.timeStretch().setTempoRatio(0.8);
    graph.mixer().setEq(-1.0f, 0.5f, 1.0f);
    graph.mixer().setFilterVal(-0.5f);
    graph.mixer().setTrim(0.7f);
    graph.mixer().setFader(0.8f);
    for (int i = 0; i < 100; ++i)
        graph.getNextAudioBlock({&output, 0, 512});

    auto trackB = cache.openTrack({stereo96});
    ok &= require(waitForPage(cache, trackB), "track B cached");
    graph.installPreparedTrack({trackB, 96'000.0, 2});
    graph.setTransportRunning(true);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(isFinite(output), "mono to stereo handover output is finite");

    graph.setReverse(false);
    graph.setPlaybackRate(1.0);
    graph.setKeylockEnabled(true);
    graph.scratch().configureTrack(96'000.0, 0.5);
    graph.scratch().beginScratch(0.1, 96'000.0, 0.5, true, 1.0);
    graph.scratch().submitHandDeltaSeconds(0.01, 0.002);
    const auto release96 = graph.scratch().requestScratchRelease(0.5, true);
    graph.getNextAudioBlock({&output, 0, 512});
    const auto snapshot96 = graph.scratch().scratchReleaseSnapshot();
    ok &= require(graph.scratch().scratchReleaseComplete(release96),
                  "96 kHz hardware release completes at a 48 kHz callback rate");
    ok &= require(std::abs(static_cast<double>(graph.playback()->getNextReadPosition())
                               / 96'000.0
                           - snapshot96.finalCursorSeconds)
                      < 2.0 / 96'000.0,
                  "96/48 hardware handoff seeks the reader within two source samples");
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(isFinite(output),
                  "96/48 keylock normal block after hardware release is finite");
    graph.setKeylockEnabled(false);

    auto staleA = cache.openTrack({mono44});
    graph.installPreparedTrack({staleA, 44'100.0, 1});
    ok &= require(graph.playback() != nullptr, "stale generation did not clear active track");

    for (std::uint64_t generation = 3; generation <= 6; ++generation) {
        const bool useA = generation % 2 != 0;
        auto handle = cache.openTrack({useA ? mono44 : stereo96});
        graph.installPreparedTrack({handle, useA ? 44'100.0 : 96'000.0, generation});
        graph.setTransportRunning(true);
        graph.getNextAudioBlock({&output, 0, 512});
        ok &= require(isFinite(output), "rapid track handover output is finite");
    }

    auto handoverHandle = cache.openTrack({mono44});
    const auto handoverStart = std::chrono::steady_clock::now();
    graph.installPreparedTrack({handoverHandle, 44'100.0, 7});
    const auto handoverUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - handoverStart).count();
    ok &= require(handoverUs < 100'000.0,
                  "track handover cannot enter JUCE's one-second stop wait");

    ok &= require(realtimeCountersAreZero(graph), "all realtime violation counters are zero");

    auto configurationGeneration = graph.timeStretch().activeConfigurationGeneration();
    graph.timeStretch().setPitchLockEnabled(false);
    graph.timeStretch().setTempoRatio(1.0);
    ok &= require(waitForTimeStretchGeneration(graph, configurationGeneration),
                  "neutral time-stretch pipeline prepared");
    graph.mixer().setEq(0.0f, 0.0f, 0.0f);
    graph.mixer().setFilterVal(0.0f);
    const auto directMixer512 = measureDirectMixer(graph, 512, 300);
    const auto neutral512 = measure(graph, 512, 300);
    configurationGeneration = graph.timeStretch().activeConfigurationGeneration();
    graph.timeStretch().setPitchLockEnabled(true);
    graph.timeStretch().setTempoRatio(0.8);
    ok &= require(waitForTimeStretchGeneration(graph, configurationGeneration),
                  "keylock pipeline prepared");
    const auto keylock512 = measure(graph, 512, 100);
    configurationGeneration = graph.timeStretch().activeConfigurationGeneration();
    graph.timeStretch().setPitchLockEnabled(false);
    ok &= require(waitForTimeStretchGeneration(graph, configurationGeneration),
                  "post-keylock bypass prepared");
    graph.mixer().setEq(-1.0f, 0.5f, 1.0f);
    graph.mixer().setFilterVal(-0.5f);
    const auto eqFilter8192 = measure(graph, 8192, 100);
    graph.scratch().configureTrack(96'000.0, 0.5);
    graph.scratch().beginScratch(0.1, 96'000.0, 0.5, true, 1.0);
    graph.scratch().addTargetDeltaSeconds(0.03, 96'000.0);
    const auto scratch512 = measure(graph, 512, 100);
    (void) graph.scratch().endScratch(false);
    std::cout << "Pre-refactor-equivalent direct Mixer 512: avg=" << directMixer512.averageUs
              << " us worst=" << directMixer512.worstUs << " us\n";
    std::cout << "DeckAudioGraph neutral 512: avg=" << neutral512.averageUs
              << " us worst=" << neutral512.worstUs << " us\n";
    std::cout << "DeckAudioGraph keylock 512: avg=" << keylock512.averageUs
              << " us worst=" << keylock512.worstUs << " us\n";
    std::cout << "DeckAudioGraph scratch 512: avg=" << scratch512.averageUs
              << " us worst=" << scratch512.worstUs << " us\n";
    std::cout << "DeckAudioGraph EQ/filter 8192: avg=" << eqFilter8192.averageUs
              << " us worst=" << eqFilter8192.worstUs << " us\n";
    std::cout << "DeckAudioGraph track handover: " << handoverUs << " us\n";

    std::vector<std::unique_ptr<DeckAudioGraph>> decks;
    decks.reserve(4);
    for (int index = 0; index < 4; ++index) {
        auto deck = std::make_unique<DeckAudioGraph>(cache);
        deck->prepareToPlay(8192, 48'000.0);
        auto handle = cache.openTrack({index % 2 == 0 ? mono44 : stereo96});
        deck->installPreparedTrack({handle, index % 2 == 0 ? 44'100.0 : 96'000.0,
                                    static_cast<std::uint64_t>(index + 1)});
        deck->setTransportRunning(true);
        deck->timeStretch().setPitchLockEnabled(index % 2 != 0);
        deck->timeStretch().setTempoRatio(0.85 + index * 0.05);
        deck->mixer().setEq(-0.5f, 0.25f, 0.5f);
        deck->mixer().setFilterVal(index % 2 == 0 ? -0.4f : 0.4f);
        if (index == 0) {
            deck->scratch().configureTrack(44'100.0, 1.0);
            deck->scratch().beginScratch(0.1, 44'100.0, 1.0, true, 1.0);
            deck->scratch().addTargetDeltaSeconds(0.02, 44'100.0);
        }
        decks.push_back(std::move(deck));
    }

    double fourDeckTotalUs = 0.0;
    double fourDeckWorstUs = 0.0;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        for (auto& deck : decks) {
            output.clear();
            deck->getNextAudioBlock({&output, 0, 512});
            ok &= require(isFinite(output), "four-deck stress output is finite");
        }
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        fourDeckTotalUs += elapsed;
        fourDeckWorstUs = std::max(fourDeckWorstUs, elapsed);
    }
    std::cout << "DeckAudioGraph four decks/512: avg=" << fourDeckTotalUs / 100.0
              << " us worst=" << fourDeckWorstUs << " us\n";

    DjMasterBus masterBus;
    std::array<DjMasterBus::DeckRegistration, 4> registrations;
    for (int index = 0; index < 4; ++index) {
        decks[index]->transport().setPosition(0.0);
        decks[index]->setTransportRunning(true);
        decks[index]->setCueEnabledForMix(index % 2 == 0);
        registrations[index] = masterBus.registerDeck(*decks[index], index);
        ok &= require(registrations[index].isValid(), "real graph registered with master bus");
    }
    DjMasterBus::setOutputRouting(1, 3, 5);
    DjMasterBus::setMasterVolume(0.7f);
    DjMasterBus::setHeadphoneMix(0.25f);
    DjMasterBus::setMasterCueEnabled(true);
    DjMasterBus::setAntiClipEnabled(true);
    masterBus.prepareToPlay(16'384, 48'000.0);
    masterBus.resetRealtimeStats();
    {
        juce::AudioBuffer<float> limiterWarmup(6, 512);
        masterBus.getNextAudioBlock({&limiterWarmup, 0, 512});
    }

    double masterBusTotalUs = 0.0;
    double masterBusWorstUs = 0.0;
    std::mt19937 stressRandom(0xB40CD1u);
    std::uniform_real_distribution<float> controlValue(0.0f, 1.0f);
    std::array<std::uint64_t, 4> stressTrackGenerations {20, 20, 20, 20};
    constexpr std::array callbackSizes {64, 128, 256, 512, 1024, 2048,
                                        4096, 8192, 16'384};
    for (int iteration = 0; iteration < 72; ++iteration) {
        const int blockSize = callbackSizes[static_cast<std::size_t>(iteration)
                                            % callbackSizes.size()];
        const int selectedDeck = static_cast<int>(stressRandom() % decks.size());
        if (iteration % 9 == 0) {
            for (auto& deck : decks) {
                deck->transport().setPosition(0.0);
                deck->setTransportRunning(true);
            }
        }
        if (iteration % 12 == 3)
            decks[selectedDeck]->setTransportRunning(false);
        if (iteration % 12 == 4)
            decks[selectedDeck]->setTransportRunning(true);
        if (iteration % 18 == 6) {
            auto& deck = *decks[selectedDeck];
            auto replacement = cache.openTrack({iteration % 2 == 0 ? mono44 : stereo96});
            deck.installPreparedTrack({replacement, iteration % 2 == 0 ? 44'100.0 : 96'000.0,
                                       ++stressTrackGenerations[selectedDeck]});
            deck.setTransportRunning(true);
        }
        if (iteration % 16 == 5) {
            auto& scratchDeck = *decks[selectedDeck];
            scratchDeck.scratch().configureTrack(48'000.0, 1.0);
            scratchDeck.scratch().beginScratch(0.1, 48'000.0, 1.0, true, 1.0);
            scratchDeck.scratch().addTargetDeltaSeconds(0.01, 48'000.0);
        }
        if (iteration % 16 == 7)
            (void) decks[selectedDeck]->scratch().endScratch(false);
        if (iteration % 10 == 2) {
            decks[selectedDeck]->timeStretch().setPitchLockEnabled(iteration % 20 == 2);
            decks[selectedDeck]->timeStretch().setTempoRatio(0.85 + 0.2 * controlValue(stressRandom));
        }
        if (iteration % 14 == 8) {
            decks[selectedDeck]->mixer().setFxEffectType(EffectType::Bitcrusher);
            decks[selectedDeck]->mixer().setFxAmount(0.15f + 0.2f * controlValue(stressRandom));
        } else if (iteration % 14 == 10) {
            decks[selectedDeck]->mixer().setFxEffectType(EffectType::None);
        }
        // DjEngine normally maps the crossfader curve/assignment to these post-fader gains.
        decks[iteration % 4]->mixer().setFader(0.2f + 0.8f * controlValue(stressRandom));
        decks[(iteration + 1) % 4]->mixer().setFilterVal(
            -0.5f + controlValue(stressRandom));
        decks[(iteration + 1) % 4]->mixer().setEq(-0.5f + controlValue(stressRandom),
                                                  -0.5f + controlValue(stressRandom),
                                                  -0.5f + controlValue(stressRandom));
        decks[(iteration + 2) % 4]->setCueEnabledForMix(iteration % 3 != 0);
        if (iteration == 36) {
            registrations[3].reset();
            registrations[3] = masterBus.registerDeck(*decks[3], 3);
            ok &= require(registrations[3].isValid(),
                          "registration swap succeeds during active-device stress");
        }

        juce::AudioBuffer<float> masterOutput(6, blockSize);
        const auto start = std::chrono::steady_clock::now();
        masterBus.getNextAudioBlock({&masterOutput, 0, blockSize});
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        masterBusTotalUs += elapsed;
        masterBusWorstUs = std::max(masterBusWorstUs, elapsed);
        ok &= require(isFinite(masterOutput), "four-graph master-bus output is finite");
        ok &= require(masterOutput.getMagnitude(0, 0, blockSize) > 0.00001f,
                      "four-graph master-bus output is non-silent");
        if (blockSize > DjMasterBus::kProcessingChunkSize) {
            const int tailStart = blockSize - std::min(64, blockSize);
            ok &= require(masterOutput.getMagnitude(0, tailStart, blockSize - tailStart)
                              > 0.00001f,
                          "chunked master-bus tail is non-silent");
        }
    }

    registrations[2].reset();
    {
        juce::AudioBuffer<float> retirementOutput(6, 4096);
        masterBus.getNextAudioBlock({&retirementOutput, 0, 4096});
        ok &= require(isFinite(retirementOutput), "master bus survives deck retirement");
    }
    registrations[2] = masterBus.registerDeck(*decks[2], 2);
    ok &= require(registrations[2].isValid(), "retired real graph can be re-registered");
    {
        juce::AudioBuffer<float> replacementOutput(6, 16'384);
        masterBus.getNextAudioBlock({&replacementOutput, 0, 16'384});
        ok &= require(isFinite(replacementOutput), "replacement generation stays finite");
    }
    const auto masterStats = masterBus.realtimeStats();
    ok &= require(masterStats.oversizedCallbacks > 0,
                  "large callbacks use the chunked master-bus path");
    ok &= require(realtimeCountersAreZero(masterBus),
                  "master-bus realtime violation counters are zero");
    std::cout << "DjMasterBus four real graphs/mixed blocks: avg="
              << masterBusTotalUs / 72.0 << " us worst=" << masterBusWorstUs << " us\n";

    for (auto& registration : registrations)
        registration.reset();
    masterBus.beginShutdown();
    masterBus.releaseResources();

    for (auto& deck : decks) {
        ok &= require(realtimeCountersAreZero(*deck), "four-deck realtime counters are zero");
        deck->clearTrack(100);
        deck->releaseResources();
    }
    decks.clear();

    graph.clearTrack(10);
    output.clear();
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(isFinite(output), "cleared graph output is finite");
    auto invalidated = cache.openTrack({mono44});
    graph.installPreparedTrack({invalidated, 44'100.0, 9});
    ok &= require(graph.playback() == nullptr, "invalidated pending generation rejected");
    ok &= require(realtimeCountersAreZero(graph), "final realtime counters are zero");
    graph.releaseResources();
    return ok ? 0 : 1;
}
