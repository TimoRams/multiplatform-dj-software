# Performance and Stability Audit

Date: 2026-08-24  
Baseline revision: `95775e7`  
Host used for measurements: Linux x86_64, Intel Core Ultra 5 125H, 18 logical
CPUs, `RelWithDebInfo`

Scrolling-waveform presentation follow-up: 2026-08-25, working baseline
`6f4232f`.

This document records the measured performance/lifetime review performed after
the scratch-quality work. It is a map for future changes, not a claim that
desktop benchmarks replace physical audio/controller testing.

## Outcome

The audit found five material problems and fixed them at their ownership
boundaries:

| Finding | Failure mode | Implemented boundary |
| --- | --- | --- |
| Track-swap guard lived inside `RenderModeRouter` | The callback could be inside `DeckAudioPipeline::consumeCommands()` while control destroyed `playback`, leaving a real use-after-free/data-race window. | One pipeline-entry activity lease now covers command consumption and the complete render graph. Track install/clear drains that lease before retiring sources. |
| Cache release held `readerMutex` across decoder access | Loading/replacing a track could freeze the Qt/control thread behind a slow compressed/USB read. | The cache atomically detaches a shared reader owner. Normal handover is deferred; explicit eject/shutdown waits for active reader leases and confirmed file closure. |
| Paused loaded keylocked decks still ran the stretcher | Signalsmith/Rubber Band repeatedly processed silent router output, consuming FFT CPU needed by live decks and scratch. | `DeckAudioPipeline` publishes normal-input activity. Paused normal transport takes one direct pull; scratch remains available and resume uses the existing seed/crossfade handshake. |
| Mutually exclusive QML surfaces were merely hidden | Startup created settings, mapping editor, C/D controls and both waveform layouts, retaining bindings and signal connections while invisible. | Settings/Source/windows and alternate deck/waveform layouts now have explicit on-demand lifetimes. `Library` stays eager for its startup/controller contract. |
| Scratch motion was only block-continuous and its high-rate filter assumed source rate equaled device rate | Fast turns exposed callback-rate acceleration chirps; release targets became pitch stairs; an 8x backspin could lose its reverse cache margin; 192/48 kHz content could use the wrong anti-alias cutoff. | Exact FLX10 position is paired with a jitter-bounded velocity, a C2 trajectory and continuous safety bounds. Release interpolates across the block. A prebuilt 64-tap absolute-rate filter and source-rate-sized window cover reverse and high-rate tracks. |
| Scrolling detail used a moving one-pixel carrier and unstable presentation scheduling | Alternating filled/empty texture columns produced a travelling moire pattern under fractional translation; a free-running 16 ms timer beat against display VSync, and guard-window advances reassigned surviving tiles to different scene nodes. Peaks appeared alternately thick, thin or briefly absent. | Detail now aggregates every physical display column, uses neighbour gutters for seam-safe linear filtering, maps global tile IDs to stable scene-node slots, and samples the playhead from `FrameAnimation`. Critical audio pressure suspends queued raster work by generation while retaining existing GPU textures. |

The review deliberately did not merge real thread or lifetime owners. Fewer
files are useful only when they also produce simpler ownership; combining the
cache worker, audio graph, transport facade or render boundary would make the
system harder to prove safe.

## Runtime topology reviewed

```text
Qt / controller writers
    -> scalar atomics and bounded generation-tagged commands
    -> DeckAudioPipeline callback lease
       -> RenderModeRouter -> TimeStretchProcessor -> DeckChannelProcessor
       -> AudioEngine -> MasterMixer / HeadphoneBus -> AudioOutputRouter

DeckTrackLoader -> prepared AudioCacheHandle
AudioCacheWorker -> immutable guarded PCM pages
TimeStretch worker -> prepared/seeded inactive pipeline

Analysis workers -> bounded immutable result mailbox -> Qt owner
Qt owner -> immutable waveform snapshots -> Qt scene graph / RHI
```

The callback lease and decoder lease solve different problems. The callback
lease protects C++ source-object lifetime for one audio block. The decoder
lease protects the backing `AudioFormatReader` while the non-realtime cache
worker finishes a read. Neither permits I/O or waiting in the callback.

## Measurements and profile

### Waveform pipeline baseline (2026-09-01)

`BrockDJ_waveform_pipeline_benchmark` measures a production `WaveformAnalyzer`
run while another track is playing through `AudioPageCache`. It reports
analysis and cache-load time, waveform cache sizes, peak RSS, callback latency,
and playback/cache starvation counters. The deterministic fixture duration is
configurable without changing the production pipeline:

```bash
BROCKDJ_WAVEFORM_BENCHMARK_SECONDS=30 \
  ./build/BrockDJ_waveform_pipeline_benchmark
```

The pre-neutral-waveform baseline on the macOS x86_64 development host was:

| Metric | Baseline |
| --- | ---: |
| 30 s track analysis | 1,918.74 ms |
| Waveform cache reload | 20.85 ms |
| Payload cache | 1,638,036 bytes |
| Render cache | 281,728 bytes |
| Peak RSS | 53,157,888 bytes |
| Deck A callback average / worst | 72.00 / 222.37 us |
| Deck A playback misses / starvation | 0 / 0 |

The separate five-second wide-scratch stress baseline recorded 248 starvation
blocks, no dropped requests, 380.28 us average callback time, and 3,815.27 us
worst callback time. These measurements are host-specific and are comparison
anchors, not universal performance thresholds.

### Neutral waveform Stage 2-3 comparison

The neutral waveform refactor retained the high-resolution geometry stream,
stored shared-scale bass/mid/treble data at approximately 150 points/second,
and added a fixed 1,200-point overview. Colour is now derived by the renderer
instead of persisted as the canonical analysis result. The same benchmark and
host produced:

| Metric | Before | After | Change |
| --- | ---: | ---: | ---: |
| 30 s track analysis | 1,918.74 ms | 1,861.32 ms | -3.0% |
| Waveform cache reload | 20.85 ms | 15.83 ms | -24.1% |
| Payload cache | 1,638,036 bytes | 1,092,044 bytes | -33.3% |
| Render cache | 281,728 bytes | 354,918 bytes | +26.0% |
| Combined waveform cache | 1,919,764 bytes | 1,446,962 bytes | -24.6% |
| Peak RSS | 53,157,888 bytes | 53,063,680 bytes | -0.2% |
| Deck A callback average / worst | 72.00 / 222.37 us | 66.30 / 234.61 us | host variance |
| Deck A playback misses / starvation | 0 / 0 | 0 / 0 | unchanged |
| Stored spectral points | 18,000 | 4,500 | -75.0% |
| Cached overview points | 0 | 1,200 | now available |

The render cache grew because each line now carries neutral minimum, maximum,
RMS, and three-band data in 10 bytes rather than an 8-byte styled RGB record.
The lower spectral resolution more than offsets that increase, reducing the
combined cache by approximately 473 KB for the fixture.

Focused cache measurements remained stable: page-cache hit/miss/duplicate
request operations measured 50.74/24.47/35.14 ns, cached playback measured
37.91 us per 512-sample block, and the scratch reader measured 920.68 us per
512-sample block. The five-second wide-scratch stress measured 256 starvation
blocks, zero dropped requests, 367.35 us average callback time, and 16,134.1 us
worst callback time. The isolated worst callback is scheduler-sensitive; the
average, starvation count, and zero-drop result are comparable to baseline.
No `AudioPageCache` scheduling or callback path changed in Stage 2-3.

The focused `deck_audio_graph` benchmark was run repeatedly because laptop
frequency scaling and hybrid-core placement cause visible variance. Observed
ranges on the audit host were:

| Path | Block | Observed average |
| --- | ---: | ---: |
| Neutral deck pipeline | 512 | 14–23 us |
| Keylocked live deck | 512 | 19–33 us |
| Active scratch deck graph | 512 | 85–110 us after the 64-tap quality pass |
| EQ/filter stress | 8192 | 251–339 us |
| Four direct deck graphs | 4 x 512 | 0.86–1.21 ms |
| Full four-deck `AudioEngine`, mixed callback sizes | 64–16384 | 2.4–3.1 ms overall average; zero measured per-block deadline misses |

The values are regression orientation points, not hard CI thresholds. A 512
sample callback at 48 kHz has about 10.67 ms; a 64 sample callback has about
1.33 ms, so validation compares every block with its own budget.

`perf` sampling identified the expected live DSP costs rather than hidden I/O
or locking:

| Sampled symbol | Approximate cycle share |
| --- | ---: |
| Signalsmith spectrum processing | 14–19% |
| Band-limited scratch stereo read | 11–18% |
| Per-channel biquad processing | 9–12% |
| Hermite resampling | 5–6% |
| Trigonometric/FFT helpers | distributed remainder |

This is why paused-keylock bypass is more valuable than rearranging small
facade methods, and why scratch sinc quality should not be reduced without an
audible/hardware reason.

The isolated 512-sample stereo scratch-reader benchmark moved from about 66 us
with the former 32-tap kernel to 115–128 us with 64 taps and the complete
cutoff range. Even the upper observed increase is about 0.6% of a 48 kHz/
512-sample deadline per actively scratching deck. It bought materially stronger
stopband rejection and passband retention; the detailed motion, spectrum, FLX10
and 192 kHz evidence is maintained in
[`scratch-engine-quality.md`](scratch-engine-quality.md).

The ordinary handover microbenchmark completes in single-digit microseconds,
but that benchmark cannot reproduce a decoder blocked inside an OS or codec
call. `audio_page_cache_tests` therefore uses a deterministic blocking reader:

- deferred release must return while the read is still blocked;
- the reader must remain alive until that read ends;
- synchronous eject must not return until the read ends and the reader is
  destroyed.

`deck_audio_graph_tests` concurrently renders while performing 200 alternating
install/clear generations. Output must remain finite and the callback must keep
making progress. Both `deck_audio_graph` and `audio_page_cache` passed GCC
ThreadSanitizer with `halt_on_error=1` on the audit host; sanitizer timings are
intentionally not used as performance measurements.

## Lifetime invariants added

### Track replacement

1. Control publishes `trackSwapInProgress` with sequential consistency.
2. A callback that entered first owns the old graph until its complete block
   returns; control waits outside the realtime thread.
3. A callback that enters after the gate emits bounded silence and returns.
4. Only after active callbacks reach zero may control detach JUCE transport,
   reset cached playback and install the new generation.
5. A scope guard always reopens the gate, including exceptional exits from
   track installation.

The gate must precede mutation of plain callback-visible generation/source
state. A guard placed only in `RenderModeRouter` is insufficient.

### Decoder retirement

1. The cache invalidates `active` and generation under its control lock.
2. It atomically exchanges the cache-owned reader with null.
3. A worker that already acquired a shared lease may finish safely.
4. Deferred callers publish the detached owner to a lock-free intrusive retire
   stack and return without waiting. The decoder worker performs uncontended
   reader destruction; an in-flight read drops its local lease first.
5. Eject/shutdown waits for the active-call count to reach zero; the worker
   drops its shared pointer before decrementing that count, so zero also means
   the file handle is closed.

### Paused keylock

Keylock rendering additionally requires active normal transport input. Pausing
does not tear down or rebuild a stretch pipeline. The direct pull still invokes
`RenderModeRouter`, allowing paused scratch and downstream effect-tail behavior.
Resuming marks keylock re-entry and uses the existing bounded worker seed.

## QML lifetime policy

Visibility is not a performance boundary in QML: hidden items retain objects,
bindings and `Connections`. The audited workspace now uses these rules:

- two-deck and four-deck waveform object trees are mutually exclusive;
- the C/D `DeckControl` row exists only after four-deck mode is selected;
- the production deck rows no longer construct invisible `MixerSection` trees;
  `MixerControl` owns their parameter side effects and the actual mixer remains
  available in the development controls window;
- AIO Settings and Source instantiate asynchronously only while active;
- the standalone desktop Settings window is created on first use and retained
  for subsequent opens;
- each mapping editor is created only on first editor use and destroyed with
  its owning settings surface;
- `Library` remains eager because startup hydration addresses
  `performanceWorkspace.librarySection`, and hidden FLX10 browse input still
  owns waveform-zoom behavior there.

First activation of a synchronous waveform/deck mode may still cost object
construction. It is intentionally synchronous to avoid partially visible deck
controls; mode-switch latency belongs in the manual regression matrix.

## Scrolling waveform presentation invariants

The enlarged waveform uses a physical-pixel grid, not a logical-pixel stroke
pattern. These are now review invariants:

1. One core texture column aggregates exactly one physical output column at the
   selected raster scale. There is no alternating empty-column mask.
2. Each 1024-column tile carries one hidden real neighbour on both sides.
   `QSGSimpleTextureNode::sourceRect` exposes only the core, so linear sampling
   crosses a tile boundary with the same two audio columns instead of clamping
   to unrelated edge values.
3. A global tile index owns `tileIndex mod poolSize`, including negative
   pre-roll indices. Advancing the guarded window by one tile changes only the
   departing and entering slots; every overlapping texture node stays resident.
4. The timeline transform remains fractional and is centred on an actual
   physical-pixel centre for the window DPR. Waveform texture, cue lines, loop
   edges, hit-testing and the QML playhead use the same centre convention.
   Beat/downbeat ticks are the one exception: they ride a second transform
   node (`markerTimeline`) snapped to the nearest whole physical pixel every
   frame (`physicalPixelSnap`), paired with geometry pre-centred on a physical
   pixel at rebuild time (`markerLineX`). A rigid tick line has no envelope to
   hide sub-pixel phase in the way waveform peaks do, so leaving it on the
   continuous transform read as a visible alternation between a crisp pixel
   and a soft two-pixel smear as playback advanced — see the residual note
   below for why the waveform texture itself keeps the continuous transform.
5. Normal motion is requested by Qt Quick's presentation clock. Reduced tiers
   may discard frames; they never run a catch-up loop.
6. The audio callback publishes only its existing playhead/diagnostic atomics.
   Qt samples those values, low-priority raster workers consume immutable
   waveform snapshots, and the render thread uploads completed images. No
   render mutex, QObject call, texture upload or worker wait is callback-reachable.
7. At rest, tiles are cut at the exact display scale: one texel covers one
   physical pixel and the destination rectangle is a whole number of physical
   pixels wide. The geometric raster ladder is a *gesture* fallback only.

Invariant 7 replaced an unconditional ladder cut. The ladder exists so a tempo
sweep repositions cached textures instead of invalidating every tile key at
frame rate, but applying it at rest left the texture permanently stretched by
up to ~2%. At that ratio roughly every 45th physical column is resampled from
two texels while its neighbours are not, which does not move with the audio —
it stands still in the viewport as a thick/thin ripple. Cutting exactly once
the scale has held still removes the resampling entirely.

`selectRasterScale` carries the decision in render-thread-only state:

- Below `kRasterScaleStickyTolerance` (0.1%, under one texel across a whole
  1024-column tile) the current cut is kept. This absorbs the continuous
  sub-visible tempo drift a running sync coordinator produces; re-cutting for an
  invisible error would blank the view repeatedly.
- While the scale genuinely moves frame to frame, tiles land on the ladder and
  survive the gesture.
- After `kRasterScaleSettleFrames` still frames, tiles are re-cut exactly.
  Re-cutting costs one visible transition, which is acceptable at the end of a
  fader move and unacceptable as a permanent ripple.
- A paused deck stops requesting frames the moment the fader is released, which
  would strand the view on the ladder cut. `rasterScaleSettlePending` makes the
  renderer request the remaining frames itself; it is self-terminating and
  bounded by `kRasterScaleSettleFrames`.

`rasterScaleStretch` reports the applied ratio and must read `1.0` during
steady playback; `rasterScalePixelExact` is its boolean form. A value that stays
away from `1.0` means tiles are stuck on the ladder cut.

Beginning with the `reduced` tier, pending raster requests and new texture
uploads are dropped. An already-running bounded worker loop may finish on its
low-priority thread, but its work generation is stale and cannot enter the
cache or wake the GUI. Existing scene textures may continue moving at a reduced
disposable frame rate.
Relevant runtime diagnostics are `rasterWorkEnabled`, `pendingTileRequests`,
`tileUpdateQueued`, `tileTexturePhysicalWidth`,
`tileFilterGutterPhysicalPixels`, `rasterScaleStretch`,
`rasterScalePixelExact`, `textureUploads`, and `worstPaintNodeUsec`.

Known residual, deliberately not "fixed": a pixel-exact texture translated by a
fractional phase still distributes an isolated one-pixel peak across two pixels
at phase 0.5. Unlike the ladder ripple this is uniform across the viewport and
moves with the audio rather than standing still in it, so it reads as motion,
not as a defect. Removing it entirely would require band-limiting the envelope
horizontally, which trades transient sharpness — a visual design decision, not a
correctness fix, and it should not be made without side-by-side hardware review.
This residual is why beat/downbeat ticks are *not* also given band-limited
envelopes to fix their own version of the same phase artifact: pixel-snapping
the tick line instead is cheap, has no transient-sharpness trade-off, and a
rigid line's own position carrying up to one physical pixel of snap error is
imperceptible, unlike a waveform peak's shape.

## Boundaries intentionally retained

- `AudioEngine`, `DeckAudioPipeline`, buses, output router, page cache and
  time-stretcher remain separate lifetime/test owners.
- `AudioCacheWorker`, `DatabaseWorker`, `MediaIoScheduler`, analysis workers and
  hardware transports remain joined independent threads.
- `DjEngine` stays a broad public QML/controller facade implemented in
  responsibility-sized translation units.
- `Library.qml` is not mechanically split. Extracting pieces without relocating
  state and focus ownership would change files, not reduce work.
- `SettingsPanel.qml` and `SettingsWindow.qml` remain near-duplicates. Shared
  content extraction is desirable only after desktop/AIO focus, popup,
  reconciliation and Apply behavior have parity tests.

## Remaining prioritized work

1. Verify scrolling waveform presentation at 59.94/60/90/120/144 Hz, DPR
   1.0/1.25/1.5/2.0/3.0, forward/reverse playback, scratch, tempo sweeps,
   guard crossings and window moves between mixed-DPR displays. Record the
   graphics API and confirm there is no thick/thin cadence or tile seam.
2. Verify two-/four-deck scratch, keylock, load and eject on the FLX10 and real
   ALSA/JACK/CoreAudio/ASIO devices. Scheduler and device jitter are not
   reproducible in a container. Use the capture/listening matrix in
   `scratch-engine-quality.md`; automated continuity is not a subjective parity
   claim.
3. Measure the Qt-owned `LibraryDatabase` compatibility queries during real
   library hydration. Move only confirmed stalls to immutable
   `DatabaseWorker` commands; never move a live QSQLITE connection.
4. Measure cancellation/join latency at every analysis phase and final artifact
   serialization on slow disks.
5. Add runtime visual parity tests before consolidating settings content or
   decomposing the library/header/deck monoliths.
6. Monitor cache starvation, worker latency, callback overruns and
   `ControlClock` late/worst ticks with production compressed media.

## Validation commands

Final audit results on the host above:

- 47/47 CTest cases passed;
- application target and all test targets built successfully;
- the four changed QML surfaces passed `qmllint`;
- ThreadSanitizer passed `scratch_motion`, `scratch_cache`, `flx10_jog_routing`,
  `audio_page_cache` and the concurrent `deck_audio_graph` test;
- AddressSanitizer and UndefinedBehaviorSanitizer passed `scratch_motion`,
  `scratch_cache` and `flx10_jog_routing`; LeakSanitizer itself cannot run in
  the ptrace-managed sandbox and was disabled for that focused run;
- repeated two-second constrained-cache stress runs completed 657–662 callbacks
  with zero dropped requests and zero callback overruns;
- repeated five-second four-deck 64/128-sample soaks completed 41,443–65,566
  blocks with zero deadline misses (75–119 us average and 1.13–2.18 ms worst;
  scheduler/core placement explains the throughput spread);
- keylock scratch-release transition work stayed below 120 us at 128 samples
  and 470 us at 512 samples, with zero pipeline rebuilds.

```bash
cmake --build build-tests -j2
ctest --test-dir build-tests --output-on-failure -j2
cmake --build build -j2 --target BrockDJ
qmllint -I build -I src/qml \
  src/qml/performance/PerformanceWorkspace.qml \
  src/qml/shell/TopHeader.qml \
  src/qml/settings/SettingsPanel.qml \
  src/qml/settings/SettingsWindow.qml
BROCKDJ_CACHE_STRESS_SECONDS=2 \
  BROCKDJ_CACHE_STRESS_TRACK_SECONDS=60 \
  ./build-tests/BrockDJ_audio_cache_stress
BROCKDJ_AUDIO_SOAK_SECONDS=5 BROCKDJ_AUDIO_SOAK_STRICT=1 \
  ./build-tests/BrockDJ_deck_audio_graph_tests
```

The manual hardware/UI matrix remains in
`docs/testing/regression-checklist.md`. Update this audit only when the topology,
invariants, measured bottlenecks or accepted boundaries change.
