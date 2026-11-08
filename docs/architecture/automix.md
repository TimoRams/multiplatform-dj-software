# Automix

Automix plays the library **Queue** on decks A and B and mixes between them
without user interaction. The transition is beat-synced and starts on a
musically sensible boundary (outro, phrase, or bar).

## Components

| Part | File | Role |
| --- | --- | --- |
| Planner | `src/deck/automix/AutomixPlanner.{h,cpp}` | Pure, Qt-free maths: where to start a transition, how long it is, crossfade/EQ curves |
| Controller | `src/app/AutomixController.{h,cpp}` | QObject state machine driving decks, mixer and queue (context property `automix`) |
| UI | `src/qml/library/Library.qml` (`AutomixBar`) | Strip in the Queue tab: toggle, status, bars selector, "Mix jetzt", "Überspringen" |
| Tests | `tests/deck/automix_planner_tests.cpp` | `ctest -R automix_planner` |

## Transition planning

`planTransition(out, in, settings, earliest, forceSoon)` takes a
`TrackTimeline` for each track. A timeline holds the duration, the BPM, the
downbeats from the beat grid, and phrase spans from `PhraseAnalyzer`.

For every length from `transitionBars` down to `minimumBars` (halving each
step), the planner tries these anchors in order:

1. **Outro**: the trailing run of low-energy/silent phrases in the second half
   of the track, snapped to the nearest downbeat.
2. **Phrase**: the latest downbeat whose bar index is a multiple of
   `phraseBars` (8).
3. **Bar**: the latest downbeat at which the transition still fits.

A transition must end before the audible end, which is the start of trailing
silence (or the track end) minus `endMarginSec`. If nothing fits, or the user
pressed "Mix jetzt", the transition starts on the next bar and is shortened
to the time that remains (**Immediate**). A track without a beat grid gets a
plain time-based crossfade of `fallbackSeconds` (**Time**).

The incoming track is cued on its first downbeat after any leading silence
(`introStartSec`). Its position follows
`inStart + (outPos - outStart) * inPerOut`, where `inPerOut = outBpm / inBpm`.
The ratio is normalised to `[0.75, 1.5)`, so half- and double-time tracks
mix at their natural phase.

## Mix curves

`transitionLevelsAt(plan, progress)` returns these values:

- `incoming`: crossfader position. It follows a smoothstep in two halves, so
  the faders meet (0.5) exactly at the bass swap.
- `inLowKill` / `outLowKill`: the low EQ is killed on the incoming deck until
  the middle bar. There it swaps over one beat, so the two kick drums never
  play together.

The controller interpolates between the DJ's own low-EQ setting and the kill
value. When the transition ends or is aborted, it restores the DJ's setting.

## Controller state machine

```
off ──enable──▶ starting (load from queue, play from intro)
                   │
                   ▼
               playing ──preload next onto other deck──▶ (next ready)
                   │            position ≥ plan.outStart
                   ▼
                mixing ──progress ≥ 1──▶ playing (decks swapped)
waiting: queue empty, resumes on queueChanged
```

- The outgoing deck becomes sync leader during the mix and hands over at the
  end.
- The crossfader assignment is set to A/B when automix takes over.
- If the DJ stops the incoming deck during a mix, automix restores the EQ and
  switches itself off.
- If the DJ pauses the playing track, automix waits.
- If the DJ loads a track onto the waiting deck, automix uses it as the next
  track.
- Missing files in the queue are skipped and dequeued.
- If a track runs out before a transition, automix hard-cuts to the prepared
  track.
- `transitionBars` (4–64) is persisted in the UI state as
  `automixTransitionBars`.
