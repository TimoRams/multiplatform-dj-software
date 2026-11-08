# Song parts

`analysis/SongParts.h` labels the phrase segments from `SongStructure.h` as
Intro, Verse, Build, Drop, Breakdown, Outro or Silence using relative energy,
position and the energy trend between neighbouring phrases. The labels are
produced by `PhraseAnalyzer`, stored with the track and shown as a clickable
strip below the overview waveform (click = jump to the part).

Legacy segments in the database without a part label are upgraded on load, so
re-analysis is not required. Automix uses the same labels to pick mix-in
(Intro) and mix-out (Outro/Breakdown) points.

Limitations: the classifier is energy-based. Heavily compressed tracks may be
labelled mostly Intro + Drop.

Debug builds compile `TimeStretchProcessor.cpp` with `-O2`, otherwise the
unoptimised stretcher misses the realtime deadline and keylock falls back.
