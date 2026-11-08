#include "controllers/midi/MidiClockScheduler.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace brockdj::midiclock;

namespace {

int g_failures = 0;

void expectTrue(const char* what, bool condition)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

void expectNear(const char* what, double actual, double expected, double tolerance)
{
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::fprintf(stderr, "FAIL %s: got %.6f expected %.6f\n", what, actual, expected);
        ++g_failures;
    }
}

struct Event {
    std::uint8_t status;
    double time;
};

// Drives the scheduler like the output thread: sleeps until the returned wake
// time, while the master snapshot is refreshed at controlRate with optional jitter.
std::vector<Event> run(MidiClockScheduler& scheduler, double seconds, double bpm,
                       double startBeat, bool running, double jitterBeats = 0.0,
                       double beatJumpAt = -1.0, double beatJump = 0.0)
{
    std::vector<Event> events;
    double now = 0.0;
    double nextSnapshot = 0.0;
    ClockSnapshot snapshot;
    unsigned noise = 12345;
    bool jumped = false;
    while (now < seconds) {
        if (now >= nextSnapshot) {
            noise = noise * 1103515245u + 12345u;
            const double n = (static_cast<double>((noise >> 8) & 0xffff) / 65535.0 - 0.5) * 2.0;
            double beat = startBeat + now * bpm / 60.0 + n * jitterBeats;
            if (beatJumpAt >= 0.0 && now >= beatJumpAt) {
                beat += beatJump;
                jumped = true;
            }
            snapshot = {true, running, bpm, beat, now, jumped ? 2u : 1u};
            nextSnapshot += 1.0 / 125.0;
        }
        const double wake = scheduler.advance(snapshot, now,
                                              [&](std::uint8_t status) { events.push_back({status, now}); });
        now = std::max(now + 1e-6, std::min(wake, nextSnapshot));
    }
    return events;
}

std::vector<double> clockTimes(const std::vector<Event>& events)
{
    std::vector<double> times;
    for (const auto& event : events)
        if (event.status == kClock)
            times.push_back(event.time);
    return times;
}

void testSteadyPulseRate()
{
    MidiClockScheduler scheduler;
    const auto events = run(scheduler, 4.0, 120.0, 0.0, false);
    const auto times = clockTimes(events);
    // 120 BPM = 2 beats/s = 48 pulses/s.
    expectTrue("steady pulse count", std::abs(static_cast<int>(times.size()) - 192) <= 1);
    double worst = 0.0;
    for (std::size_t i = 1; i < times.size(); ++i)
        worst = std::max(worst, std::abs((times[i] - times[i - 1]) - 1.0 / 48.0));
    expectTrue("steady pulse spacing within 0.1 ms", worst < 1e-4);
    for (const auto& event : events)
        expectTrue("no transport while stopped", event.status == kClock);
}

void testStartQuantisedToBar()
{
    MidiClockScheduler scheduler;
    // Start at beat 1.5: START must wait for beat 4 (pulse 96).
    const auto events = run(scheduler, 2.0, 120.0, 1.5, true);
    std::size_t clocksBeforeStart = 0;
    bool started = false;
    for (const auto& event : events) {
        if (event.status == kStart) { started = true; break; }
        if (event.status == kClock) ++clocksBeforeStart;
    }
    expectTrue("start emitted", started);
    // Pulses 37..95 precede START (beat 1.5 → pulse 36 is the sync point).
    expectTrue("start lands on bar", clocksBeforeStart == 59);
    expectTrue("transport running", scheduler.transportRunning());
}

void testStopAndTransportDisable()
{
    MidiClockScheduler scheduler;
    std::vector<std::uint8_t> statuses;
    const auto emit = [&](std::uint8_t status) { statuses.push_back(status); };
    ClockSnapshot snapshot {true, true, 120.0, 3.99, 0.0, 1};
    double now = 0.0;
    for (int i = 0; i < 200; ++i, now += 0.002) {
        snapshot.beat = 3.99 + now * 2.0;
        snapshot.timestamp = now;
        scheduler.advance(snapshot, now, emit);
    }
    expectTrue("started", scheduler.transportRunning());
    snapshot.running = false;
    scheduler.advance(snapshot, now, emit);
    expectTrue("stop emitted", !statuses.empty() && statuses.back() == kStop);
    expectTrue("not running after stop", !scheduler.transportRunning());

    statuses.clear();
    scheduler.setTransportEnabled(false);
    snapshot.running = true;
    for (int i = 0; i < 400; ++i, now += 0.002) {
        snapshot.beat = 3.99 + now * 2.0;
        snapshot.timestamp = now;
        scheduler.advance(snapshot, now, emit);
    }
    bool transport = false;
    for (auto status : statuses)
        transport |= status != kClock;
    expectTrue("transport disabled sends clock only", !transport && !statuses.empty());
}

void testJitterIsSmoothed()
{
    MidiClockScheduler scheduler;
    // ±3 ms of master jitter at 120 BPM.
    const auto times = clockTimes(run(scheduler, 6.0, 120.0, 0.0, false, 0.006));
    double worst = 0.0;
    for (std::size_t i = 48; i < times.size(); ++i)
        worst = std::max(worst, std::abs((times[i] - times[i - 1]) - 1.0 / 48.0));
    expectTrue("jitter smoothed below 1 ms per pulse", worst < 0.001);
}

void testLoopJumpResyncsWithoutBurst()
{
    MidiClockScheduler scheduler;
    // Deck loops back 4 beats at t=2 s (new generation).
    const auto times = clockTimes(run(scheduler, 4.0, 120.0, 0.0, false, 0.0, 2.0, -4.0));
    double smallest = 1.0;
    for (std::size_t i = 1; i < times.size(); ++i)
        smallest = std::min(smallest, times[i] - times[i - 1]);
    expectTrue("no pulse burst after loop", smallest > 0.5 / 48.0);
    expectTrue("pulses continue after loop", std::abs(static_cast<int>(times.size()) - 192) <= 2);
}

void testForwardJumpDoesNotBurst()
{
    MidiClockScheduler scheduler;
    const auto times = clockTimes(run(scheduler, 3.0, 120.0, 0.0, false, 0.0, 1.0, 16.0));
    double smallest = 1.0;
    for (std::size_t i = 1; i < times.size(); ++i)
        smallest = std::min(smallest, times[i] - times[i - 1]);
    expectTrue("no pulse burst after forward jump", smallest > 0.5 / 48.0);
}

void testInvalidSnapshotStops()
{
    MidiClockScheduler scheduler;
    std::vector<std::uint8_t> statuses;
    const auto emit = [&](std::uint8_t status) { statuses.push_back(status); };
    double now = 0.0;
    for (int i = 0; i < 1200; ++i, now += 0.002)
        scheduler.advance({true, true, 120.0, now * 2.0, now, 1}, now, emit);
    expectTrue("running before invalid", scheduler.transportRunning());
    statuses.clear();
    scheduler.advance({false, true, 0.0, 0.0, now, 1}, now, emit);
    expectTrue("invalid snapshot sends stop", statuses.size() == 1 && statuses[0] == kStop);
}

void testFollowerTempoAndBeat()
{
    MidiClockFollower follower;
    const double bpm = 133.0;
    const double period = 60.0 / (bpm * kPulsesPerBeat);
    unsigned noise = 99;
    follower.handle(kStart, 0.0);
    double t = 0.0;
    for (int i = 0; i < 24 * 8; ++i) {
        noise = noise * 1664525u + 1013904223u;
        const double jitter = (static_cast<double>((noise >> 8) & 0xffff) / 65535.0 - 0.5) * 0.002;
        follower.handle(kClock, t + jitter);
        t += period;
    }
    const auto snapshot = follower.snapshot(t);
    expectTrue("follower valid", snapshot.valid);
    expectTrue("follower running", snapshot.running);
    expectNear("follower tempo", snapshot.bpm, bpm, 0.15);
    expectNear("follower beat", snapshot.beat, (24.0 * 8 - 1) / 24.0, 1e-9);

    follower.handle(kStop, t);
    expectTrue("follower stopped", !follower.snapshot(t).running);
    expectTrue("follower times out", !follower.snapshot(t + 1.0).valid);

    const auto generation = follower.snapshot(t).generation;
    follower.handle(kSongPosition, t, 16, 0); // 16 sixteenths = 4 beats
    follower.handle(kClock, t + period);
    const auto moved = follower.snapshot(t + period);
    expectNear("song position", moved.beat, 4.0, 1e-9);
    expectTrue("song position new generation", moved.generation != generation);
}

void testFollowerNeedsPulses()
{
    MidiClockFollower follower;
    follower.handle(kClock, 0.0);
    follower.handle(kClock, 0.02);
    expectTrue("too few pulses invalid", !follower.snapshot(0.03).valid);
}

} // namespace

int main()
{
    testSteadyPulseRate();
    testStartQuantisedToBar();
    testStopAndTransportDisable();
    testJitterIsSmoothed();
    testLoopJumpResyncsWithoutBurst();
    testForwardJumpDoesNotBurst();
    testInvalidSnapshotStops();
    testFollowerTempoAndBeat();
    testFollowerNeedsPulses();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("midi_clock: all tests passed");
    return EXIT_SUCCESS;
}
