#include "engine/deck/DeckTransport.h"

#include "audio/cache/AudioPageCache.h"
#include "engine/deck/DeckAudioGraph.h"
#include "engine/sync/DeckSyncController.h"
#include "engine/sync/SyncCoordinator.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

namespace {

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

bool writeWave(const QString& path, double frequency)
{
    constexpr double sampleRate = 48'000.0;
    constexpr int sampleCount = 48'000;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream =
        std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    auto writer = format.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}
                    .withSampleRate(sampleRate).withNumChannels(2).withBitsPerSample(16));
    if (!writer)
        return false;
    juce::AudioBuffer<float> buffer(2, sampleCount);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < sampleCount; ++sample)
            buffer.setSample(channel, sample, static_cast<float>(0.15 * std::sin(
                juce::MathConstants<double>::twoPi * (frequency + channel * 37.0)
                * sample / sampleRate)));
    return writer->writeFromAudioSampleBuffer(buffer, 0, sampleCount);
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

bool finiteSnapshot(const DeckTransportSnapshot& snapshot)
{
    return std::isfinite(snapshot.audiblePositionSeconds)
        && std::isfinite(snapshot.backgroundPositionSeconds)
        && std::isfinite(snapshot.trackLengthSeconds)
        && std::isfinite(snapshot.playbackRate)
        && std::isfinite(snapshot.preRollPositionSeconds)
        && std::isfinite(snapshot.sourceSampleRate);
}

bool realtimeCountersAreZero(const DeckAudioGraph& graph)
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

void render(DeckAudioGraph& graph, int blocks = 1)
{
    juce::AudioBuffer<float> buffer(2, 256);
    for (int i = 0; i < blocks; ++i)
        graph.getNextAudioBlock({&buffer, 0, buffer.getNumSamples()});
}

struct DeckFixture {
    explicit DeckFixture(AudioPageCache& cache) : graph(cache), transport(graph)
    {
        graph.prepareToPlay(512, 48'000.0);
    }
    ~DeckFixture() { graph.releaseResources(); }
    DeckAudioGraph graph;
    DeckTransport transport;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir directory;
    std::array<QString, 4> paths;
    for (int i = 0; i < 4; ++i) {
        paths[static_cast<std::size_t>(i)] = directory.filePath(QString("track-%1.wav").arg(i));
        ok &= require(writeWave(paths[static_cast<std::size_t>(i)], 180.0 + i * 80.0),
                      "synthetic track created");
    }

    AudioPageCache cache(32 * 1024 * 1024);
    DeckFixture deck(cache);
    auto& transport = deck.transport;

    const auto initial = transport.snapshot();
    ok &= require(!initial.hasTrack && !initial.playing && finiteSnapshot(initial),
                  "initial snapshot without track");
    ok &= require(transport.setPlaying(true), "play intent without track changes once");
    ok &= require(!transport.setPlaying(true), "identical play command is idempotent");
    ok &= require(!transport.seekToSeconds(0.2), "seek without track rejected");
    ok &= require(transport.setReverse(true) && transport.setSlipEnabled(true),
                  "reverse and slip state valid without track");
    transport.setPlaying(false);

    auto trackA = cache.openTrack({paths[0]});
    ok &= require(waitForPage(cache, trackA), "track A cached");
    ok &= require(transport.installPreparedTrack(trackA, {1, 48'000.0, 1.0}),
                  "track A installed");
    ok &= require(transport.snapshot().trackGeneration == 1
                  && transport.snapshot().trackLengthSeconds == 1.0,
                  "track configuration published");
    transport.setReverse(false);

    ok &= require(transport.seekToSeconds(0.25), "forward seek");
    ok &= require(std::abs(transport.snapshot().audiblePositionSeconds - 0.25) < 1.0e-9,
                  "forward seek position");
    transport.seekToSeconds(0.1);
    transport.seekToSeconds(0.9);
    transport.seekToSeconds(0.0);
    ok &= require(transport.snapshot().audiblePositionSeconds == 0.0, "rapid seeks end at latest target");
    transport.seekToSeconds(2.0);
    ok &= require(transport.snapshot().atTrackEnd
                  && transport.snapshot().audiblePositionSeconds == 1.0,
                  "seek clamps to and identifies track end");
    ok &= require(transport.setPlaying(true), "play intent accepted at track end");
    transport.ensureAudioRunning();
    ok &= require(!transport.audioRunning(), "audio does not restart at true EOF");
    transport.seekToSeconds(0.2);
    transport.ensureAudioRunning();
    ok &= require(transport.audioRunning(), "seek back from end permits play");
    ok &= require(transport.setPlaying(false), "pause changes state");
    ok &= require(!transport.setPlaying(false), "identical pause is idempotent");

    transport.setReverse(false);
    ok &= require(transport.setReverse(true) && transport.reverse(), "reverse enabled");
    ok &= require(transport.setReverse(false) && !transport.reverse(), "reverse disabled");
    transport.seekToSeconds(-0.02);
    ok &= require(transport.snapshot().preRollPositionSeconds < 0.0, "negative seek retained");
    transport.setReverse(true);
    ok &= require(transport.snapshot().reverse, "reverse remains defined in pre-roll");
    transport.setPlaybackRate(8.0);
    transport.beginPreRoll(-0.01);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const auto preRollUpdate = transport.updateControlState({}, false, false);
    ok &= require(preRollUpdate.leftPreRoll && transport.snapshot().audiblePositionSeconds == 0.0,
                  "pre-roll crosses sample zero");
    transport.setPlaying(false);

    transport.seekToSeconds(0.1);
    transport.setSlipEnabled(false);
    transport.setSlipEnabled(true);
    const double slipStart = transport.snapshot().backgroundPositionSeconds;
    transport.setPlaying(true);
    render(deck.graph, 4);
    transport.updateControlState({true, 0.05, 0.15}, false, false);
    transport.seekAudioToSeconds(0.6);
    render(deck.graph, 4);
    transport.updateControlState({true, 0.05, 0.15}, false, false);
    ok &= require(transport.snapshot().backgroundPositionSeconds >= slipStart,
                  "slip background advances independently");
    transport.returnToSlipPosition();
    ok &= require(std::abs(transport.audioPositionSeconds()
                           - transport.snapshot().backgroundPositionSeconds) < 0.01,
                  "slip returns to background position");
    transport.setPlaying(false);

    auto trackB = cache.openTrack({paths[1]});
    ok &= require(waitForPage(cache, trackB), "track B cached");
    ok &= require(transport.installPreparedTrack(trackB, {2, 48'000.0, 1.0}),
                  "track B installed");
    ok &= require(!transport.installPreparedTrack(trackA, {1, 48'000.0, 1.0}),
                  "stale track generation rejected");
    ok &= require(transport.snapshot().trackGeneration == 2
                  && transport.snapshot().audiblePositionSeconds == 0.0,
                  "track switch resets position and preserves current generation");
    double trackSwitchTotalUs = 0.0;
    for (std::uint64_t generation = 3; generation <= 5; ++generation) {
        auto handle = cache.openTrack({paths[static_cast<std::size_t>((generation - 1) % 4)]});
        ok &= require(waitForPage(cache, handle), "switch track cached");
        const auto switchStart = std::chrono::steady_clock::now();
        ok &= require(transport.installPreparedTrack(handle, {generation, 48'000.0, 1.0}),
                      "rapid generation installed");
        trackSwitchTotalUs += std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - switchStart).count();
    }
    transport.clearTrack(6);
    ok &= require(!transport.snapshot().hasTrack && transport.snapshot().trackGeneration == 6,
                  "clear invalidates old generations");

    auto stressHandle = cache.openTrack({paths[0]});
    ok &= require(waitForPage(cache, stressHandle), "stress track cached");
    transport.installPreparedTrack(stressHandle, {7, 48'000.0, 1.0});
    std::atomic<bool> reading {true};
    std::atomic<bool> snapshotsValid {true};
    std::thread reader([&] {
        std::uint64_t previousState = 0;
        while (reading.load(std::memory_order_acquire)) {
            const auto snapshot = transport.snapshot();
            if (!finiteSnapshot(snapshot) || snapshot.trackGeneration != 7
                || snapshot.stateGeneration < previousState)
                snapshotsValid.store(false, std::memory_order_release);
            previousState = snapshot.stateGeneration;
        }
    });
    for (int i = 0; i < 1'000; ++i) {
        transport.seekToSeconds(static_cast<double>(i % 1000) / 1000.0);
        transport.setReverse((i & 1) != 0);
        transport.setSlipEnabled((i & 2) != 0);
    }
    reading.store(false, std::memory_order_release);
    reader.join();
    ok &= require(snapshotsValid.load(std::memory_order_acquire),
                  "concurrent snapshots remain coherent");

    std::array<std::unique_ptr<DeckFixture>, 4> decks;
    for (auto& fixture : decks)
        fixture = std::make_unique<DeckFixture>(cache);
    std::array<std::uint64_t, 4> generations {10, 20, 30, 40};
    engine::sync::SyncCoordinator syncCoordinator;
    std::array<engine::sync::DeckSyncController, 4> syncControllers {{
        engine::sync::DeckSyncController({0}), engine::sync::DeckSyncController({1}),
        engine::sync::DeckSyncController({2}), engine::sync::DeckSyncController({3})}};
    for (std::size_t i = 0; i < decks.size(); ++i) {
        auto handle = cache.openTrack({paths[i]});
        ok &= require(waitForPage(cache, handle), "four-deck fixture cached");
        ok &= require(decks[i]->transport.installPreparedTrack(
                          handle, {generations[i], 48'000.0, 1.0}),
                      "four-deck track installed");
        syncCoordinator.registerDeck(static_cast<int>(i), syncControllers[i]);
        syncCoordinator.setDeckSyncEnabled(static_cast<int>(i), true);
    }
    std::mt19937 random(0xB40CD1u);
    auto controlStart = std::chrono::steady_clock::now();
    constexpr int stressSteps = 40;
    for (int step = 0; step < stressSteps; ++step) {
        for (std::size_t i = 0; i < decks.size(); ++i) {
            auto& fixture = *decks[i];
            const auto command = random() % 9;
            if (command == 0) fixture.transport.setPlaying(!fixture.transport.playRequested());
            if (command == 1) fixture.transport.seekToSeconds((random() % 1200) / 1000.0 - 0.1);
            if (command == 2) fixture.transport.setReverse((random() & 1u) != 0);
            if (command == 3) fixture.transport.setSlipEnabled((random() & 1u) != 0);
            if (command == 4) fixture.transport.setPlaybackRate(0.5 + (random() % 1500) / 1000.0);
            if (command == 5) fixture.transport.setKeylockEnabled((random() & 1u) != 0);
            if (command == 6) fixture.transport.beginPreRoll(-0.001 * (1 + random() % 20));
            if (command == 7) fixture.transport.setLoopRegion({true, 0.1, 0.4});
            if (command == 8) fixture.transport.publishScratchPosition((random() % 1000) / 1000.0);
            const auto transportSnapshot = fixture.transport.snapshot();
            engine::sync::DeckSyncInputSnapshot syncInput;
            syncInput.hasTrack = transportSnapshot.hasTrack;
            syncInput.playing = transportSnapshot.playing;
            syncInput.scratching = command == 8;
            syncInput.reverse = transportSnapshot.reverse;
            syncInput.slipEnabled = transportSnapshot.slipEnabled;
            syncInput.loopActive = (step & 3) == 0;
            syncInput.keylockEnabled = (command == 5);
            syncInput.beatgridValid = true;
            syncInput.downbeatValid = true;
            syncInput.trackBpm = 96.0 + static_cast<double>(i) * 12.0;
            syncInput.effectiveBpm = syncInput.trackBpm * transportSnapshot.playbackRate;
            syncInput.playbackRate = transportSnapshot.playbackRate;
            syncInput.audiblePositionSeconds = transportSnapshot.audiblePositionSeconds;
            syncInput.beatPosition = syncInput.audiblePositionSeconds * syncInput.trackBpm / 60.0;
            syncInput.beatPhase = std::fmod(syncInput.beatPosition + 4.0, 1.0);
            syncInput.barPosition = std::fmod(syncInput.beatPosition + 16.0, 4.0) / 4.0;
            syncInput.beatLengthSeconds = 60.0 / syncInput.trackBpm;
            syncInput.beatConfidence = 1.0;
            syncInput.downbeatConfidence = 1.0;
            syncInput.trackIdentity = i + 1;
            syncInput.trackGeneration = transportSnapshot.trackGeneration;
            syncInput.transportGeneration = transportSnapshot.stateGeneration;
            syncCoordinator.updateDeck(static_cast<int>(i), syncInput);
            const auto syncActions = syncControllers[i].takeActions();
            if (syncActions.tempoChanged)
                fixture.transport.setPlaybackRate(std::clamp(
                    1.0 + syncActions.targetTempoPercent / 100.0, 0.01, 8.0));
            if (syncActions.seekRequested
                && syncActions.targetTrackGeneration == fixture.transport.trackGeneration())
                fixture.transport.seekToSeconds(transportSnapshot.audiblePositionSeconds
                                                + syncActions.seekOffsetSeconds);
            render(fixture.graph);
            fixture.transport.updateControlState({(step & 3) == 0, 0.1, 0.4},
                                                  command == 8, false);
            const auto snapshot = fixture.transport.snapshot();
            ok &= require(finiteSnapshot(snapshot), "four-deck snapshot finite");
            ok &= require(snapshot.trackGeneration == generations[i],
                          "four-deck generation stable");
        }
        if (step % 9 == 0)
            syncCoordinator.requestMaster((step / 9) % 4, true);
        if (step % 13 == 0) {
            engine::sync::LinkSyncSnapshot link;
            link.enabled = (step & 1) == 0;
            link.bpm = 120.0 + step;
            link.generation = static_cast<std::uint64_t>(step + 1);
            syncCoordinator.setLinkSnapshot(link);
        }
    }
    const double fourDeckStressStepUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - controlStart).count() / (stressSteps * 4.0);

    controlStart = std::chrono::steady_clock::now();
    constexpr int controlIterations = 2'000;
    for (int step = 0; step < controlIterations; ++step)
        for (auto& fixture : decks)
            fixture->transport.updateControlState({}, false, false);
    const double fourDeckAverageUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - controlStart).count() / (controlIterations * 4.0);

    constexpr int measurementIterations = 20'000;
    auto measurementStart = std::chrono::steady_clock::now();
    std::uint64_t generationAccumulator = 0;
    for (int i = 0; i < measurementIterations; ++i)
        generationAccumulator += transport.snapshot().stateGeneration;
    const double snapshotNs = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - measurementStart).count() / measurementIterations;
    measurementStart = std::chrono::steady_clock::now();
    for (int i = 0; i < measurementIterations; ++i)
        transport.setPlaybackRate(0.75 + (i & 1) * 0.5);
    const double commandNs = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - measurementStart).count() / measurementIterations;
    measurementStart = std::chrono::steady_clock::now();
    constexpr int seekIterations = 500;
    for (int i = 0; i < seekIterations; ++i)
        transport.seekToSeconds((i % 1000) / 1000.0);
    const double seekUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - measurementStart).count() / seekIterations;

    for (const auto& fixture : decks)
        ok &= require(realtimeCountersAreZero(fixture->graph),
                      "four-deck realtime violation counters remain zero");
    ok &= require(realtimeCountersAreZero(deck.graph),
                  "transport realtime violation counters remain zero");
    syncCoordinator.shutdown();
    ok &= require(generationAccumulator != 0, "snapshot measurement consumed");
    std::cout << "PERF snapshot_read_ns=" << snapshotNs
              << " command_handoff_ns=" << commandNs
              << " seek_handoff_us=" << seekUs
              << " track_switch_us=" << trackSwitchTotalUs / 3.0
              << " four_deck_control_update_us=" << fourDeckAverageUs
              << " four_deck_stress_step_us=" << fourDeckStressStepUs << '\n';

    return ok ? 0 : 1;
}
