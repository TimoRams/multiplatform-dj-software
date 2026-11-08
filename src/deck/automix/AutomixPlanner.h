#pragma once

#include <vector>

// Pure, Qt-free transition planning for Automix. The controller converts deck
// state into TrackTimelines, asks for a plan every tick until the transition
// starts, and then samples transitionLevelsAt() while it runs.
namespace brockdj::automix {

enum class PhraseKind { Silence, Low, Medium, High, Unknown };

struct PhraseSpan {
    double startSec = 0.0;
    double endSec = 0.0;
    PhraseKind kind = PhraseKind::Unknown;
};

struct TrackTimeline {
    double durationSec = 0.0;
    double bpm = 0.0;                 // native (unstretched) track tempo
    std::vector<double> downbeats;    // ascending bar starts in track seconds
    std::vector<PhraseSpan> phrases;  // ascending, from the phrase analyzer
};

struct AutomixSettings {
    int transitionBars = 16;
    int beatsPerBar = 4;
    int phraseBars = 8;
    int minimumBars = 4;
    double endMarginSec = 0.25;       // never fade out later than this before the end
    double fallbackSeconds = 12.0;    // plain crossfade length without a beat grid
};

enum class TransitionAnchor {
    Outro,       // starts where the outgoing track's outro phrase begins
    Phrase,      // starts on the latest phrase boundary that fits
    Bar,         // starts on a bar line (no phrase boundary fits)
    Immediate,   // forced or too late: next bar / now, shortened to what remains
    Time         // no beat grid: plain time-based crossfade
};

struct TransitionPlan {
    bool valid = false;
    bool beatAligned = false;
    TransitionAnchor anchor = TransitionAnchor::Time;
    double outStartSec = 0.0;     // outgoing track position where the mix starts
    double outLengthSec = 0.0;    // transition length in outgoing track seconds
    double inStartSec = 0.0;      // incoming cue (first downbeat after silence)
    double inPerOut = 1.0;        // incoming track seconds per outgoing second when synced
    int bars = 0;
    double bassSwapProgress = 0.5;   // progress at which low frequencies are swapped
    double bassSwapWidth = 0.1;      // progress span of the swap (one beat when aligned)
};

struct TransitionLevels {
    double incoming = 0.0;   // 0 = only outgoing audible, 1 = only incoming
    double outLowKill = 0.0; // 0 = user EQ, 1 = low band fully killed
    double inLowKill = 1.0;
};

[[nodiscard]] PhraseKind phraseKindFromLabel(const char* label) noexcept;

// First downbeat after any leading silence (or the end of that silence without a grid).
[[nodiscard]] double introStartSec(const TrackTimeline& track) noexcept;

// earliestOutStartSec is the outgoing playhead (minus a small grace): anything
// before it is in the past. forceSoon picks the next bar instead of the best one.
[[nodiscard]] TransitionPlan planTransition(const TrackTimeline& out,
                                            const TrackTimeline& in,
                                            const AutomixSettings& settings,
                                            double earliestOutStartSec,
                                            bool forceSoon = false);

[[nodiscard]] TransitionLevels transitionLevelsAt(const TransitionPlan& plan, double progress) noexcept;

// Incoming position that is phase-aligned with the outgoing playhead.
[[nodiscard]] double incomingPositionFor(const TransitionPlan& plan, double outPositionSec) noexcept;

} // namespace brockdj::automix
