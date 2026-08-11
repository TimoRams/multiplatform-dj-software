#include "deck/DeckCueLoopController.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
}

int main()
{
    bool ok = true;
    DeckCueLoopController controller;

    controller.beginTrack(7);
    ok &= require(controller.trackGeneration() == 7, "track generation is retained");
    ok &= require(controller.mainCue().positionSec == DeckCueLoopController::kUnsetMainCue,
                  "main cue resets to the unset sentinel");

    for (int i = 0; i < DeckCueLoopController::kSlotCount; ++i) {
        ok &= require(controller.setHotCue(i, i - 2.0), "all eight hot-cue slots can be set");
        ok &= require(controller.hotCues()[static_cast<size_t>(i)].set,
                      "set hot cue is visible in controller state");
    }
    ok &= require(!controller.setHotCue(-1, 0.0), "negative hot-cue index is rejected");
    ok &= require(!controller.setHotCue(8, 0.0), "out-of-range hot-cue index is rejected");
    ok &= require(!controller.setHotCue(0, std::numeric_limits<double>::infinity()),
                  "infinite hot-cue position is rejected");
    ok &= require(controller.setHotCue(0, -3.0, QStringLiteral("Intro"), QStringLiteral("#123456")),
                  "pre-roll hot cue with name and color is accepted");
    ok &= require(controller.clearHotCue(0), "hot cue can be cleared");
    ok &= require(!controller.clearHotCue(0), "unchanged hot cue reports no change");

    ok &= require(controller.setLoop(-2.0, 2.0, 8.0), "pre-roll loop is accepted");
    ok &= require(controller.activeLoop().active && controller.activeLoop().inSet,
                  "loop activation state is consistent");
    ok &= require(controller.deactivateLoop(), "active loop can be deactivated");
    ok &= require(controller.reactivateLoop(), "valid inactive loop can be reactivated");
    ok &= require(!controller.setLoop(4.0, 3.0, 4.0), "inverted loop is rejected");
    ok &= require(!controller.setLoop(0.0, std::numeric_limits<double>::quiet_NaN(), 4.0),
                  "NaN loop boundary is rejected");
    ok &= require(controller.setLoop(0.0, 1.0, 1e9), "finite loop with oversized beat count is accepted");
    ok &= require(near(controller.activeLoop().lengthBeats, 4096.0), "loop beats clamp to maximum");

    ok &= require(controller.setSavedLoop(0, -1.0, 3.0, 8.0,
                                          QStringLiteral("Build"), QStringLiteral("#abcdef")),
                  "saved loop stores boundaries, name, and color");
    ok &= require(!controller.setSavedLoop(8, 0.0, 1.0, 1.0),
                  "invalid saved-loop index is rejected");
    ok &= require(controller.clearSavedLoop(0), "saved loop can be cleared");

    DeckCueLoopController::BeatGridSnapshot dynamicGrid{{0.0, 0.5, 1.1, 1.8}, 120.0, 0.0};
    ok &= require(near(DeckCueLoopController::quantizedBeatAt(0.9, dynamicGrid), 1.1),
                  "quantize uses dynamic beat-grid markers");
    ok &= require(near(DeckCueLoopController::beatDurationAround(1.2, dynamicGrid), 0.7),
                  "local beat duration follows the dynamic grid");
    ok &= require(near(DeckCueLoopController::nextBeatBoundaryAfter(1.1, dynamicGrid), 1.8),
                  "next boundary skips the current marker");
    DeckCueLoopController::BeatGridSnapshot fallback{{}, 120.0, -0.25};
    ok &= require(near(DeckCueLoopController::quantizedBeatAt(-0.1, fallback), -0.25),
                  "fallback quantize preserves a negative first beat");

    controller.scheduleCueJump(5.0, 2.0, 1.0);
    ok &= require(!controller.serviceCueJump(1.5), "pending cue waits for its beat boundary");
    ok &= require(controller.serviceCueJump(2.0) == 5.0, "pending cue fires at its boundary");
    controller.scheduleCueJump(9.0, 4.0, 3.0);
    controller.beginTrack(8);
    ok &= require(!controller.serviceCueJump(5.0), "track change invalidates old cue command");
    ok &= require(!controller.activeLoop().active && !controller.hotCues()[1].set,
                  "track change clears deck-local loop and cue state");

    for (int i = 0; i < 1000; ++i) {
        ok &= require(controller.setLoop(i * 0.01, i * 0.01 + 0.5, 1.0),
                      "rapid valid loop commands preserve state");
        controller.deactivateLoop();
        controller.reactivateLoop();
    }
    return ok ? 0 : 1;
}
