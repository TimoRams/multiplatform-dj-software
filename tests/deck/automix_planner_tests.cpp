#include "deck/automix/AutomixPlanner.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace brockdj::automix;

namespace {

int g_failures = 0;

void expectNear(const char* what, double actual, double expected, double tolerance = 1e-6)
{
    if (std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL %s: got %.6f expected %.6f\n", what, actual, expected);
        ++g_failures;
    }
}

void expectTrue(const char* what, bool condition)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

TrackTimeline gridTrack(double duration, double bpm, double firstDownbeat)
{
    TrackTimeline track;
    track.durationSec = duration;
    track.bpm = bpm;
    const double bar = 240.0 / bpm;
    for (double t = firstDownbeat; t < duration; t += bar)
        track.downbeats.push_back(t);
    return track;
}

void testLatestPhraseWithoutOutro()
{
    const auto out = gridTrack(300.0, 128.0, 0.5);
    const auto in = gridTrack(240.0, 128.0, 0.2);
    AutomixSettings settings;   // 16 bars = 30 s at 128 BPM
    const auto plan = planTransition(out, in, settings, 0.0);
    expectTrue("phrase plan valid", plan.valid && plan.beatAligned);
    expectTrue("phrase anchor", plan.anchor == TransitionAnchor::Phrase);
    expectTrue("phrase bars", plan.bars == 16);
    // Latest bar index that is a multiple of 8 and still ends before 299.75 s.
    int best = -1;
    for (int i = 0; i < static_cast<int>(out.downbeats.size()); ++i)
        if (i % 8 == 0 && out.downbeats[i] + 30.0 <= 299.75 + 1e-6)
            best = i;
    expectNear("phrase start", plan.outStartSec, out.downbeats[best]);
    expectNear("phrase length", plan.outLengthSec, 30.0, 1e-6);
    expectNear("incoming cue", plan.inStartSec, 0.2);
    expectNear("same tempo ratio", plan.inPerOut, 1.0);
    expectNear("bass swap mid", plan.bassSwapProgress, 0.5);
    expectNear("bass swap one beat", plan.bassSwapWidth, 1.0 / 64.0);
}

void testOutroPreferred()
{
    auto out = gridTrack(300.0, 128.0, 0.5);
    out.phrases = {{0.0, 60.0, PhraseKind::Low},
                   {60.0, 240.0, PhraseKind::High},
                   {240.0, 262.0, PhraseKind::Medium},
                   {262.0, 300.0, PhraseKind::Low}};
    const auto in = gridTrack(240.0, 128.0, 0.2);
    const auto plan = planTransition(out, in, AutomixSettings {}, 0.0);
    expectTrue("outro anchor", plan.anchor == TransitionAnchor::Outro);
    // Nearest downbeat to 262 s: 0.5 + 140 * 1.875 = 263.0 or 261.125 -> 261.125.
    expectNear("outro start on bar", plan.outStartSec, 0.5 + 139 * 1.875);
    expectTrue("outro keeps full length", plan.bars == 16);

    // Once the playhead is past the outro start, the plan falls back to a later bar.
    const auto late = planTransition(out, in, AutomixSettings {}, 262.0);
    expectTrue("late plan not outro", late.anchor != TransitionAnchor::Outro);
    expectTrue("late plan in future", late.outStartSec >= 262.0 - 1e-6);
    expectTrue("late plan ends in time", late.outStartSec + late.outLengthSec <= 299.75 + 1e-6);
}

void testShortTailSilenceEndsFadeEarlier()
{
    auto out = gridTrack(300.0, 128.0, 0.5);
    out.phrases = {{0.0, 294.0, PhraseKind::High}, {294.0, 300.0, PhraseKind::Silence}};
    const auto plan = planTransition(out, gridTrack(200.0, 128.0, 0.0), AutomixSettings {}, 0.0);
    expectTrue("tail silence plan valid", plan.valid);
    expectTrue("fade done before silence", plan.outStartSec + plan.outLengthSec <= 294.0 + 1e-6);
}

void testTooLateShortens()
{
    const auto out = gridTrack(300.0, 128.0, 0.5);
    const auto in = gridTrack(240.0, 128.0, 0.0);
    const auto barPlan = planTransition(out, in, AutomixSettings {}, 290.0);
    expectTrue("minimum length still fits", barPlan.anchor == TransitionAnchor::Bar && barPlan.bars == 4);

    const auto plan = planTransition(out, in, AutomixSettings {}, 294.0);
    expectTrue("late valid", plan.valid);
    expectTrue("late anchored to next bar", plan.anchor == TransitionAnchor::Immediate);
    expectTrue("late starts after playhead", plan.outStartSec >= 294.0 - 1e-6);
    expectTrue("late shortened", plan.bars >= 1 && plan.bars < 16);
    expectTrue("late fits", plan.outStartSec + plan.outLengthSec <= 299.75 + 1e-6);

    const auto forced = planTransition(out, in, AutomixSettings {}, 100.0, true);
    expectTrue("forced immediate", forced.anchor == TransitionAnchor::Immediate);
    expectTrue("forced on next bar", forced.outStartSec >= 100.0 && forced.outStartSec < 100.0 + 1.875 + 1e-6);
    expectTrue("forced full length", forced.bars == 16);
}

void testNoGridFallsBackToTime()
{
    TrackTimeline out;
    out.durationSec = 200.0;
    TrackTimeline in;
    in.durationSec = 180.0;
    in.phrases = {{0.0, 3.0, PhraseKind::Silence}, {3.0, 180.0, PhraseKind::High}};
    const auto plan = planTransition(out, in, AutomixSettings {}, 0.0);
    expectTrue("time plan valid", plan.valid && !plan.beatAligned);
    expectTrue("time anchor", plan.anchor == TransitionAnchor::Time);
    expectNear("time start", plan.outStartSec, 200.0 - 0.25 - 12.0);
    expectNear("time length", plan.outLengthSec, 12.0);
    expectNear("skip leading silence", plan.inStartSec, 3.0);
}

void testIntroAndTempo()
{
    auto in = gridTrack(200.0, 125.0, 0.1);
    in.phrases = {{0.0, 3.9, PhraseKind::Silence}, {3.9, 200.0, PhraseKind::Medium}};
    const double firstAfterSilence = 0.1 + 2 * 240.0 / 125.0;   // 3.94 s
    expectNear("intro after silence", introStartSec(in), firstAfterSilence);

    const auto out = gridTrack(300.0, 128.0, 0.0);
    expectNear("tempo ratio", planTransition(out, in, AutomixSettings {}, 0.0).inPerOut, 128.0 / 125.0);
    const auto halfTime = gridTrack(200.0, 64.0, 0.0);
    expectNear("half-time ratio", planTransition(out, halfTime, AutomixSettings {}, 0.0).inPerOut, 1.0);

    TransitionPlan plan;
    plan.inStartSec = 4.0;
    plan.outStartSec = 100.0;
    plan.inPerOut = 1.024;
    expectNear("phase aligned incoming position", incomingPositionFor(plan, 110.0), 4.0 + 10.24);
}

void testLevels()
{
    TransitionPlan plan;
    plan.bars = 16;
    plan.bassSwapProgress = 0.5;
    plan.bassSwapWidth = 1.0 / 64.0;
    const auto start = transitionLevelsAt(plan, 0.0);
    expectNear("start incoming", start.incoming, 0.0);
    expectNear("start in low killed", start.inLowKill, 1.0);
    expectNear("start out low kept", start.outLowKill, 0.0);
    const auto beforeSwap = transitionLevelsAt(plan, 0.49);
    expectNear("before swap in low killed", beforeSwap.inLowKill, 1.0);
    const auto afterSwap = transitionLevelsAt(plan, 0.5 + 1.0 / 64.0);
    expectNear("after swap in low open", afterSwap.inLowKill, 0.0);
    expectNear("after swap out low killed", afterSwap.outLowKill, 1.0);
    expectNear("swap point is centre", transitionLevelsAt(plan, 0.5).incoming, 0.5);
    expectNear("end incoming", transitionLevelsAt(plan, 1.0).incoming, 1.0);
    double previous = -1.0;
    bool monotonic = true;
    for (int i = 0; i <= 100; ++i) {
        const double value = transitionLevelsAt(plan, i / 100.0).incoming;
        monotonic &= value >= previous - 1e-12;
        previous = value;
    }
    expectTrue("crossfade monotonic", monotonic);
}

void testLabels()
{
    expectTrue("label silence", phraseKindFromLabel("SilenceOrOutro") == PhraseKind::Silence);
    expectTrue("label low", phraseKindFromLabel("LowEnergy") == PhraseKind::Low);
    expectTrue("label medium", phraseKindFromLabel("MediumEnergy") == PhraseKind::Medium);
    expectTrue("label high", phraseKindFromLabel("HighEnergy") == PhraseKind::High);
    expectTrue("label outro", phraseKindFromLabel("Outro") == PhraseKind::Low);
    expectTrue("label build", phraseKindFromLabel("Build") == PhraseKind::Medium);
    expectTrue("label verse", phraseKindFromLabel("Verse") == PhraseKind::Medium);
    expectTrue("label drop", phraseKindFromLabel("Drop") == PhraseKind::High);
    expectTrue("label unknown", phraseKindFromLabel("Phrase") == PhraseKind::Unknown);
    expectTrue("label null", phraseKindFromLabel(nullptr) == PhraseKind::Unknown);
}

} // namespace

int main()
{
    testLatestPhraseWithoutOutro();
    testOutroPreferred();
    testShortTailSilenceEndsFadeEarlier();
    testTooLateShortens();
    testNoGridFallsBackToTime();
    testIntroAndTempo();
    testLevels();
    testLabels();
    if (g_failures == 0)
        std::puts("automix_planner: all tests passed");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
