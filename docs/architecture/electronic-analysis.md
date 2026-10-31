# Electronic track analysis

The default analysis profile is **Electronic**, with a configurable 48–255 BPM
search range. Settings → Analyse also offers **Legacy** (the existing rhythm
pipeline and libKeyFinder, initially 60–200 BPM). Each profile retains its own
range. Queued jobs capture these options; later settings changes do not alter
running or queued work.

## Signal processing

`ElectronicAnalysis.cpp` implements a native C++ offline pipeline. A JUCE reader
streams the whole track at a canonical 44.1 kHz. Channel magnitudes and attack
energies are combined after processing each channel, avoiding cancellation of
antiphase stereo material. The implementation retains feature arrays rather
than the whole decoded PCM track.

Rhythm features use 1024-point FFTs with a 256-sample hop. Autocorrelation and
windowed Fourier support produce tempo candidates within the selected range.
A separate 900–9000 Hz attack envelope, sampled every 44 samples, refines phase
and period by robust regression. The refined tempo is not rounded to integer
BPM. Regression is also checked against the selected BPM range.

Local models use 16-second windows at 8-second intervals. A new stable tempo or
phase needs three consistent windows and a supported run of actual attacks at
its boundary. Gradual transitions follow measured beat intervals; a breakdown
without a phase change preserves the constant grid. Twelve logarithmic energy
bands provide arrangement novelty at 1, 2, 4 and 8 bars. Novelty selects the bar
phase and can correct a louder offbeat interpretation when bass support agrees.
No structural evidence means the bar phase remains an assumption.

Key detection uses the entire track, 4096-point FFTs, interpolated spectral
peaks in approximately 25–3500 Hz, local spectral whitening, harmonic folding,
36-bin chroma with tuning correction, and EDMA major/minor profiles. Close
parallel major/minor matches prefer minor. Results use Camelot notation;
insufficient tonal evidence returns unknown. Existing keys survive an unknown
replacement result. Legacy retains its existing first-90-second KeyFinder path.

## Ownership and persistence

All decoding, FFTs, feature allocation and fitting run on the existing analysis
worker, under its background-work gate and cooperative cancellation/pause
checks. No processing is added to the audio callback or `RenderModeRouter`.

An analysis request carries a typed options snapshot and fingerprint such as
`electronic-v1:48:255`. Beat and key results retain separate fingerprints
and algorithm versions (Electronic: beat 4/key 2; Legacy: beat 3/key 1). Schema
18 adds these provenance columns without removing old results. The aggregate
compatibility version is deliberately unchanged: choosing a new profile does
not automatically invalidate a library.

Automatic waveform reconstruction preserves existing BPM/grid/key results.
Explicit library reanalysis may replace automatic results, but both owner-thread
publication and the database writer protect user-modified and locked grids.
The SQL guard is evaluated when the write executes, covering manual edits made
while analysis was running. Cues and loops are not rewritten.

Waveform caches are loaded on the worker and reused for explicit reanalysis.
Completed library analysis is merged into loaded decks while retaining their
current waveform. File identity and track generation guard publication.

## Validation and limits

`electronic_analysis` covers constant 48–255 BPM, fractional tempo over ten
minutes, tempo cuts, ramps, breakdowns with and without a phase change,
structural downbeats, loud offbeats, selected BPM ranges, 44.1/48/96 kHz input,
antiphase stereo, 24 synthetic major/minor chords, detuning, right-only audio,
tonal content after 90 seconds, silence, noise and cancellation.

Integration tests cover option snapshots and queue deduplication, persisted
profile/range settings, schema migration, grid protection during database
writes and owner-thread publication, and automatic reconstruction versus
explicit reanalysis. Existing sync, cue/loop, transport and audio graph tests
provide regression coverage.

These are synthetic fixtures, not an accuracy score on commercial releases.
Annotated reference tracks across House, Techno, Trance, DnB and Hardstyle,
subjective checks of downbeats/phrases, and analysis while real decks play on
target hardware remain necessary. Very short tempo changes and ambiguous
meters are not guaranteed by the stable-window detector. Key modulation is
represented by one whole-track key, not a time-varying harmonic map.
Transition walking currently searches at most 48 seconds before the next
supported stable region; longer continuous ramps require additional validation.

## Validation snapshot (2026-10-29, macOS Debug)

The application and analysis test executables build successfully. After the
window-bounded fitting change, the focused analysis/persistence suite passed
6/6 tests. The expanded electronic test, including exactly 48 and 255 BPM,
passed separately in 32.72 seconds.
The final anchor/provenance checks (`ci_runtime_smoke`, `analysis_lifetime`,
`analysis_snapshot`, `library_analysis_manager`) also passed 4/4 in 15.64 seconds,
including preservation of a manual grid's sample rate when a replacement
result carries a different rate.

Selected actual fixture output (times are seconds):

```text
Before offbeat correction:
structure offbeat=1 bpm=128 first=0.407392 bar-phase=4
FAIL: structure puts the grid on the kick even with louder offbeat attacks
FAIL: arrangement changes identify the musical downbeat

After correction:
structure offbeat=1 bpm=128 first=0.173017 bar-phase=3
timeline p95=0.000203766 nodes=2
timeline p95=0.000465986 nodes=61
tempo 48 -> 48 phase=5.50276e-05 drift=1.82069e-05 nodes=1
tempo 255 -> 255 phase=2.65234e-06 drift=9.36233e-07 nodes=1
100% tests passed out of 1
```

The complete regression selection passed all 12 analysis/integration tests
(including `deck_audio_graph`, `deck_transport`, `sync_coordinator`,
`cue_loop_controller` and QML). The additional, unchanged `time_stretch` test
was unstable: an isolated alignment-enabled run passed in 10.09 seconds, but
both a parallel run and the complete serial run failed with
`scratch release restores keylock after a bounded bridge`. Therefore the broad
selection is **12/13, not fully green**. That scratch-release failure remains
open; no audio processing code or audio test was modified in this task.

The production pipeline benchmark used a 120-second generated track, 48 kHz,
512-sample blocks, an explicit seek, and looped cached playback while a separate
track was analysed. Playback decks share one cache handle, so this does not
model four independent files, a complete mixer/FX graph, or an audio device.
Both benchmark runs exited successfully.

| Playing sources | Analysis | Callback mean / worst | Cache misses / starvation | Peak RSS |
| --- | --- | --- | --- | --- |
| 2 | 53.964 s | 0.122 / 3.576 ms | 0 / 0 | 92.9 MB |
| 4 | 55.043 s | 0.226 / 1.239 ms | 0 / 0 | 93.9 MB |

The callback block budget was 10.667 ms. Both runs had zero dropped cache
requests and passed the existing guards against audio-thread disk/decoder
access. An earlier two-source run took 61.765 seconds before restricting local
fits to their actual hit windows; these are individual Debug measurements,
not a statistical performance guarantee or a Release benchmark.

Reproduce the focused analysis tests and benchmark:

In CLion, use the shared **Electronic Analysis (CMake)** run configuration
(Debug profile). This builds `BrockDJ_electronic_analysis_tests` with its source
include directory, C++23, Qt and JUCE dependencies. The temporary **C/C++ File**
configuration compiles only `electronic_analysis_tests.cpp`; it cannot build
this multi-source test executable. From a terminal, build and run it with:

```sh
cmake --build cmake-build-debug --target BrockDJ_electronic_analysis_tests
ctest --test-dir cmake-build-debug -R '^electronic_analysis$' --output-on-failure
```

For the broader analysis selection and playback benchmark:

```sh
ctest --test-dir cmake-build-debug --output-on-failure -R \
  '^(ci_runtime_smoke|analysis_lifetime|analysis_snapshot|library_analysis_manager|electronic_analysis|beat_analysis)$'
BROCKDJ_WAVEFORM_BENCHMARK_SECONDS=120 \
BROCKDJ_WAVEFORM_BENCHMARK_PLAYING_DECKS=4 \
  ./cmake-build-debug/BrockDJ_waveform_pipeline_benchmark
```

## References and provenance

The architectural inspiration is [chrisle/rbxport](https://github.com/chrisle/rbxport),
particularly its [analysis overview](https://github.com/chrisle/rbxport/blob/main/crates/rbl-analysis/docs/overview.md),
[key notes](https://github.com/chrisle/rbxport/blob/main/crates/rbl-analysis/docs/algorithms/key.md),
[beat notes](https://github.com/chrisle/rbxport/blob/main/crates/rbl-analysis/docs/algorithms/beat.md)
and [downbeat notes](https://github.com/chrisle/rbxport/blob/main/crates/rbl-analysis/docs/algorithms/downbeat.md).
The EDMA profile values and parallel-minor heuristic were consulted in its
[key implementation](https://github.com/chrisle/rbxport/blob/main/crates/rbl-analysis/src/key.rs).
rbxport is GPL-2.0-or-later; this repository is AGPL-3.0. This implementation
does not embed its Rust crate, invoke a sidecar, or add an Essentia dependency.
Its fitter and structural detector are independently implemented approximations;
their behavior and accuracy are not claimed to match rbxport.
