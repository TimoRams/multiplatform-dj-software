#include "engine/sync/DeckSyncController.h"
#include "engine/sync/SyncCoordinator.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>

using namespace engine::sync;

namespace {

DeckSyncInputSnapshot input(std::uint64_t generation, double bpm, double phase = 0.0,
                            bool playing = true)
{
    DeckSyncInputSnapshot value;
    value.hasTrack = bpm > 0.0 && std::isfinite(bpm);
    value.playing = playing;
    value.trackBpm = value.hasTrack ? bpm : 0.0;
    value.effectiveBpm = value.trackBpm;
    value.playbackRate = 1.0;
    value.beatPhase = phase;
    value.barPosition = phase / 4.0;
    value.beatPosition = phase;
    value.beatLengthSeconds = value.hasTrack ? 60.0 / bpm : 0.0;
    value.beatgridValid = value.hasTrack;
    value.downbeatValid = value.hasTrack;
    value.beatConfidence = value.hasTrack ? 1.0 : 0.0;
    value.downbeatConfidence = value.hasTrack ? 1.0 : 0.0;
    value.trackIdentity = generation + 100;
    value.trackGeneration = generation;
    value.transportGeneration = generation;
    return value;
}

void controllerTests()
{
    DeckSyncController controller({0});
    assert(!controller.snapshot().syncEnabled);
    assert(controller.setSyncEnabled(true));
    controller.update(input(1, 0.0));

    DeckSyncCommand invalid;
    invalid.syncEnabled = true;
    invalid.targetBpm = 120.0;
    invalid.masterGeneration = 1;
    invalid.targetTrackGeneration = 1;
    controller.applyCoordinatorCommand(invalid);
    assert(controller.snapshot().error == SyncError::NoTrack);

    controller.update(input(2, 120.0));
    DeckSyncCommand sameTempo;
    sameTempo.syncEnabled = true;
    sameTempo.targetBpm = 120.0;
    sameTempo.masterGeneration = 2;
    sameTempo.targetTrackGeneration = 2;
    sameTempo.masterPlaying = true;
    sameTempo.masterBeatPhase = 0.0;
    controller.applyCoordinatorCommand(sameTempo);
    auto actions = controller.takeActions();
    assert(actions.tempoChanged && std::abs(actions.targetTempoPercent) < 1.0e-12);

    sameTempo.targetBpm = 60.0;
    controller.applyCoordinatorCommand(sameTempo);
    assert(std::abs(controller.takeActions().targetTempoPercent + 50.0) < 1.0e-12);
    sameTempo.targetBpm = 240.0;
    controller.applyCoordinatorCommand(sameTempo);
    assert(std::abs(controller.takeActions().targetTempoPercent - 100.0) < 1.0e-12);

    sameTempo.targetBpm = std::numeric_limits<double>::quiet_NaN();
    controller.applyCoordinatorCommand(sameTempo);
    assert(controller.snapshot().error == SyncError::InvalidBpm);

    controller.update(input(3, 120.0, 0.02));
    sameTempo.targetBpm = 120.0;
    sameTempo.targetTrackGeneration = 3;
    sameTempo.masterGeneration = 3;
    sameTempo.masterBeatPhase = 0.0;
    controller.applyCoordinatorCommand(sameTempo);
    actions = controller.takeActions();
    assert(actions.phaseNudgeChanged);
    assert(std::isfinite(actions.phaseNudgePercent));

    auto scratch = input(3, 120.0, 0.25);
    scratch.scratching = true;
    controller.update(scratch);
    controller.applyCoordinatorCommand(sameTempo);
    assert(std::abs(controller.snapshot().phaseNudgePercent) < 1.0e-12);
    scratch.scratching = false;
    scratch.reverse = true;
    scratch.slipEnabled = true;
    scratch.loopActive = true;
    controller.update(scratch);
    controller.applyCoordinatorCommand(sameTempo);
    assert(std::isfinite(controller.snapshot().phaseNudgePercent));

    DeckSyncCommand arrange = sameTempo;
    arrange.phaseArrangeRequested = true;
    arrange.masterBarPosition = 0.4;
    controller.applyCoordinatorCommand(arrange);
    actions = controller.takeActions();
    assert(actions.seekRequested);
    assert(std::abs(actions.seekOffsetSeconds) <= 2.0 * scratch.beatLengthSeconds + 1.0e-12);

    controller.update(input(4, 122.0));
    controller.applyCoordinatorCommand(arrange);
    assert(controller.snapshot().error == SyncError::StaleTrackGeneration);
    arrange.targetTrackGeneration = 4;
    arrange.masterGeneration = 4;
    controller.applyCoordinatorCommand(arrange);
    const auto current = controller.snapshot();
    arrange.masterGeneration = 3;
    controller.applyCoordinatorCommand(arrange);
    assert(controller.snapshot().error == SyncError::StaleMasterGeneration);
    assert(controller.snapshot().masterGeneration == current.masterGeneration);

    for (std::uint64_t generation = 5; generation <= 8; ++generation) {
        controller.update(input(generation, 120.0 + generation));
        arrange.targetTrackGeneration = generation;
        arrange.masterGeneration = generation;
        controller.applyCoordinatorCommand(arrange);
        assert(controller.snapshot().trackGeneration == generation);
    }
}

void coordinatorTests()
{
    SyncCoordinator coordinator;
    assert(coordinator.snapshot().masterDeckIndex == -1);
    std::array<DeckSyncController, 4> decks {{
        DeckSyncController({0}), DeckSyncController({1}),
        DeckSyncController({2}), DeckSyncController({3})}};
    for (int index = 0; index < 4; ++index) {
        assert(coordinator.registerDeck(index, decks[index]));
        coordinator.updateDeck(index, input(1, 120.0 + index));
    }
    assert(coordinator.registeredDeckCount() == 4);

    coordinator.setDeckSyncEnabled(2, true);
    assert(coordinator.snapshot().masterDeckIndex == 2);
    const auto generation1 = coordinator.snapshot().masterGeneration;
    coordinator.setDeckSyncEnabled(0, true);
    coordinator.setDeckSyncEnabled(1, true);
    coordinator.setDeckSyncEnabled(3, true);
    assert(coordinator.snapshot().masterDeckIndex == 2);

    coordinator.requestMaster(1, true);
    assert(coordinator.snapshot().masterDeckIndex == 1);
    assert(coordinator.snapshot().masterGeneration > generation1);
    auto paused = input(1, 121.0, 0.0, false);
    coordinator.updateDeck(1, paused);
    assert(coordinator.snapshot().masterDeckIndex == 1);

    coordinator.updateDeck(1, input(2, 128.0));
    assert(coordinator.snapshot().masterGeneration > generation1);
    coordinator.unregisterDeck(1);
    assert(coordinator.snapshot().masterDeckIndex == 2);
    coordinator.setDeckSyncEnabled(2, false);
    assert(coordinator.snapshot().masterDeckIndex == 0);

    LinkSyncSnapshot link;
    link.enabled = true;
    link.numPeers = 2;
    link.bpm = 126.0;
    link.generation = 1;
    coordinator.setLinkSnapshot(link);
    assert(coordinator.snapshot().linkActive);
    assert(coordinator.snapshot().masterDeckIndex == 0); // Existing product rule: Link is external.

    coordinator.shutdown();
    assert(coordinator.registeredDeckCount() == 0);
}

void fourDeckStressAndPerformance()
{
    SyncCoordinator coordinator;
    std::array<DeckSyncController, 4> decks {{
        DeckSyncController({0}), DeckSyncController({1}),
        DeckSyncController({2}), DeckSyncController({3})}};
    for (int index = 0; index < 4; ++index) {
        coordinator.registerDeck(index, decks[index]);
        coordinator.setDeckSyncEnabled(index, true);
    }

    std::mt19937 random(0xB0C4D1u);
    std::uniform_real_distribution<double> bpm(70.0, 180.0);
    std::uniform_real_distribution<double> phase(0.0, 0.9999);
    const auto start = std::chrono::steady_clock::now();
    constexpr int iterations = 50'000;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int index = 0; index < 4; ++index) {
            auto value = input(1 + static_cast<std::uint64_t>(iteration / 1000), bpm(random),
                               phase(random), (iteration + index) % 11 != 0);
            value.scratching = (iteration + index) % 127 == 0;
            value.reverse = (iteration + index) % 211 == 0;
            value.slipEnabled = (iteration + index) % 173 == 0;
            value.loopActive = (iteration + index) % 97 < 3;
            coordinator.updateDeck(index, value);
            const auto actions = decks[index].takeActions();
            assert(!actions.tempoChanged || std::isfinite(actions.targetTempoPercent));
            assert(!actions.phaseNudgeChanged || std::isfinite(actions.phaseNudgePercent));
            assert(!actions.seekRequested || std::isfinite(actions.seekOffsetSeconds));
        }
        if (iteration % 997 == 0)
            coordinator.requestMaster((iteration / 997) % 4, true);
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    const double perFourDeckUpdate = elapsed / iterations;
    constexpr int measurements = 100'000;
    auto measureStart = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < measurements; ++iteration)
        coordinator.updateDeck(0, input(60, 128.0, (iteration % 1000) / 1000.0));
    const double oneDeckUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - measureStart).count() / measurements;

    measureStart = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < measurements; ++iteration)
        coordinator.requestMaster(iteration & 3, true);
    const double masterSwitchUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - measureStart).count() / measurements;

    measureStart = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < measurements; ++iteration)
        coordinator.updateDeck(coordinator.snapshot().masterDeckIndex,
                               input(61 + static_cast<std::uint64_t>(iteration), 126.0));
    const double masterTrackSwitchUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - measureStart).count() / measurements;

    std::cout << "PERF sync_one_deck_update_us=" << oneDeckUs
              << " sync_four_deck_cycle_us=" << perFourDeckUpdate
              << " sync_per_deck_command_us=" << elapsed / (iterations * 4.0)
              << " sync_master_selection_us=" << masterSwitchUs
              << " sync_master_track_switch_us=" << masterTrackSwitchUs << '\n';
    assert(coordinator.snapshot().masterDeckIndex >= 0);
    coordinator.shutdown();
}

} // namespace

int main()
{
    controllerTests();
    coordinatorTests();
    fourDeckStressAndPerformance();
    std::cout << "Sync controller/coordinator tests passed\n";
    return 0;
}
