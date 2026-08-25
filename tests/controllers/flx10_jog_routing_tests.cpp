#include "controllers/flx10/Flx10JogRouter.h"
#include "controllers/flx10/Flx10RealtimeScratchIngress.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

bool near(double actual, double expected, double tolerance = 1.0e-9)
{
    return std::abs(actual - expected) <= tolerance;
}

double ticksForRate(double rate, double elapsedSeconds)
{
    return rate * elapsedSeconds / flx10::scratchDeltaSeconds(1.0);
}

flx10::JogRouteResult route(flx10::Flx10JogRouter& router,
                            flx10::JogEventType type,
                            double timestampSeconds,
                            double ticks = 0.0,
                            bool engineReleaseActive = false)
{
    return router.route({type, ticks, timestampSeconds, engineReleaseActive});
}

bool testRelativeTickDecoding()
{
    bool ok = true;
    ok &= require(flx10::relativeTicksFromRaw(64) == 0,
                  "raw 64 remains the neutral relative-encoder value");
    ok &= require(flx10::relativeTicksFromRaw(65) == 1
                      && flx10::relativeTicksFromRaw(66) == 2
                      && flx10::relativeTicksFromRaw(63) == -1,
                  "observed FLX10 values decode without changing their sign");
    ok &= require(flx10::relativeTicksFromRaw(-20) == -64
                      && flx10::relativeTicksFromRaw(200) == 63,
                  "relative raw values are clamped before decoding");
    return ok;
}

bool testPhysicalRevolutionCalibration()
{
    constexpr double vinylRevolutionSeconds = 60.0 / flx10::kVinylRpm;
    return require(near(
                       flx10::scratchDeltaSeconds(
                           flx10::kScratchIntervalsPerRevolution),
                       vinylRevolutionSeconds),
                   "one measured FLX10 revolution maps to one virtual revolution");
}

bool testTimestampedSpeedMeasurement()
{
    using enum flx10::JogEventType;
    using enum flx10::JogRouteAction;
    bool ok = true;

    struct Case {
        double elapsedSeconds;
        double ticks;
    };

    for (const Case test : {
             Case {0.001, ticksForRate(1.2, 0.001)},
             Case {0.002, ticksForRate(1.2, 0.002)},
             Case {0.004, ticksForRate(1.2, 0.004)}}) {
        flx10::Flx10JogRouter router;
        ok &= require(route(router, TouchDown, 10.0).action == BeginScratch,
                      "touch-down begins scratch tracking");
        const auto motion = route(router,
                                  Platter,
                                  10.0 + test.elapsedSeconds,
                                  test.ticks);
        ok &= require(motion.action == ScratchDelta,
                      "timestamped platter motion produces a scratch delta");
        ok &= require(near(motion.estimatedRate, 1.2),
                      "1, 2, and 4 ms events use their real timestamp interval");
        ok &= require(motion.eventIntervalSeconds == 0.0,
                      "the first accepted tick has no previous tick interval");
    }

    flx10::Flx10JogRouter batched;
    route(batched, TouchDown, 20.0);
    route(batched, Platter, 20.001, ticksForRate(1.2, 0.001));
    route(batched, Platter, 20.002, ticksForRate(1.2, 0.001));
    const auto batchedTail = route(batched, Platter, 20.004,
                                   ticksForRate(1.2, 0.002));
    ok &= require(near(batchedTail.estimatedRate, 1.2),
                  "batched delivery retains each event's original timestamp");
    ok &= require(near(batchedTail.eventIntervalSeconds, 0.002),
                  "diagnostics retain the interval between batched wheel events");

    const auto release = route(batched, TouchUp, 20.005);
    ok &= require(release.action == RequestRelease
                      && near(release.estimatedRate, 1.2),
                  "touch-up publishes the timestamped signed release rate");

    flx10::Flx10JogRouter delayedStart;
    route(delayedStart, TouchDown, 30.0);
    const double oneMillisecondTicks = ticksForRate(1.2, 0.001);
    ok &= require(near(route(delayedStart, Platter, 30.100,
                             oneMillisecondTicks).estimatedRate, 0.0),
                  "a first tick after a long touch hold starts a fresh speed window");
    ok &= require(near(route(delayedStart, Platter, 30.101,
                             oneMillisecondTicks).estimatedRate, 1.2),
                  "the second tick after a long hold measures only recent motion");

    flx10::JogSpeedEstimator estimator;
    estimator.reset(30.0);
    for (int i = 1; i <= 80; ++i)
        estimator.push(oneMillisecondTicks,
                       30.0 + static_cast<double>(i) * 0.001);
    ok &= require(estimator.sampleCount() <= flx10::JogSpeedEstimator::kCapacity,
                  "speed history never exceeds its fixed 32-sample capacity");
    ok &= require(estimator.sampleCount() <= 32,
                  "speed history uses no more than 32 events");
    ok &= require(near(estimator.rate(30.080), 1.2),
                  "the rolling 32 ms speed window remains accurate");
    ok &= require(estimator.rate(30.141) == 0.0,
                  "a speed estimate is stale after 60 ms without motion");
    ok &= require(estimator.push(oneMillisecondTicks, 30.200) == 0.0,
                  "the first tick after stale history starts a new window");
    ok &= require(near(estimator.push(oneMillisecondTicks, 30.201), 1.2),
                  "motion after stale history is measured from recent ticks only");

    return ok;
}

bool testReleaseRoutingAndCompletion()
{
    using enum flx10::JogEventType;
    using enum flx10::JogPhase;
    using enum flx10::JogRouteAction;
    bool ok = true;
    flx10::Flx10JogRouter router;

    ok &= require(route(router, Platter, 1.0, 1.0).action == Ignore
                      && router.phase() == Idle,
                  "an untouched platter packet cannot create a scratch grab");
    ok &= require(route(router, Rim, 1.001, 1.0).action == Nudge,
                  "rim motion is a normal nudge while idle");
    ok &= require(route(router, Generic, 1.002, -1.0).action == Nudge,
                  "generic jog motion remains a normal nudge while idle");

    ok &= require(route(router, TouchDown, 2.0).action == BeginScratch
                      && router.phase() == TouchTracking,
                  "touch-down owns the platter for scratch");
    ok &= require(route(router, Rim, 2.001, 1.0).action == Ignore,
                  "rim packets do not duplicate top-platter motion while touched");
    ok &= require(route(router, Platter, 2.002, 2.0).action == ScratchDelta,
                  "CC 0x22 is scratch motion while touched");

    const auto touchUp = route(router, TouchUp, 2.003);
    ok &= require(touchUp.action == RequestRelease
                      && touchUp.phase == ReleaseOwned,
                  "touch-up requests release without immediately yielding the wheel");
    ok &= require(route(router, Platter, 2.004, 1.0, true).action == Ignore,
                  "CC 0x22 cannot move the released platter");

    const auto coast = route(router, Rim, 2.005, 2.0, true);
    ok &= require(coast.action == ReleaseDelta
                      && coast.phase == ReleaseOwned,
                  "CC 0x21 continues physical release motion while the engine owns release");
    ok &= require(near(coast.eventIntervalSeconds, 0.003),
                  "release diagnostics bridge the last platter tick to the first rim tick");
    ok &= require(coast.deltaSeconds == flx10::scratchDeltaSeconds(2.0),
                  "release motion carries the one-to-one scratch delta");
    ok &= require(route(router, Generic, 2.006, 1.0, true).action == ReleaseDelta,
                  "generic jog motion follows the same owned-release path");

    const auto completed = route(router, Generic, 2.007, 0.0, false);
    ok &= require(completed.action == Ignore
                      && completed.phase == TailSuppression,
                  "the first inactive engine snapshot completes release into tail suppression");
    ok &= require(route(router, Platter, 2.008, -1.0, false).action == Ignore,
                  "late platter packets remain ignored after release completion");
    return ok;
}

bool testSlowReleaseTailSuppression()
{
    using enum flx10::JogEventType;
    using enum flx10::JogPhase;
    using enum flx10::JogRouteAction;
    bool ok = true;
    flx10::Flx10JogRouter router;

    route(router, TouchDown, 4.0);
    const auto slowMotion = route(router, Platter, 4.004,
                                  ticksForRate(0.3, 0.004));
    ok &= require(near(slowMotion.estimatedRate, 0.3),
                  "a genuinely slow four-millisecond tick is not promoted above deck rate");
    ok &= require(route(router, TouchUp, 4.005).action == RequestRelease,
                  "slow top motion still publishes a release decision");

    route(router, Generic, 4.006, 0.0, false);
    ok &= require(router.phase() == TailSuppression,
                  "an immediately completed slow release enters tail suppression");
    ok &= require(route(router, Rim, 4.050, 1.0).action == Ignore,
                  "slow physical tail packets do not become a post-release nudge");
    ok &= require(route(router, Rim, 4.160, 1.0).action == Ignore,
                  "each suppressed tail packet extends the wheel-stillness interval");
    ok &= require(route(router, Generic, 4.270, 0.0).phase == TailSuppression,
                  "less than 120 ms of wheel stillness keeps suppression active");

    const auto expired = route(router, Generic, 4.281, 0.0);
    ok &= require(expired.phase == Idle,
                  "120 ms of wheel stillness returns the router to idle");
    ok &= require(route(router, Rim, 4.282, 1.0).action == Nudge,
                  "fresh rim motion after tail suppression is a normal nudge");
    return ok;
}

bool testDuplicateTouchAndRegrab()
{
    using enum flx10::JogEventType;
    using enum flx10::JogPhase;
    using enum flx10::JogRouteAction;
    bool ok = true;
    flx10::Flx10JogRouter router;

    ok &= require(route(router, TouchDown, 6.0).action == BeginScratch,
                  "first touch-down begins scratch");
    const double oneMillisecondTicks = ticksForRate(1.2, 0.001);
    route(router, Platter, 6.001, oneMillisecondTicks);
    ok &= require(route(router, TouchDown, 6.0015).action == Ignore,
                  "duplicate touch-down is idempotent");
    const auto afterDuplicate = route(router, Platter, 6.002,
                                      oneMillisecondTicks);
    ok &= require(near(afterDuplicate.estimatedRate, 1.2),
                  "duplicate touch-down does not reset the speed history");

    ok &= require(route(router, TouchUp, 6.003).action == RequestRelease,
                  "first touch-up requests release");
    ok &= require(route(router, TouchUp, 6.0035, 0.0, true).action == Ignore
                      && router.phase() == ReleaseOwned,
                  "duplicate touch-up cannot publish a second release");

    const auto regrab = route(router, TouchDown, 6.004, 0.0, true);
    ok &= require(regrab.action == BeginScratch
                      && regrab.phase == TouchTracking,
                  "touch-down during release immediately transfers ownership to a new grab");
    const auto regrabMotion = route(router, Platter, 6.005,
                                    -oneMillisecondTicks);
    ok &= require(near(regrabMotion.estimatedRate, -1.2),
                  "regrab starts a fresh signed speed history at the current timestamp");

    route(router, TouchUp, 6.006);
    route(router, Generic, 6.007, 0.0, false);
    const auto regrabTail = route(router, TouchDown, 6.008);
    ok &= require(regrabTail.action == BeginScratch
                      && regrabTail.phase == TouchTracking,
                  "touch-down also cancels tail suppression without waiting 120 ms");
    return ok;
}

bool testReleaseCompletionAfterWheelSilence()
{
    using enum flx10::JogEventType;
    using enum flx10::JogRouteAction;
    bool ok = true;
    flx10::Flx10JogRouter router;

    route(router, TouchDown, 50.0);
    route(router, Platter, 50.001, 1.0);
    route(router, Platter, 50.002, 1.0);
    route(router, TouchUp, 50.003);

    const auto freshSpin = route(router, Rim, 51.0, 1.0, false);
    ok &= require(freshSpin.action == Nudge,
                  "a new spin after wheel silence is not swallowed as release tail");
    return ok;
}

// A slow, deliberate movement is the hardest case to measure: single ticks
// arrive far apart and their timestamps are quantised by the USB frame the
// packet happened to land in. A window of fixed duration then holds one or two
// ticks and the rate swings with that quantisation — which is heard directly,
// because the audio read head is driven by it.
bool testSlowSpeedRateResolution()
{
    using enum flx10::JogEventType;
    bool ok = true;
    flx10::Flx10JogRouter router;
    route(router, TouchDown, 0.0);

    constexpr double kRate = 0.05;
    constexpr double kFrameSec = 0.001;   // USB frame granularity
    // One tick at a time, each timestamp snapped to the frame it arrived in.
    const double tickSeconds = std::abs(flx10::scratchDeltaSeconds(1.0) / kRate);

    double worst = 0.0;
    double best = 1.0e9;
    double exact = 0.0;
    for (int i = 1; i <= 400; ++i) {
        exact += tickSeconds;
        const double stamped = std::floor(exact / kFrameSec) * kFrameSec;
        const auto result = route(router, Platter, stamped, 1.0);
        if (i < 40)   // let the window fill
            continue;
        worst = std::max(worst, result.estimatedRate);
        best = std::min(best, result.estimatedRate);
    }

    // Spread of the measured rate around the true one. Deriving the rate from a
    // single tick over a single frame-quantised gap gives well over 50% here.
    const double spread = (worst - best) / kRate;
    ok &= require(spread < 0.15,
                  "slow platter speed resolves without frame-quantisation swing");
    ok &= require(best > kRate * 0.85 && worst < kRate * 1.15,
                  "slow platter speed stays centred on the true rate");
    return ok;
}

bool testBatchedReceiveTimestampsAndReversal()
{
    using enum flx10::JogEventType;
    bool ok = true;
    flx10::Flx10JogRouter router;
    route(router, TouchDown, 70.0);

    const double oneMsTicks = ticksForRate(1.0, 0.001);
    for (int i = 1; i <= 9; ++i)
        route(router, Platter, 70.0 + i * 0.001, oneMsTicks);

    // Three USB packets drained in one scheduler wake-up. Production ALSA
    // timestamps these a few microseconds apart; they still represent three
    // milliseconds of platter travel and may neither collapse to zero nor
    // explode to the speed clamp.
    route(router, Platter, 70.012000, oneMsTicks);
    route(router, Platter, 70.012010, oneMsTicks);
    const auto burstTail = route(router, Platter, 70.012020, oneMsTicks);
    ok &= require(near(burstTail.estimatedRate, 1.0, 0.08),
                  "scheduler-batched FLX10 packets retain their aggregate rate");
    ok &= require(burstTail.eventIntervalSeconds == 0.0,
                  "sub-frame packets are marked as one coalesced observation");

    // A reversal must not be averaged with old-direction ticks. Position turns
    // immediately, so a still-positive velocity estimate makes the audio servo
    // fight the hand and produces the characteristic rubbery chirp.
    const auto reverse = route(router, Platter, 70.013, -oneMsTicks);
    ok &= require(reverse.estimatedRate < -0.8
                      && reverse.estimatedRate > -1.2,
                  "the first distinct reverse packet changes velocity sign immediately");

    flx10::Flx10JogRouter sameFrameTurn;
    route(sameFrameTurn, TouchDown, 71.0);
    route(sameFrameTurn, Platter, 71.001, oneMsTicks);
    route(sameFrameTurn, Platter, 71.002, oneMsTicks);
    const auto turnPacket = route(
        sameFrameTurn, Platter, 71.002010, -oneMsTicks);
    const auto turnBurstTail = route(
        sameFrameTurn, Platter, 71.002020, -oneMsTicks);
    ok &= require(turnPacket.estimatedRate == 0.0
                      && turnBurstTail.estimatedRate == 0.0,
                  "same-frame reversal is a zero-speed turn, not an artificial velocity spike");
    const auto reverseNextFrame = route(
        sameFrameTurn, Platter, 71.003, -oneMsTicks);
    ok &= require(reverseNextFrame.estimatedRate < -0.8
                      && reverseNextFrame.estimatedRate > -1.2,
                  "the frame after a coalesced turn resumes a clean reverse estimate");
    return ok;
}

bool testFastSpeedRejectsUsbFrameJitter()
{
    using enum flx10::JogEventType;
    bool ok = true;
    flx10::Flx10JogRouter router;
    route(router, TouchDown, 90.0);

    constexpr double kRate = 6.0;
    constexpr double kPhysicalInterval = 0.001;
    const double ticks = ticksForRate(kRate, kPhysicalInterval);
    // Each packet represents one millisecond of real platter travel, while the
    // receive timestamps alternate between short and long scheduler intervals.
    // A one-frame quotient swings between roughly 4.6x and 8.6x here.
    constexpr double receiveIntervals[] = {0.0007, 0.0013};
    double timestamp = 90.0;
    double lowest = 100.0;
    double highest = -100.0;
    for (int i = 0; i < 80; ++i) {
        timestamp += receiveIntervals[i & 1];
        const auto result = route(router, Platter, timestamp, ticks);
        if (i < 12)
            continue;
        lowest = std::min(lowest, result.estimatedRate);
        highest = std::max(highest, result.estimatedRate);
    }

    ok &= require(lowest > kRate * 0.94 && highest < kRate * 1.06,
                  "fast platter rate averages USB-frame jitter over a stable minimum window");

    // The averaging window may not delay a real turn. Its history is discarded
    // on the first opposite-direction packet, whose sign must be visible at once.
    const auto reverse = route(router, Platter, timestamp + 0.001, -ticks);
    ok &= require(reverse.estimatedRate < -kRate * 0.9,
                  "fast reversal bypasses the same-direction jitter window immediately");
    return ok;
}

bool testRealtimeScratchIngress()
{
    bool ok = true;
    flx10::Flx10RealtimeScratchIngress ingress;
    const auto stream = ingress.stream();
    constexpr std::uint64_t kPortA = 1;
    constexpr std::uint64_t kPortB = 2;

    ok &= require(ingress.touchDown(80.0, kPortA)
                      == flx10::RealtimeIngressResult::Accepted,
                  "native touch begins the realtime scratch generation");
    const double ticks = ticksForRate(0.5, 0.002);
    ok &= require(ingress.platter(ticks, 80.002, kPortA)
                      == flx10::RealtimeIngressResult::Accepted,
                  "native platter motion enters the realtime stream");

    auto first = stream->readForControlThread();
    ok &= require(first.touching() && first.generation != 0,
                  "audio snapshot identifies the active touch generation");
    ok &= require(near(first.cumulativeDeltaSeconds,
                       flx10::scratchDeltaSeconds(ticks)),
                  "audio snapshot retains exact one-to-one platter travel");
    ok &= require(near(first.velocity, 0.5),
                  "audio snapshot carries the timestamped platter velocity");

    ok &= require(ingress.platter(ticks, 80.003, kPortB)
                      == flx10::RealtimeIngressResult::MirroredDuplicate,
                  "a mirrored jog packet from another ALSA port is dropped");
    const auto afterDuplicate = stream->readForControlThread();
    ok &= require(near(afterDuplicate.cumulativeDeltaSeconds,
                       first.cumulativeDeltaSeconds),
                  "a mirrored packet cannot double the scratch distance");

    ok &= require(ingress.touchUp(80.004, kPortA)
                      == flx10::RealtimeIngressResult::Accepted,
                  "native touch-up publishes release without clearing trajectory");
    ok &= require(ingress.rim(ticks, 80.006, kPortA)
                      == flx10::RealtimeIngressResult::Accepted,
                  "post-release rim motion refreshes physical throw velocity");
    const auto released = stream->readForControlThread();
    ok &= require(!released.touching()
                      && released.phase == engine::scratch::RealtimeScratchPhase::Released
                      && released.motionSequence > first.motionSequence,
                  "released audio snapshot remains available for callback-owned inertia");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= testRelativeTickDecoding();
    ok &= testPhysicalRevolutionCalibration();
    ok &= testTimestampedSpeedMeasurement();
    ok &= testReleaseRoutingAndCompletion();
    ok &= testSlowReleaseTailSuppression();
    ok &= testDuplicateTouchAndRegrab();
    ok &= testReleaseCompletionAfterWheelSilence();
    ok &= testSlowSpeedRateResolution();
    ok &= testBatchedReceiveTimestampsAndReversal();
    ok &= testFastSpeedRejectsUsbFrameJitter();
    ok &= testRealtimeScratchIngress();
    return ok ? 0 : 1;
}
