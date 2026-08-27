#include "audio/DeckAudioPipeline.h"
#include "audio/AudioEngine.h"

#include "audio/cache/AudioPageCache.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "audio/DeckChannelProcessor.h"
#include "audio/RenderModeRouter.h"
#include "audio/TimeStretchProcessor.h"
#include "controllers/flx10/Flx10RealtimeScratchIngress.h"
#include "deck/scratch/ScratchController.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

bool realtimeCountersAreZero(DeckAudioPipeline& graph)
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

bool realtimeCountersAreZero(const AudioEngine& bus)
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

Timing measure(DeckAudioPipeline& graph, int blockSize, int iterations)
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

Timing measureDirectMixer(DeckAudioPipeline& graph, int blockSize, int iterations)
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
    DeckAudioPipeline graph(cache);
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

    graph.renderModeRouter().configureTrack(44'100.0, 1.0);
    graph.renderModeRouter().beginScratch(0.2, 44'100.0, 1.0, true, 1.0);
    graph.renderModeRouter().addTargetDeltaSeconds(0.04, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});
    const double scratchReleasePosition = graph.renderModeRouter().readPositionSeconds(44'100.0);
    ok &= require(std::abs(scratchReleasePosition - 0.2) > 1.0e-4,
                  "scratch cursor advances away from its grab position");
    (void) graph.renderModeRouter().endScratch(false);
    graph.renderModeRouter().prepareNormalPlaybackHandoff(scratchReleasePosition, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(isFinite(output), "scratch and playback handoff output is finite");
    ok &= require(std::abs(graph.renderModeRouter().readPositionSeconds(44'100.0) - scratchReleasePosition)
                      < 1.0e-6,
                  "scratch handoff keeps the final audio cursor");

    // FLX10 queues relative platter CCs and touch-up on the UI thread. Touch-up
    // can therefore be handled before the callback has rendered the final CCs.
    // The callback must render one final tracking block and capture its end cursor.
    graph.setReverse(false);
    graph.renderModeRouter().configureTrack(44'100.0, 1.0);
    graph.renderModeRouter().beginScratch(0.2, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    const double flx10CursorBeforeQueuedTicks = graph.renderModeRouter().readPositionSeconds(44'100.0);
    graph.renderModeRouter().submitHandDeltaSeconds(0.2, 0.016);
    graph.renderModeRouter().prepareNormalPlaybackHandoffFromScratchCursor(44'100.0);
    ok &= require(graph.renderModeRouter().normalPlaybackHandoffPending(),
                  "FLX10 release waits for the audio-thread cursor");
    graph.getNextAudioBlock({&output, 0, 512});
    const double flx10ReleaseCursor = graph.renderModeRouter().readPositionSeconds(44'100.0);
    ok &= require(flx10ReleaseCursor > flx10CursorBeforeQueuedTicks + 0.001,
                  "FLX10 release renders queued platter ticks before handoff");
    ok &= require(!graph.renderModeRouter().normalPlaybackHandoffPending(),
                  "FLX10 cursor handoff completes at the block boundary");
    ok &= require(std::abs(static_cast<double>(graph.playback()->getNextReadPosition()) / 44'100.0
                           - flx10ReleaseCursor) < 2.0 / 44'100.0,
                  "normal reader starts at the final FLX10 scratch sample");

    // Native FLX10 motion must not depend on Qt draining its event queue. Model
    // a busy UI by publishing physical packets only to the realtime ingress: no
    // submitHandDeltaSeconds/addTargetDeltaSeconds call is made between blocks.
    flx10::Flx10RealtimeScratchIngress realtimeIngress;
    graph.renderModeRouter().setRealtimeScratchInput(realtimeIngress.stream());
    graph.setReverse(false);
    graph.setTransportRunning(true);
    graph.renderModeRouter().configureTrack(44'100.0, 1.0);
    const double realtimeStart =
        engine::scratch::RealtimeScratchInput::clockSeconds() - 0.010;
    (void) realtimeIngress.touchDown(realtimeStart, 1);
    graph.renderModeRouter().beginScratch(0.30, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    const double realtimeAnchor =
        graph.renderModeRouter().readPositionSeconds(44'100.0);
    for (int packet = 1; packet <= 6; ++packet) {
        (void) realtimeIngress.platter(
            7.0, realtimeStart + 0.001 * packet, 1);
    }
    for (int block = 0; block < 4; ++block)
        graph.getNextAudioBlock({&output, 0, 128});
    const double realtimeForward =
        graph.renderModeRouter().readPositionSeconds(44'100.0);
    ok &= require(realtimeForward > realtimeAnchor + 0.001,
                  "native FLX10 trajectory advances while the UI path is stalled");

    const double reverseStart =
        engine::scratch::RealtimeScratchInput::clockSeconds() - 0.010;
    for (int packet = 0; packet < 10; ++packet) {
        (void) realtimeIngress.platter(
            -7.0, reverseStart + 0.001 * packet, 1);
    }
    bool renderedReverse = false;
    for (int block = 0; block < 12; ++block) {
        graph.getNextAudioBlock({&output, 0, 128});
        renderedReverse = renderedReverse
            || graph.renderModeRouter().scratchRate() < -0.01;
    }
    const double realtimeReverse =
        graph.renderModeRouter().readPositionSeconds(44'100.0);
    ok &= require(renderedReverse && realtimeReverse < realtimeForward,
                  "native FLX10 reversal reaches audio without a UI callback");

    (void) realtimeIngress.touchUp(reverseStart + 0.011, 1);
    const auto nativeRelease = graph.renderModeRouter().requestScratchRelease(
        0.7, true, true); // deliberately contradict the physical reverse throw
    juce::AudioBuffer<float> nativeReleaseOutput(2, 64);
    graph.getNextAudioBlock({&nativeReleaseOutput, 0, 64});
    ok &= require(graph.renderModeRouter().scratchReleaseSnapshot().disposition
                      == engine::scratch::ScratchReleaseDisposition::CoastToStop,
                  "native FLX10 throw velocity owns the release decision");
    for (int block = 0;
         block < 4000
             && !graph.renderModeRouter().scratchReleaseComplete(nativeRelease);
         ++block) {
        graph.getNextAudioBlock({&nativeReleaseOutput, 0, 64});
    }
    ok &= require(graph.renderModeRouter().scratchReleaseComplete(nativeRelease),
                  "native FLX10 release completes on the audio thread");
    graph.renderModeRouter().setRealtimeScratchInput(nullptr);

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
        graph.renderModeRouter().configureTrack(44'100.0, 1.0);
        graph.renderModeRouter().beginScratch(anchor, 44'100.0, 1.0, wasPlaying, 1.0);
        graph.renderModeRouter().submitHandDeltaSeconds(
            releaseSpeed < 0.0 ? -0.025 : 0.025, 0.004);

        const auto releaseGeneration = graph.renderModeRouter().requestScratchRelease(
            releaseSpeed, true, wasPlaying);
        ok &= require(releaseGeneration != 0,
                      "hardware release publishes a generation");
        ok &= require(graph.renderModeRouter().normalPlaybackHandoffPending(),
                      "hardware release keeps the normal reader masked");

        juce::AudioBuffer<float> transition(2, 64);
        transition.clear();
        graph.getNextAudioBlock({&transition, 0, 64});
        auto release = graph.renderModeRouter().scratchReleaseSnapshot();
        ok &= require(release.generation == releaseGeneration,
                      "audio callback acknowledges the hardware release generation");
        ok &= require(release.disposition == expected, phaseMessage);

        for (int block = 0;
             block < 4000 && !graph.renderModeRouter().scratchReleaseComplete(releaseGeneration);
             ++block) {
            transition.clear();
            graph.getNextAudioBlock({&transition, 0, 64});
        }

        release = graph.renderModeRouter().scratchReleaseSnapshot();
        ok &= require(graph.renderModeRouter().scratchReleaseComplete(releaseGeneration),
                      "hardware release completes on the audio thread");
        ok &= require(release.generation == releaseGeneration
                          && release.phase == engine::audio::ScratchReleasePhase::TailSuppression,
                      "reader handoff acknowledges its tail-suppression phase");
        ok &= require(std::abs(release.finalCursorSeconds
                               - graph.renderModeRouter().readPositionSeconds(44'100.0))
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

    // Regression: the play state at grab time is historical context only. A
    // newer explicit Pause must determine release behaviour and must never be
    // replaced by the pre-scratch Playing state.
    graph.setTransportRunning(false);
    graph.renderModeRouter().configureTrack(44'100.0, 1.0);
    graph.renderModeRouter().beginScratch(0.45, 44'100.0, 1.0, true, 1.0);
    graph.renderModeRouter().submitHandDeltaSeconds(0.025, 0.004);
    const auto pausedDuringScratchRelease =
        graph.renderModeRouter().requestScratchRelease(1.4, true, false);
    juce::AudioBuffer<float> pausedReleaseOutput(2, 64);
    graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    ok &= require(graph.renderModeRouter().scratchReleaseSnapshot().disposition
                      == engine::scratch::ScratchReleaseDisposition::CoastToStop,
                  "current Pause intent overrides the play state captured at scratch begin");
    for (int block = 0;
         block < 4000
             && !graph.renderModeRouter().scratchReleaseComplete(pausedDuringScratchRelease);
         ++block) {
        graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    }
    ok &= require(graph.renderModeRouter().scratchReleaseComplete(pausedDuringScratchRelease)
                      && !graph.transportSnapshot().running,
                  "scratch release after Pause leaves normal transport paused");

    // Pause during a running throw changes the coast target to zero. It must
    // preserve momentum instead of cancelling the callback-owned release.
    graph.setTransportRunning(true);
    graph.renderModeRouter().beginScratch(0.35, 44'100.0, 1.0, true, 1.0);
    graph.renderModeRouter().submitHandDeltaSeconds(0.025, 0.004);
    const auto retargetedByPauseRelease =
        graph.renderModeRouter().requestScratchRelease(1.5, true, true);
    graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    ok &= require(graph.renderModeRouter().scratchReleaseSnapshot().phase
                      == engine::audio::ScratchReleasePhase::CoastToDeck,
                  "playing scratch release enters coast before Pause");
    const double pausedCursor = graph.renderModeRouter().readPositionSeconds(44'100.0);
    graph.setTransportRunning(false);
    graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    const auto pauseRetarget = graph.renderModeRouter().scratchReleaseSnapshot();
    ok &= require(pauseRetarget.phase
                      == engine::audio::ScratchReleasePhase::CoastToStop
                      && graph.renderModeRouter().isInertiaActive(),
                  "Pause retargets active release inertia toward zero");
    ok &= require(graph.renderModeRouter().readPositionSeconds(44'100.0) > pausedCursor,
                  "Pause preserves forward platter momentum after the command");
    for (int block = 0;
         block < 4000
             && !graph.renderModeRouter().scratchReleaseComplete(retargetedByPauseRelease);
         ++block) {
        graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    }
    ok &= require(graph.renderModeRouter().scratchReleaseComplete(retargetedByPauseRelease)
                      && !graph.renderModeRouter().isInertiaActive()
                      && !graph.transportSnapshot().running,
                  "Pause lets active release coast fully to zero and remain stopped");

    // Starting from a paused deck, Play during the spin must update the active
    // coast and hand off at deck rate without restarting from the grab cursor.
    graph.setTransportRunning(false);
    graph.renderModeRouter().beginScratch(0.25, 44'100.0, 1.0, false, 1.0);
    graph.renderModeRouter().submitHandDeltaSeconds(0.025, 0.004);
    const auto retargetedByPlayRelease =
        graph.renderModeRouter().requestScratchRelease(1.5, true, false);
    graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    ok &= require(graph.renderModeRouter().scratchReleaseSnapshot().phase
                      == engine::audio::ScratchReleasePhase::CoastToStop,
                  "paused platter spin initially coasts toward zero");
    const double playCursor = graph.renderModeRouter().readPositionSeconds(44'100.0);
    graph.setTransportRunning(true);
    graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    ok &= require(graph.renderModeRouter().scratchReleaseSnapshot().phase
                      == engine::audio::ScratchReleasePhase::CoastToDeck,
                  "Play retargets a same-direction spin toward live deck rate");
    for (int block = 0;
         block < 4000
             && !graph.renderModeRouter().scratchReleaseComplete(retargetedByPlayRelease);
         ++block) {
        graph.getNextAudioBlock({&pausedReleaseOutput, 0, 64});
    }
    ok &= require(graph.renderModeRouter().scratchReleaseComplete(retargetedByPlayRelease)
                      && graph.transportSnapshot().running
                      && graph.renderModeRouter().readPositionSeconds(44'100.0) > playCursor,
                  "Play completes the moving handoff without rewinding the spin");

    graph.renderModeRouter().beginScratch(0.3, 44'100.0, 1.0, true, 1.0);
    const auto cancelledRelease = graph.renderModeRouter().requestScratchRelease(
        1.5, true, true);
    graph.renderModeRouter().beginScratch(0.4, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(graph.renderModeRouter().isScratching(),
                  "new grab supersedes a callback-owned hardware release");
    ok &= require(graph.renderModeRouter().scratchReleaseComplete(cancelledRelease),
                  "superseded hardware release is acknowledged as cancelled");
    ok &= require(std::abs(graph.renderModeRouter().readPositionSeconds(44'100.0) - 0.4) < 0.01,
                  "cancelled hardware release cannot seek away from the new grab");
    (void) graph.renderModeRouter().endScratch(false);
    graph.renderModeRouter().prepareNormalPlaybackHandoff(0.4, 44'100.0);
    graph.getNextAudioBlock({&output, 0, 512});

    graph.renderModeRouter().beginScratch(0.3, 44'100.0, 1.0, true, 1.0);
    graph.renderModeRouter().prepareNormalPlaybackHandoffFromScratchCursor(44'100.0);
    graph.renderModeRouter().beginScratch(0.4, 44'100.0, 1.0, true, 1.0);
    graph.getNextAudioBlock({&output, 0, 512});
    ok &= require(graph.renderModeRouter().isScratching(),
                  "rapid FLX10 re-grab cancels the pending release");
    ok &= require(std::abs(graph.renderModeRouter().readPositionSeconds(44'100.0) - 0.4) < 0.01,
                  "rapid FLX10 re-grab keeps the new grab cursor");
    (void) graph.renderModeRouter().endScratch(false);
    graph.renderModeRouter().prepareNormalPlaybackHandoff(0.4, 44'100.0);
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
        double lastBackspinRate = 0.0;
        for (int block = 0; block < 2000
             && controller.phase() != ScratchPhase::HandoffPending; ++block) {
            lastBackspinRate = controller.processAudioBlock(64, 48'000.0, 44'100.0);
        }
        ok &= require(controller.phase() == ScratchPhase::HandoffPending
                          && lastBackspinRate < -0.04,
                      "playing backspin hands off with reverse momentum instead of a zero-speed gap");

        controller.startScratch(0.0, false, 1.0);
        controller.setMeasuredNormalizedSpeed(-0.8);
        ok &= require(controller.releaseScratch() == ScratchReleaseDisposition::CoastToStop,
                      "paused reverse throw still coasts toward a true stop");
        double pausedBackspinRate = -1.0;
        for (int block = 0; block < 2000
             && controller.phase() != ScratchPhase::HandoffPending; ++block) {
            pausedBackspinRate = controller.processAudioBlock(64, 48'000.0, 44'100.0);
        }
        ok &= require(controller.phase() == ScratchPhase::HandoffPending
                          && std::abs(pausedBackspinRate) < 1.0e-9,
                      "paused backspin keeps the full coast-to-zero behaviour");

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

    {
        using engine::scratch::ScratchController;
        using engine::scratch::ScratchReleaseDisposition;
        ScratchController controller;
        controller.startScratch(0.0, true, 1.0);
        // A steady on-screen forward drag: no measured rate, so every event
        // estimates speed as deltaTrackSec/dtSec.
        for (int i = 0; i < 10; ++i)
            controller.submitHandDelta(0.032, 0.016);
        ok &= require(controller.smoothedSpeed() > 1.5,
                      "steady forward drag settles near its true delta/dt speed");

        // A near-zero-time UI sample right before release — e.g. the render
        // thread catching up from a stall, or the final move immediately
        // before button-up — must not divide a tiny position jitter by a
        // tiny interval and manufacture a large wrong-direction speed that
        // then becomes the release direction.
        controller.submitHandDelta(-0.00005, 1.0e-6);
        ok &= require(controller.smoothedSpeed() > 0.0
                          && controller.smoothedSpeed() < 1.5,
                      "a near-zero-dt jitter sample damps toward, but cannot flip, direction");
        ok &= require(controller.releaseScratch() != ScratchReleaseDisposition::CoastToStop,
                      "release right after that jitter sample still coasts toward the deck, not backward");
        controller.stopScratch();
    }

    // When inertia finishes, the normal reader is still at its pre-scratch
    // position until the facade publishes the blockwise handoff. The bridge
    // must keep serving the scratch path during that short interval.
    graph.setReverse(false);
    graph.setKeylockEnabled(false);
    graph.playback()->clearLoopRangeSamples();
    graph.renderModeRouter().configureTrack(44'100.0, 1.0);
    graph.renderModeRouter().beginScratch(0.2, 44'100.0, 1.0, true, 1.0);
    // A fast physical move carries both exact position and measured velocity.
    // A position-only teleport is deliberately acceleration-bounded by the
    // tracker and is not a valid throw-speed fixture.
    graph.renderModeRouter().submitHandDeltaSeconds(0.2, 0.020, 8.0);
    juce::AudioBuffer<float> releaseTransition(2, 64);
    graph.getNextAudioBlock({&releaseTransition, 0, 64});
    const auto releaseDisposition = graph.renderModeRouter().endScratch(true);
    ok &= require(releaseDisposition == engine::scratch::ScratchReleaseDisposition::CoastToDeckRate,
                  "fast scratch release starts inertia");
    for (int block = 0; block < 2000 && graph.renderModeRouter().isInertiaActive(); ++block)
        graph.getNextAudioBlock({&releaseTransition, 0, 64});
    ok &= require(!graph.renderModeRouter().isInertiaActive(), "scratch inertia reaches deck rate");
    const auto normalReaderBeforeHandoff = graph.playback()->getNextReadPosition();
    graph.getNextAudioBlock({&releaseTransition, 0, 64});
    ok &= require(graph.playback()->getNextReadPosition() == normalReaderBeforeHandoff,
                  "completed inertia cannot expose the stale normal reader");
    const double pendingHandoffPosition = graph.renderModeRouter().readPositionSeconds(44'100.0);
    graph.renderModeRouter().prepareNormalPlaybackHandoff(pendingHandoffPosition, 44'100.0);
    graph.getNextAudioBlock({&releaseTransition, 0, 64});
    ok &= require(isFinite(releaseTransition), "deferred scratch handoff output is finite");

    // Exercise callback-owned scratch entry/exit commands across the device
    // block sizes we support. A full cache is already present, so starvation
    // here would indicate a handoff/read-head regression rather than loading.
    const auto starvationBeforeScratchStress = graph.renderModeRouter().scratchCacheStats().starvationBlocks;
    for (const bool reverse : {false, true}) {
        graph.setReverse(reverse);
        for (const bool keylock : {false, true}) {
            graph.setKeylockEnabled(keylock);
            for (const int size : {64, 128, 256, 512, 1024, 2048, 4096, 8192}) {
                juce::AudioBuffer<float> transition(2, size);
                for (int cycle = 0; cycle < 3; ++cycle) {
                    const double anchor = 0.25 + 0.03 * cycle;
                    graph.renderModeRouter().configureTrack(44'100.0, 1.0);
                    graph.renderModeRouter().beginScratch(anchor, 44'100.0, 1.0, true,
                                                 reverse ? -1.0 : 1.0);
                    graph.renderModeRouter().addTargetDeltaSeconds(reverse ? -0.015 : 0.015, 44'100.0);
                    graph.getNextAudioBlock({&transition, 0, size});
                    ok &= require(isFinite(transition), "scratch stress output is finite");
                    (void) graph.renderModeRouter().endScratch(false);
                    graph.renderModeRouter().prepareNormalPlaybackHandoff(anchor, 44'100.0);
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
    ok &= require(graph.renderModeRouter().scratchCacheStats().starvationBlocks == starvationBeforeScratchStress,
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
    graph.renderModeRouter().configureTrack(96'000.0, 0.5);
    graph.renderModeRouter().beginScratch(0.1, 96'000.0, 0.5, true, 1.0);
    graph.renderModeRouter().submitHandDeltaSeconds(0.01, 0.002);
    const auto release96 = graph.renderModeRouter().requestScratchRelease(
        0.5, true, true);
    graph.getNextAudioBlock({&output, 0, 512});
    const auto snapshot96 = graph.renderModeRouter().scratchReleaseSnapshot();
    ok &= require(graph.renderModeRouter().scratchReleaseComplete(release96),
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

    auto invalidRateHandle = cache.openTrack({mono44});
    graph.installPreparedTrack({invalidRateHandle,
                                std::numeric_limits<double>::quiet_NaN(), 8});
    ok &= require(graph.transportSnapshot().trackGeneration == 7
                      && graph.playback() != nullptr,
                  "non-finite track rate cannot replace the active source");

    // Track retirement runs on the control thread while the device callback is
    // live. The lifetime gate has to cover command consumption as well as the
    // downstream router; otherwise this loop can dereference a playback source
    // after installPreparedTrack()/clearTrack() has freed it.
    {
        DeckAudioPipeline swappingGraph(cache);
        swappingGraph.prepareToPlay(512, 48'000.0);
        auto initial = cache.openTrack({mono44});
        swappingGraph.installPreparedTrack({initial, 44'100.0, 1});
        swappingGraph.setTransportRunning(true);

        std::atomic<bool> stopRender { false };
        std::atomic<bool> finiteOutput { true };
        std::atomic<std::uint64_t> renderedBlocks { 0 };
        std::thread renderThread([&] {
            juce::AudioBuffer<float> concurrentOutput(2, 512);
            while (!stopRender.load(std::memory_order_acquire)) {
                swappingGraph.getNextAudioBlock({&concurrentOutput, 0, 512});
                if (!isFinite(concurrentOutput))
                    finiteOutput.store(false, std::memory_order_release);
                renderedBlocks.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::uint64_t generation = 1;
        for (int iteration = 0; iteration < 200; ++iteration) {
            if (iteration % 11 == 0)
                swappingGraph.clearTrack(++generation);
            const bool useMono = (iteration & 1) == 0;
            auto next = cache.openTrack({useMono ? mono44 : stereo96});
            swappingGraph.installPreparedTrack(
                {next, useMono ? 44'100.0 : 96'000.0, ++generation});
            swappingGraph.setTransportRunning(true);
        }

        stopRender.store(true, std::memory_order_release);
        renderThread.join();
        ok &= require(renderedBlocks.load(std::memory_order_relaxed) > 0,
                      "concurrent track-swap stress rendered audio callbacks");
        ok &= require(finiteOutput.load(std::memory_order_acquire),
                      "concurrent track swaps keep callback output finite");
        swappingGraph.clearTrack(++generation);
        swappingGraph.releaseResources();
    }

    ok &= require(realtimeCountersAreZero(graph), "all realtime violation counters are zero");

    auto configurationGeneration = graph.timeStretch().activeConfigurationGeneration();
    graph.timeStretch().setPitchLockEnabled(false);
    graph.timeStretch().setTempoRatio(1.0);
    graph.mixer().setEq(0.0f, 0.0f, 0.0f);
    graph.mixer().setFilterVal(0.0f);
    const auto directMixer512 = measureDirectMixer(graph, 512, 300);
    const auto neutral512 = measure(graph, 512, 300);
    graph.timeStretch().setPitchLockEnabled(true);
    graph.timeStretch().setTempoRatio(0.8);
    const auto keylock512 = measure(graph, 512, 100);
    graph.timeStretch().setPitchLockEnabled(false);
    // Keylock is a per-block routing decision, not a pipeline identity, so none
    // of these toggles may cost a rebuild — that wait used to be exactly the
    // stretch where the deck played back unlocked and jumped when it ended.
    ok &= require(graph.timeStretch().activeConfigurationGeneration() == configurationGeneration,
                  "toggling keylock never rebuilds the time-stretch pipeline");
    graph.mixer().setEq(-1.0f, 0.5f, 1.0f);
    graph.mixer().setFilterVal(-0.5f);
    const auto eqFilter8192 = measure(graph, 8192, 100);
    graph.renderModeRouter().configureTrack(96'000.0, 0.5);
    graph.renderModeRouter().beginScratch(0.1, 96'000.0, 0.5, true, 1.0);
    graph.renderModeRouter().addTargetDeltaSeconds(0.03, 96'000.0);
    const auto scratch512 = measure(graph, 512, 100);
    (void) graph.renderModeRouter().endScratch(false);
    std::cout << "Pre-refactor-equivalent direct Mixer 512: avg=" << directMixer512.averageUs
              << " us worst=" << directMixer512.worstUs << " us\n";
    std::cout << "DeckAudioPipeline neutral 512: avg=" << neutral512.averageUs
              << " us worst=" << neutral512.worstUs << " us\n";
    std::cout << "DeckAudioPipeline keylock 512: avg=" << keylock512.averageUs
              << " us worst=" << keylock512.worstUs << " us\n";
    std::cout << "DeckAudioPipeline scratch 512: avg=" << scratch512.averageUs
              << " us worst=" << scratch512.worstUs << " us\n";
    std::cout << "DeckAudioPipeline EQ/filter 8192: avg=" << eqFilter8192.averageUs
              << " us worst=" << eqFilter8192.worstUs << " us\n";
    std::cout << "DeckAudioPipeline track handover: " << handoverUs << " us\n";

    std::vector<std::unique_ptr<DeckAudioPipeline>> directDecks;
    directDecks.reserve(4);
    for (int index = 0; index < 4; ++index) {
        auto deck = std::make_unique<DeckAudioPipeline>(cache);
        deck->prepareToPlay(8192, 48'000.0);
        auto handle = cache.openTrack({index % 2 == 0 ? mono44 : stereo96});
        deck->installPreparedTrack({handle, index % 2 == 0 ? 44'100.0 : 96'000.0,
                                    static_cast<std::uint64_t>(index + 1)});
        deck->setTransportRunning(true);
        deck->setKeylockEnabled(index % 2 != 0);
        deck->setPlaybackRate(0.85 + index * 0.05);
        deck->mixer().setEq(-0.5f, 0.25f, 0.5f);
        deck->mixer().setFilterVal(index % 2 == 0 ? -0.4f : 0.4f);
        if (index == 0) {
            deck->renderModeRouter().configureTrack(44'100.0, 1.0);
            deck->renderModeRouter().beginScratch(0.1, 44'100.0, 1.0, true, 1.0);
            deck->renderModeRouter().addTargetDeltaSeconds(0.02, 44'100.0);
        }
        directDecks.push_back(std::move(deck));
    }

    double fourDeckTotalUs = 0.0;
    double fourDeckWorstUs = 0.0;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        for (auto& deck : directDecks) {
            output.clear();
            deck->getNextAudioBlock({&output, 0, 512});
            ok &= require(isFinite(output), "four-deck stress output is finite");
        }
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        fourDeckTotalUs += elapsed;
        fourDeckWorstUs = std::max(fourDeckWorstUs, elapsed);
    }
    std::cout << "DeckAudioPipeline four decks/512: avg=" << fourDeckTotalUs / 100.0
              << " us worst=" << fourDeckWorstUs << " us\n";

    for (auto& deck : directDecks) {
        deck->clearTrack(100);
        deck->releaseResources();
    }
    directDecks.clear();

    AudioEngine audioEngine(cache);
    std::array<DeckAudioPipeline*, 4> decks {
        &audioEngine.deck(0), &audioEngine.deck(1), &audioEngine.deck(2), &audioEngine.deck(3)
    };
    for (int index = 0; index < 4; ++index) {
        auto handle = cache.openTrack({index % 2 == 0 ? mono44 : stereo96});
        decks[index]->installPreparedTrack({handle, index % 2 == 0 ? 44'100.0 : 96'000.0,
                                            static_cast<std::uint64_t>(index + 1)});
        decks[index]->setTransportRunning(true);
        AudioEngine::setPflEnabled(index, index % 2 == 0);
        decks[index]->setKeylockEnabled(index % 2 != 0);
        decks[index]->setPlaybackRate(0.85 + index * 0.05);
        decks[index]->mixer().setEq(-0.5f, 0.25f, 0.5f);
        decks[index]->mixer().setFilterVal(index % 2 == 0 ? -0.4f : 0.4f);
    }
    AudioEngine::setOutputRouting(1, 3, 5);
    AudioEngine::setMasterVolume(0.7f);
    AudioEngine::setHeadphoneMix(0.25f);
    AudioEngine::setMasterCueEnabled(true);
    AudioEngine::setAntiClipEnabled(true);
    audioEngine.prepareToPlay(16'384, 48'000.0);
    audioEngine.resetRealtimeStats();
    AudioEngine::resetCallbackStats();
    {
        juce::AudioBuffer<float> limiterWarmup(6, 512);
        audioEngine.getNextAudioBlock({&limiterWarmup, 0, 512});
    }

    double audioEngineTotalUs = 0.0;
    double audioEngineWorstUs = 0.0;
    std::mt19937 stressRandom(0xB40CD1u);
    std::uniform_real_distribution<float> controlValue(0.0f, 1.0f);
    std::array<std::uint64_t, 4> stressTrackGenerations {20, 20, 20, 20};
    constexpr std::array callbackSizes {64, 128, 256, 512, 1024, 2048,
                                        4096, 8192, 16'384};
    double soakSeconds = 0.0;
    if (const char* configured = std::getenv("BROCKDJ_AUDIO_SOAK_SECONDS"))
        soakSeconds = std::max(0.0, std::strtod(configured, nullptr));
    const bool soakMode = soakSeconds > 0.0;
    const auto soakDeadline = std::chrono::steady_clock::now()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(soakSeconds));
    if (soakMode) {
        for (auto* deck : decks) {
            deck->mixer().setFxSlotEffectType(1, EffectType::Bitcrusher);
            deck->mixer().setFxSlotAmount(1, 0.2f);
        }
        AudioEngine::setMasterFx(EffectType::Bitcrusher, 0.1f);
    }
    std::uint64_t measuredBlocks = 0;
    std::uint64_t deadlineMisses = 0;
    std::uint64_t silentBlocks = 0;
    for (int iteration = 0;
         soakMode ? std::chrono::steady_clock::now() < soakDeadline : iteration < 72;
         ++iteration) {
        const int blockSize = soakMode ? ((iteration & 1) == 0 ? 64 : 128)
                                       : callbackSizes[static_cast<std::size_t>(iteration)
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
        if (!soakMode && iteration % 18 == 6) {
            auto& deck = *decks[selectedDeck];
            auto replacement = cache.openTrack({iteration % 2 == 0 ? mono44 : stereo96});
            deck.installPreparedTrack({replacement, iteration % 2 == 0 ? 44'100.0 : 96'000.0,
                                       ++stressTrackGenerations[selectedDeck]});
            deck.setTransportRunning(true);
        }
        if (iteration % 16 == 5) {
            auto& scratchDeck = *decks[selectedDeck];
            scratchDeck.renderModeRouter().configureTrack(48'000.0, 1.0);
            scratchDeck.renderModeRouter().beginScratch(0.1, 48'000.0, 1.0, true, 1.0);
            scratchDeck.renderModeRouter().addTargetDeltaSeconds(0.01, 48'000.0);
        }
        if (iteration % 16 == 7)
            (void) decks[selectedDeck]->renderModeRouter().endScratch(false);
        if (iteration % 10 == 2) {
            decks[selectedDeck]->setKeylockEnabled(iteration % 20 == 2);
            decks[selectedDeck]->setPlaybackRate(0.85 + 0.2 * controlValue(stressRandom));
        }
        if (!soakMode && iteration % 14 == 8) {
            decks[selectedDeck]->mixer().setFxSlotEffectType(1, EffectType::Bitcrusher);
            decks[selectedDeck]->mixer().setFxSlotAmount(
                1, 0.15f + 0.2f * controlValue(stressRandom));
        } else if (!soakMode && iteration % 14 == 10) {
            decks[selectedDeck]->mixer().setFxSlotEffectType(1, EffectType::None);
        }
        // DjEngine normally maps the crossfader curve/assignment to these post-fader gains.
        decks[iteration % 4]->mixer().setFader(0.2f + 0.8f * controlValue(stressRandom));
        decks[(iteration + 1) % 4]->mixer().setFilterVal(
            -0.5f + controlValue(stressRandom));
        decks[(iteration + 1) % 4]->mixer().setEq(-0.5f + controlValue(stressRandom),
                                                  -0.5f + controlValue(stressRandom),
                                                  -0.5f + controlValue(stressRandom));
        AudioEngine::setPflEnabled((iteration + 2) % 4, iteration % 3 != 0);
        juce::AudioBuffer<float> masterOutput(6, blockSize);
        const auto start = std::chrono::steady_clock::now();
        audioEngine.getNextAudioBlock({&masterOutput, 0, blockSize});
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        audioEngineTotalUs += elapsed;
        audioEngineWorstUs = std::max(audioEngineWorstUs, elapsed);
        ++measuredBlocks;
        const double callbackBudgetUs = static_cast<double>(blockSize) * 1'000'000.0 / 48'000.0;
        if (elapsed > callbackBudgetUs)
            ++deadlineMisses;
        ok &= require(isFinite(masterOutput), "four-graph master-bus output is finite");
        const bool outputIsSilent = masterOutput.getMagnitude(0, 0, blockSize) <= 0.00001f;
        if (outputIsSilent)
            ++silentBlocks;
        if (!soakMode)
            ok &= require(!outputIsSilent, "four-graph master-bus output is non-silent");
        if (blockSize > AudioEngine::kProcessingChunkSize) {
            const int tailStart = blockSize - std::min(64, blockSize);
            ok &= require(masterOutput.getMagnitude(0, tailStart, blockSize - tailStart)
                              > 0.00001f,
                          "chunked master-bus tail is non-silent");
        }
    }

    auto& retiringDeck = *decks[2];
    retiringDeck.setKeylockEnabled(false);
    retiringDeck.setPlaybackRate(1.0);
    retiringDeck.renderModeRouter().exitScratchMode(0.0, 44'100.0);
    retiringDeck.transport().setPosition(0.0);
    retiringDeck.setTransportRunning(true);
    retiringDeck.mixer().setFader(1.0f);
    retiringDeck.mixer().setFxSlotEffectType(1, EffectType::Echo);
    retiringDeck.mixer().setFxSlotAmount(1, 1.0f);
    retiringDeck.mixer().setFxSlotExternalDelayTime(1, 0.005f);
    for (int block = 0; block < 8; ++block) {
        juce::AudioBuffer<float> tailWarmup(6, 512);
        audioEngine.getNextAudioBlock({&tailWarmup, 0, 512});
    }
    ok &= require(retiringDeck.postFaderTailBuffer().getMagnitude(0, 0, 512) > 0.00001f,
                  "post-fader echo is active before deck reset");
    retiringDeck.clearTrack(100);
    {
        juce::AudioBuffer<float> retirementOutput(6, 512);
        audioEngine.getNextAudioBlock({&retirementOutput, 0, 512});
        ok &= require(isFinite(retirementOutput), "master bus survives deck reset");
        ok &= require(retiringDeck.postFaderTailBuffer().getMagnitude(0, 0, 512) > 0.00001f,
                      "post-fader echo tail survives a real deck reset");
    }
    auto replacement = cache.openTrack({mono44});
    decks[2]->installPreparedTrack({replacement, 44'100.0, 101});
    decks[2]->setTransportRunning(true);
    {
        juce::AudioBuffer<float> replacementOutput(6, 16'384);
        audioEngine.getNextAudioBlock({&replacementOutput, 0, 16'384});
        ok &= require(isFinite(replacementOutput), "replacement generation stays finite");
    }
    const auto masterStats = audioEngine.realtimeStats();
    ok &= require(masterStats.oversizedCallbacks > 0,
                  "large callbacks use the chunked master-bus path");
    ok &= require(realtimeCountersAreZero(audioEngine),
                  "master-bus realtime violation counters are zero");
    if (soakMode) {
        ok &= require(silentBlocks * 20 < measuredBlocks,
                      "soak has no sustained output starvation");
        const bool strictRealtime = std::getenv("BROCKDJ_AUDIO_SOAK_STRICT") != nullptr;
        if (strictRealtime) {
            ok &= require(deadlineMisses == 0 && AudioEngine::callbackOverrunCount() == 0,
                          "realtime-scheduled soak stays within every callback budget");
        }
    }
    std::cout << "AudioEngine four real graphs/mixed blocks: avg="
              << audioEngineTotalUs / static_cast<double>(std::max<std::uint64_t>(1, measuredBlocks))
              << " us worst=" << audioEngineWorstUs << " us blocks=" << measuredBlocks
              << " deadline-misses=" << deadlineMisses
              << " silent-transition-blocks=" << silentBlocks << "\n";

    audioEngine.beginShutdown();
    audioEngine.releaseResources();

    for (auto& deck : decks) {
        ok &= require(realtimeCountersAreZero(*deck), "four-deck realtime counters are zero");
        deck->clearTrack(100);
        deck->releaseResources();
    }
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
