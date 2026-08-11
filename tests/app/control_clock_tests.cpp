#include "app/ControlClock.h"

#include <QCoreApplication>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>
#include <vector>

namespace {

struct Counts {
    int fast = 0;
    int transport = 0;
    int syncInput = 0;
    int syncCoordinate = 0;
    int syncApply = 0;
    int waveform = 0;
    int feedback = 0;
    int display = 0;
    int meters = 0;
    int statistics = 0;
    int housekeeping = 0;
};

ControlClock::Callbacks callbacksFor(Counts& counts, std::vector<int>* order = nullptr)
{
    ControlClock::Callbacks callbacks;
    callbacks.fast = [&counts](const ControlTickContext& context) {
        assert(std::isfinite(context.deltaSeconds));
        ++counts.fast;
    };
    callbacks.transport = [&counts, order](const ControlTickContext&) {
        ++counts.transport;
        if (order) order->push_back(1);
    };
    callbacks.syncInput = [&counts, order](const ControlTickContext&) {
        ++counts.syncInput;
        if (order) order->push_back(2);
    };
    callbacks.syncCoordinate = [&counts, order](const ControlTickContext&) {
        ++counts.syncCoordinate;
        if (order) order->push_back(3);
    };
    callbacks.syncApply = [&counts, order](const ControlTickContext&) {
        ++counts.syncApply;
        if (order) order->push_back(4);
    };
    callbacks.waveform = [&counts, order](const ControlTickContext&) {
        ++counts.waveform;
        if (order) order->push_back(5);
    };
    callbacks.feedback = [&counts, order](const ControlTickContext&) {
        ++counts.feedback;
        if (order) order->push_back(6);
    };
    callbacks.display = [&counts](const ControlTickContext&) { ++counts.display; };
    callbacks.meters = [&counts](const ControlTickContext&) { ++counts.meters; };
    callbacks.statistics = [&counts, order](const ControlTickContext&) {
        ++counts.statistics;
        if (order) order->push_back(7);
    };
    callbacks.housekeeping = [&counts, order](const ControlTickContext&) {
        ++counts.housekeeping;
        if (order) order->push_back(8);
    };
    return callbacks;
}

void basicAndRates()
{
    ControlClock clock;
    assert(!clock.isRunning());
    clock.start();
    assert(clock.isRunning());
    clock.start();
    assert(clock.isRunning());
    clock.stop();
    clock.stop();
    assert(!clock.isRunning());

    Counts counts;
    auto registration = clock.registerCallbacks(callbacksFor(counts));
    assert(registration.valid());
    constexpr int baseTicks = 250;
    for (int index = 0; index < baseTicks; ++index)
        clock.advanceForTesting(0.004);

    assert(counts.fast == 250);
    assert(counts.transport >= 124 && counts.transport <= 126);
    assert(counts.syncInput == counts.transport);
    assert(counts.syncCoordinate == counts.syncInput);
    assert(counts.syncApply == counts.syncInput);
    assert(counts.waveform >= 59 && counts.waveform <= 61);
    assert(counts.feedback >= 29 && counts.feedback <= 31);
    assert(counts.display >= 59 && counts.display <= 61);
    assert(counts.meters >= 29 && counts.meters <= 31);
    assert(counts.statistics >= 9 && counts.statistics <= 11);
    assert(counts.housekeeping >= 1 && counts.housekeeping <= 3);
}

void orderingAndRegistration()
{
    ControlClock clock;
    Counts counts;
    std::vector<int> order;
    auto registration = clock.registerCallbacks(callbacksFor(counts, &order));
    clock.advanceForTesting(0.004);
    const std::vector<int> expected {1, 2, 3, 4, 5, 6, 7, 8};
    assert(order == expected);

    registration.reset();
    const int callsBefore = counts.fast;
    clock.advanceForTesting(0.004);
    assert(counts.fast == callsBefore);

    Counts fourCounts[4];
    std::array<ControlClock::Registration, 4> registrations;
    for (int index = 0; index < 4; ++index)
        registrations[index] = clock.registerCallbacks(callbacksFor(fourCounts[index]));
    clock.advanceForTesting(0.004);
    for (const auto& value : fourCounts)
        assert(value.fast == 1);
    registrations[2].reset();
    clock.advanceForTesting(0.004);
    assert(fourCounts[2].fast == 1);
    assert(fourCounts[0].fast == 2);
}

void latenessAndCoalescing()
{
    ControlClock clock;
    Counts counts;
    auto registration = clock.registerCallbacks(callbacksFor(counts));
    clock.advanceForTesting(0.004);
    const Counts before = counts;
    clock.advanceForTesting(2.0);
    assert(counts.fast == before.fast + 1);
    assert(counts.transport == before.transport + 1);
    assert(counts.syncInput == before.syncInput + 1);
    assert(counts.waveform == before.waveform);
    assert(counts.feedback == before.feedback);
    const auto stats = clock.stats();
    assert(stats.lateTicks == 1);
    assert(stats.skippedWaveformTicks == 1);
    assert(stats.skippedFeedbackTicks == 1);
    assert(stats.skippedDisplayTicks == 1);
    assert(stats.skippedMeterTicks == 1);
    assert(stats.skippedLinkTicks == 1);
    assert(stats.skippedStatisticsTicks == 1);
    assert(stats.skippedHousekeepingTicks == 1);

    double lastPosition = 0.0;
    int positionSignals = 0;
    auto publishPosition = [&](double position) {
        if (std::abs(position - lastPosition) <= 0.001)
            return;
        lastPosition = position;
        ++positionSignals;
    };
    publishPosition(0.0005);
    publishPosition(0.002);
    publishPosition(0.0024);
    assert(positionSignals == 1);

    int lastMidi = -1;
    int midiMessages = 0;
    auto sendMidi = [&](int value) {
        if (value == lastMidi) return;
        lastMidi = value;
        ++midiMessages;
    };
    sendMidi(64); sendMidi(64); sendMidi(65);
    assert(midiMessages == 2);
}

void performance()
{
    constexpr int iterations = 100'000;
    auto measureScenario = [](int deckCount, bool playing, bool sync, bool waveformAndMidi) {
        ControlClock clock;
        std::array<ControlClock::Registration, 4> registrations;
        std::array<std::uint64_t, 4> sinks {};
        for (int deck = 0; deck < deckCount; ++deck) {
            ControlClock::Callbacks callbacks;
            callbacks.transport = [&, deck](const ControlTickContext&) { ++sinks[deck]; };
            if (playing)
                callbacks.fast = [&, deck](const ControlTickContext&) { ++sinks[deck]; };
            if (sync) {
                callbacks.syncInput = [&, deck](const ControlTickContext&) { ++sinks[deck]; };
                callbacks.syncApply = [&, deck](const ControlTickContext&) { ++sinks[deck]; };
            }
            if (waveformAndMidi) {
                callbacks.waveform = [&, deck](const ControlTickContext&) { ++sinks[deck]; };
                callbacks.feedback = [&, deck](const ControlTickContext&) { ++sinks[deck]; };
            }
            registrations[deck] = clock.registerCallbacks(std::move(callbacks));
        }
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < iterations; ++index)
            clock.advanceForTesting(0.004);
        assert(sinks[0] > 0 || deckCount == 0);
        return std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count() / iterations;
    };

    ControlClock empty;
    auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index)
        empty.advanceForTesting(0.004);
    const double emptyUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count() / iterations;

    ControlClock loaded;
    std::array<Counts, 4> counts;
    std::array<ControlClock::Registration, 4> registrations;
    for (int index = 0; index < 4; ++index)
        registrations[index] = loaded.registerCallbacks(callbacksFor(counts[index]));
    start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index)
        loaded.advanceForTesting(0.004);
    const double fourDeckUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count() / iterations;
    const auto stats = loaded.stats();
    std::cout << "PERF control_clock_empty_tick_us=" << emptyUs
              << " one_deck_paused_us=" << measureScenario(1, false, false, false)
              << " four_decks_paused_us=" << measureScenario(4, false, false, false)
              << " four_decks_playing_us=" << measureScenario(4, true, false, false)
              << " four_decks_sync_us=" << measureScenario(4, true, true, false)
              << " four_decks_waveform_midi_us=" << measureScenario(4, true, true, true)
              << " control_clock_four_deck_tick_us=" << fourDeckUs
              << " measured_average_us=" << stats.averageTickDurationMicros
              << " measured_worst_us=" << stats.worstTickDurationMicros
              << " max_callbacks=" << stats.maxCallbacksPerTick << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    basicAndRates();
    orderingAndRegistration();
    latenessAndCoalescing();
    performance();
    std::cout << "ControlClock tests passed\n";
    return 0;
}
