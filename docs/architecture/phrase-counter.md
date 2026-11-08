# Beat & Phrase Counter

Each deck's track info panel shows, next to the TIME/REMAIN label:

- eight small blocks for the bars of the current 8-bar phrase (past bars dimmed,
  the current bar lit in the deck accent colour, pulsing off the downbeat);
- either the next analysed section with a bar countdown (e.g. `DROP 4`, orange
  when two bars or fewer remain) or, without segments, the phrase number (`PH 12`).

While track analysis is running, its percentage and progress bar temporarily
replace the phrase counter, including when a provisional BPM tag is available.
The counter returns when analysis ends. A thin orange bar above this area shows
the current deck's stem-separation progress; a pulsing dash indicates that the
job is queued. Hovering over the bar shows its status and percentage.

## Data flow

`DjEngine::beatDisplayInfo()` (`src/deck/DjEngineTransport.cpp`) computes the beat
position from the elastic beat grid (or anchor + BPM), and uses the pure helpers in
`src/deck/PhraseCounter.h`:

- `fromBar()` maps a bar number to phrase number / bar-in-phrase (handles pre-roll bars);
- `locateSegment()` finds current and next track segment (order independent);
- `barsUntil()` rounds the beat distance to the next segment up to whole bars.

Returned fields: `valid`, `beatInBar`, `barNumber`, `barInPhrase`, `phraseNumber`,
`barsPerPhrase`, `segmentLabel`, `nextSegmentLabel`, `barsToNextSegment` (-1 = none).

Phrases are counted from the grid anchor in fixed 4/4, 8-bar units; tracks whose
first downbeat is not a phrase start will show offset phrases.

Tests: `tests/deck/phrase_counter_tests.cpp` (ctest `phrase_counter`).
