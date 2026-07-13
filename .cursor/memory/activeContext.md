# Active Context

*Last updated: 2026-07-13*

## Current task
**Per-deck audio pipeline extracted into DeckAudioGraph**
- `DeckAudioGraph` uniquely owns cached playback, JUCE transport, scratch bridge/resampler,
  time stretch, mixer/FX, audio cache handle, audio generation and complete source lifecycle.
- `DjEngine` remains the QML/product/transport/sync facade but stores only one graph pointer for
  the pipeline. Concrete sources are reached through transitional graph forwarding; no aliases,
  duplicate owners, Qt dependencies, or `DjEngine` backreference exist in the graph.
- Dedicated tests cover empty/mono/stereo graphs, 44.1/48/96 kHz, 64–8192 blocks, play/pause/seek,
  reverse/loop, scratch handoff, keylock/tempo, EQ/filter/gain, stale and rapid handovers, four
  simultaneous graphs, shutdown invalidation, finite output, retained zero RT counters and timings.
- Release timing found no graph-forwarding regression versus the former direct mixer endpoint, but
  a stressed synchronous cached-source retirement produced a ~1.05 s control-thread handover;
  this is recorded for focused cache-retirement profiling rather than hidden by the refactor.
- Validation: Linux release build and 12/12 CTests pass. Isolated ASAN+UBSAN and TSAN graph stress
  builds pass without reported memory, UB, or race errors. LeakSanitizer itself cannot run in the
  ptrace-managed environment; ASAN was rerun successfully with leak detection disabled.
- Release measurements (single run, microseconds): old-equivalent direct mixer 512 avg 36.45/worst
  381.74; neutral graph 512 avg 34.52/worst 48.34; keylock 512 avg 449.12/worst 788.49; scratch
  512 avg 44.95/worst 190.12; EQ/filter 8192 avg 606.14/worst 900.59; four graphs/512 avg
  2116.28/worst 8891.46; stressed handover 1,053,360.

## Previous task
**Mixer EQ/color-filter coefficient lifecycle fixed**
- `MixerDspSource` no longer calls JUCE coefficient factories in `getNextAudioBlock`. Trivial RBJ low-shelf/peak/high-shelf/LP/HP snapshots are built from clamped control targets outside the callback.
- Two fixed snapshot slots publish complete parameter/device generations. The callback atomically adopts only the newest matching snapshot and crossfades two preallocated stateful filter banks over 128 samples.
- Trim/fader remain atomic targets with existing per-sample smoothers. Dedicated numerical, block-size, rapid-control and concurrent tests keep all five mixer realtime counters at zero.

## Previous task
**RubberBand/TimeStretch realtime lifecycle fixed**
- `TimeStretchAudioSource` owns two fully allocated pipeline slots. RubberBand construction, setters, prewarm, FIFO/buffer setup and latency discovery run in a joined preparation worker or `prepareToPlay`, never in `getNextAudioBlock`.
- Control changes publish a latest-only configuration generation. The callback atomically activates only a ready slot with matching track/configuration generation and performs a preallocated 256-sample output-tail crossfade.
- Scratch bypass, keylock and tempo changes no longer reset/mutate RubberBand in the callback. Ten CTest targets pass and all five realtime-violation counters remain zero.

## Previous task
**Normal playback migrated to AudioPageCache**
- `CachedPlaybackAudioSource` replaced the deck `AudioFormatReaderSource -> BufferingAudioSource -> ReverseStreamAudioSource` chain and its private read-ahead thread.
- Playback and scratch share the global immutable PCM pages and generation-scoped handle. Misses never fall back to a reader/decoder and fade to silence while the timeline advances.
- `AudioTransportSource` remains transport/resampling owner; RubberBand is intentionally unchanged.
- The dedicated cached-playback test and all 9 CTest targets pass. Automated counters prove zero audio-thread disk reads and decoder calls. Hardware/listening verification remains open.

## Earlier task
**DjEngine ownership map + AudioDeviceService extraction**
- Added the exhaustive method/state mapping in `djEngineOwnershipMap.md` and thread rules in `threadOwnership.md`.
- `ApplicationRuntime` uniquely owns one `AudioDeviceService`, which owns the application `AudioDeviceManager`, global routing, error/fallback state and sample-rate/buffer snapshot.
- Four `DjEngine` instances receive a constructor-injected reference. Public QML device APIs remain forwarding facades.
- `DjMasterBus` remains the only audio callback and registers through the service. Shutdown unregisters/closes audio before decks, master bus and service are destroyed in dependency order.
- Service signals refresh deck latency/tempo outside the audio callback and re-emit existing QML NOTIFY signals.
- Hardware-independent service tests and full Linux build/ctest pass; headless startup constructed four decks and shut down cleanly.
- `DjEngine.h` is 729 lines; `DjEngine_Settings.cpp` shrank 445→25 lines and `DjEngine_SettingsQuery.cpp` 207→27. `TransportLatency` remains mixed global/per-deck by design.

## Previous task
**Async-signal-safe POSIX SIGINT/SIGTERM handoff**
- Added a small Unix-only `PosixSignalHandler`: `sigaction` writes one byte to a nonblocking/CLOEXEC self-pipe; a `QSocketNotifier` drains it in the Qt eventloop and requests quit exactly once.
- The actual handler performs no Qt calls, allocation, logging, locking, object access or teardown. It only preserves `errno`, reads a `sig_atomic_t` fd and calls `write()`.
- RAII teardown restores previous handlers while signals are blocked, disables the notifier, clears the global fd and closes both pipe ends. Initialization failure is logged in normal startup context and does not abort audio startup.
- Linux tests send real SIGINT/SIGTERM, including a burst, and verify one notification, drained pipe, descriptor flags/closure and safe reinitialization. Full headless app Ctrl+C and SIGTERM runs exited through the normal shutdown path.
- macOS uses the same `Q_OS_UNIX` implementation but was not locally run; Windows does not instantiate the component and its existing path is unchanged.

## Previous task
**Analyzer lifetime, cancellation and stale-completion fix**
- `WaveformAnalyzer` now has explicit job states, monotonically increasing analyzer generations, non-blocking `requestCancel()`, and deterministic `shutdownAndJoin()`; the former 150 ms destruction timeout is gone.
- Deck track replacement/eject and application shutdown keep using the compatibility `stopAnalysis()` entry point, which now performs the safe join before persistent `TrackData` is cleared/reused or destroyed.
- `LibraryAnalysisManager` uses a deque and validates completion with `QPointer`, a manager-global job generation, analyzer generation and file path. Same-track reanalysis cannot accept an old queued callback.
- `TrackData` progress coalescing is atomic; RGB coalescing/timer state is confined to its QObject owner thread through queued scheduling.
- Added `analysis_lifetime` tests for invalid/error termination, unique/repeated generations, cancellation just after start, destructor join and rejection of cancelled completion. RelWithDebInfo, ASan+UBSan and TSan passes are green.
- Remaining scoped limitation: analysis still progressively writes mutex-protected fields rather than one final immutable snapshot; the key-finder library call itself is not interruptible, so a join can wait for that bounded phase.

## Previous task
**FX effect-switch realtime/threading fix**
- `FxProcessor::setEffectType()` no longer mutates DSP state. It validates the enum and publishes a packed atomic `{generation,type}` command, including repeated same-type requests.
- `MixerDspSource::getNextAudioBlock()` consumes all FX commands once before pre/post-EQ routing. `FxProcessor::process()` has the same idempotent boundary apply as a direct-use fallback; the active type stays fixed for the block.
- Pitch resources and all vectors/buffers are prepared only in `prepare()`. Pitch and roll switches now reset only small preallocated metadata on the audio thread (`noexcept`), without clearing/allocating large buffers.
- Removed the defensive SC-crush `vector::assign` from processing; an invalid unprepared state now asserts and fails soft.
- Added deterministic all-type/block-size tests and a two-thread FX/parameter stress test with finite-sample checks.

## Previous task
**Stability/realtime preparation pass**
- Created `.cursor/memory/riskRegister.md`, `.cursor/memory/regressionChecklist.md`, and `.cursor/memory/realtimeRules.md`.
- `DjEngine::onTimer()` sync maintenance now uses explicit braces through `engine::shouldRunFollowerSyncMaintenance()`.
  Decision: both `updatePhaseCorrection()` and `updateTightDoubleAlignment()` belong under the same follower-sync condition (`syncEnabled && !syncMaster && !scrubbing && !releaseGlide`). Tight-double alignment also has internal guards, but running it when sync is disabled is unnecessary and contradicted the surrounding sync logic.
- Added a focused smoke-test policy check so `updateTightDoubleAlignment()` cannot become an unconditional timer action again without the shared predicate test failing.
- Removed the duplicate `src/domain/TrackData.cpp` CMake source entry.
- CI/package `APP_VERSION` now derives from the configured CMake `PROJECT_VERSION`; CMake remains the single source of truth.
- Release builds are portable by default. Native `-march=native` is opt-in via `BROCKDJ_ENABLE_NATIVE_ARCH=ON` on Linux and Intel macOS only.
- Still open: audio cache redesign, FX thread-safety, analyzer lifetime ownership, joined DB/track-load workers, and `DjEngine` component split.
- Recommended next step: fix the FX data race with a small command/snapshot handoff before attempting the larger architecture split.

## Previous task
**MixerSection `polActive` QML scope fix**
- Polarity invert button: child bindings (`SilkLabel`, indicator `Rectangle`, hover overlay) referenced `polActive` unqualified; unlike `cueActive` (on `KnobStackColumn`), `polActive` is a custom property on the parent `Rectangle` and is not in child JS scope.
- Fix: `id: polBtn` on that `Rectangle`; children use `polBtn.polActive`.

## Previous task
**Scratch start lag fix**
Root causes at scratch begin:
1. **`emitPlaybackStateChanged()` in `tickScratchPhysics`** — fired progress/VU/gr NOTIFYs at 250 Hz (every 4 ms control tick), flooding QML repaints at scratch start.
2. **Triple waveform repaint path** — `scratchBySeconds` emitted `progressChanged` per drag event + `EnlargedWaveform` `onProgressChanged` + `FrameAnimation` + explicit `requestUpdate()` in `onPositionChanged`.
3. **`ScrollingWaveformItem` dual subsampling during scratch** — `subSamples=2` when not on playback snap-grid doubled per-column Catmull work every frame.
4. **Empty scratch RAM window on first audio block** — `ScratchResampler::reset()` cleared the window; first callback did synchronous stream reload on the audio thread (glitch + CPU spike).

Fixes:
- `tickScratchPhysics`: atomic playhead + throttled `notifyProgressIfNeeded()` only (~20 Hz).
- `scratchBySeconds` / release jog: no per-event `progressChanged`.
- `EnlargedWaveform` / `TurntableIndicator`: rely on `FrameAnimation` during play/scratch; `onProgressChanged` only when paused idle.
- `ScrollingWaveformItem`: `subSamples=1` always.
- `ScratchResampler::tryPrimeWindowFromDisk()` called from `beginScratch` before enabling scratch path (disk-backed tracks).

## Previous task
**AIO 2-Deck waveform placeholder slots**
- `AioWaveformInfoSlot.qml` — upper/lower placeholder panels ("Controls / Info") in 4-row layout.
- `main.qml`: `aioTwoDeckWaveformSlots` when AIO + 2-deck + performance tab; replaces C/D waveform slots.
- Desktop 2-deck unchanged (A/B only, 50% each); 4-deck unchanged (C/A/B/D waveforms).

### AIO sprint backlog (priority order)

| P | Item | Scope | Status |
|---|------|-------|--------|
| P0 | Library tab flows | Tile hub + playlist/smart pickers + quick load buttons | **mostly done** — polish filters/history tabs |
| P0 | Performance tab @ 10" | Deck controls touch-sized; `compactLayout` fits 1280×800 / 1024×600 | partial — compact FX/CF done; transport buttons TBD |
| P1 | Settings tab (AIO) | [`SettingsPanel`](src/qml/SettingsPanel.qml) scrollable, touch targets in embedded tab (`main.qml` `settingsPanelActive`) | open |
| P1 | Gesture stability | Swipe vs vertical scroll; close swipes on flick; no fight with preview bar | mostly done |
| P2 | AIO performance polish | Optional simplified mixer/FX overlay for touch (without forking engine) | open |
| defer | Desktop library power-user | Column resize, DnD reorder, keyboard browser — own sprint later | deferred |

### Definition of done (every AIO feature)

- Works @ **1280×800** and smoke @ **1024×600**
- Core path still OK in **DESK** (preview, load, SYNC)
- No new `allInOneMode` outside `main.qml` / `TopHeader.qml` / `Library.qml` without note in PR
- `./build-fast` + `./scripts/desktop-regression-checklist.sh`

## Previous task
**Library key match highlighting + track preview**
- **Camelot key match** — library rows compare each track key against loaded deck keys. Exact
  match = green (`accentKeyMatch`), compatible (±1 / relative) = blue (`accentKeyCompat`), subtle
  row tint. Updates when deck metadata changes.
- **Track preview** — new `LibraryPreviewPlayer` mixed into master bus (~72% gain, starts ~25%
  or 30s in). QML: `libraryPreview` context property; ♪ in AIO load bar, context menu,
  **P** hotkey. **`PreviewControlBar`** at library bottom: stop + scrub + time (desktop + AIO).
  Stops when loading to deck.

## Previous task
**Library AIO stability + sync master handoff**
- **Sync master = first SYNC press** — `s_syncEnableOrder` tracks enable order; master is the
  first entry still synced (not deck A in vector order). Disabling master removes it from order
  and propagates tempo from the next synced deck automatically.
- **AIO library swipe fixes** — horizontal swipe only wins when `|dx| > 1.25×|dy|`; vertical
  scroll no longer fights swipe. Animated snap on `swipeX`; swipes close on list scroll/flick.
- **AIO load UX** — `AioLoadBar` (▶ A/B/C/D) when a track is selected; load closes swipes and
  returns to performance tab in AIO mode.

## Previous task
**UI polish + AIO/10" optimization + remember last mode**
- **Layout mode persisted across launches** — new generic `SettingsManager::getUiState/setUiState`
  (`UI/` key prefix in the JUCE props file). `main.qml` `_restoreUiState()` (in
  `Component.onCompleted`) restores `allInOneMode` (via `setAllInOneMode` so mixer/FX stash runs),
  `fourDeckMode`, all `show*` toggles, and `activeMainTab`. `_persistUiState()` (200 ms debounce
  timer) writes on any `on*Changed`. Desktop mixer/FX intent is saved from `_desktopShow*` while AIO.
- **Responsive font scale enabled** — `responsiveFontScale` was hardcoded 1.0; now gently scales
  with window height (`clamp(height/800, 0.84, 1.18)`), 1.0 at the 800px reference (no regression
  at default size). Affects every `window.sp()/spViewport()` consumer; TopHeader keeps its own
  fixed `sp`.
- **Compact small-screen layout** — `compactLayout = height<720 || width<1100`. `fxBarHeight`
  90→74 and new `crossfaderBarHeight` 36→30 when compact, so AIO fits ~10" panels (1280x800/1024x600).
  Height accounting in `fixedPerformanceHeight`/`hiddenPerformanceHeight` uses these props.
- **TopHeader tab polish** — DESK/AIO button + LIB/⚙ tabs get accent (masterBlue) underlines on the
  active state, brighter hover, slightly wider; clearer at a glance.
- **VU color tokens** — new `UiTheme.vuLow/vuMid/vuHigh/vuClip/vuPeak/vuOff`; `MixerSection`
  channel meters now use them (were hardcoded). Header master meter keeps its own 4-tier gradient.
- Verified live via XWayland screenshot (xcb + xdotool + import): desktop renders correctly,
  compact 1024x600 layout works, header underline shows. App stable (no crash from changes).

## Previous task
**Scratch quality + performance rewrite (slow/precise focus)**
Root causes of the bad slow-scratch sound:
1. **Dual position authority** — UI thread slammed `m_readPos` to the hand position each
   event while the audio thread integrated a *smoothed/slewed rate*. Between events audio
   overshot, then snapped back on the next event → audible warble/zipper.
2. **Rate (not position) was authoritative** — hand motion → rate → smooth → slew → re-integrate
   lost precision; slow moves felt rubbery and lagged.
3. **Tiny scratch read window at low rate** — window margin scaled with rate, so slow scratch
   reloaded/decoded on the audio thread almost every move → crackle.

Fix:
- **Critically-damped (zeta=1) position tracker** on the audio thread
  (`ScratchResampler::processScratchTracking`, ~52 Hz bandwidth). Glides the read head to the
  absolute hand target (platter `m_targetSamplePos`): exact slow tracking, momentum across
  sparse UI events, no overshoot/snap. Used during **touch & no active loop**; inertia/release
  glide and loop-scratch keep the rate-integration path.
- **Single authority** — UI thread no longer writes `m_readPos` during drag; new
  `ScratchDeckBridge::publishScratchDisplay()` only publishes the visible playhead (+ hand pos).
  Audio measures the tracker rate back into the controller (`setMeasuredNormalizedSpeed`) for
  accurate release throws / inertia / timbre.
- **Time-based scratch window** — `ScratchResampler` holds ~0.5s capacity with a ~100ms/side
  margin floor, so slow scratching reads from RAM instead of decoding on the audio thread.

## Earlier task
**Preroll scratch snap fix** — scratching in negative pre-roll no longer jumps to t=0:
- `pauseForScrub`: capture `visualAtGrab` before flag changes; accept negative anchors
  from UI; never fall back to `transportSource.getCurrentPosition()` in pre-roll.
- `restorePostScrubPlaybackState` / `resumeAfterScrub`: preserve virtual negative position;
  audio handoff still uses `max(0, pos)`.
- `ScratchDeckBridge`: inertia playhead uses `m_scratchDisplaySec` (integrated), not
  resampler read head (clamped at 0).

Prior: command-path cleanup / Phase 1 (mixer MIDI sync, progress throttle, dead WaveformItem removed).
  via `MixerControl::setChannelFader()` (C++ path, includes CF multiplier). CrossfaderBar
  QML only mirrors `volA`–`volD` for UI; no duplicate audio apply. Keeps `m_faderA`–`m_faderD`
  aligned when MIDI/parameterStore moves bypass UI faders (same pattern as mix sync).
- **`MixerControl` MIDI mix state sync** — `syncMixFromNormalized()` after silent `apply*`
  for gain/EQ/filter.
- **`progressChanged` NOTIFY throttle** — `notifyProgressIfNeeded()` (~20 Hz); atomic playhead
  250 Hz; `OverallWaveform` FrameAnimation + atomic; `EnlargedWaveform` paused refresh 66 ms.
- **QML binding audit** — DeckControl, FxBar, FxUnit, PerformancePads, CrossfaderBar: clean.
  ScrollingWaveform already uses atomic/visual playhead + FrameAnimation (no change needed).
- **Dead code removed** — legacy `WaveformItem` + `DeckBoundQuickItem` (unused in QML;
  superseded by `RgbWaveformItem` / `ScrollingWaveformItem`).

Prior session: `DeckChannels.h`, unified MIDI mixer via `MixerParameterBridge`,
silent `apply*()`, FxManager dedup, VU NOTIFY ~20 Hz, EQ/filter coeff cap.

## Previous task (real-time mixer / audio engine performance)
Knob/fader lag was caused by UI-thread NOTIFY churn + unthrottled VU repaints, not
missing audio routing. Fixes:
- **`apply*()` vs `set*()` on DjEngine** — `MixerControl` / `FxManager` hot path calls
  `applyTrim/applyEq*/applyFilter/applyVolume` (audio only, no Qt NOTIFY). `set*()` kept
  for MIDI/Q_PROPERTY and emits signals.
- **VU meter NOTIFY throttled** — `notifyVuMetersIfNeeded()` in `onTimer` (was 250 Hz
  `vuLevelChanged` per deck → full VU Repeater repaints). Now ~20 Hz max or on
  meaningful level change (±0.015).
- **EQ/filter coeff updates on audio thread** — `MixerDspSource`: 12 ms SmoothedValue on
  EQ bands + filter, rate-capped ~60 Hz `updateFiltersFromValues()` (no per-knob IIR
  recompute storm while dragging).

## Previous task (stability / DB recovery / cleanup)
- **DB recovery warning is now precise.** Previously `SettingsManager.previousRunUnclean`
  (App/CleanShutdown flag) fired on every abrupt exit including Ctrl+C with no library
  writes. Now `LibraryDatabase::recoveryWarningNeeded` is true only when the previous
  session ended uncleanly **and** `Meta.session_dirty == 1` (any library write this
  session via `scheduleBackupSync` → `markSessionDirty`). Ctrl+C with no mutations →
  no dialog. `main.qml` reads `libraryDb.recoveryWarningNeeded` / `recoveryWarningMessage`.
  Session markers: `session_closed_cleanly` (0 while open, 1 on `shutdown()`), `session_dirty`.
- **SIGINT/SIGTERM → graceful quit** (`ApplicationBootstrap.cpp`): handler calls
  `QCoreApplication::quit()` so `shutdownApplication` runs DB checkpoint + clean markers.
- **Sync cleanup** (`DjEngine_Sync.cpp`): shared `wrapUnitPhase()`, `std::ranges::find_if`
  for downbeat lookup, `currentSyncMaster()` reuse in phase correction.

## Previous task (Serato-style multi-deck sync)
Sync previously matched BPM + only **sub-beat** phase (`getBeatPhase`), so beats
could lock while bars/downbeats did not — and a synced follower could drop a beat
off. Now sync arranges the **beatgrids on the bar** like Serato.
- **`getBarPhase()`** (`DjEngine_Sync.cpp`, Q_INVOKABLE): 0=downbeat … 0.25/0.5/0.75
  = beats 2/3/4, anchored to the first `isDownbeat` marker (offset `% 4`), built on
  `getBeatPosition()`; BPM/first-beat fallback when no grid.
- **Bar (downbeat) alignment via seek** on the events that "arrange":
  - `snapPhaseToMaster` (sync enable) → full bar align, wrap ±0.5 bar (max 2-beat jump).
  - `reSync` (manual) → full bar align + boosted residual cleanup.
  - `alignToSyncMasterOnPlay` (new) → when a synced **follower starts playing**
    (`play()` / `togglePlay()` play branch) it re-derives tempo from the current
    master then bar-snaps, so it drops in phase-locked.
  - Shared clamped seek helper `applySyncSeekOffset` (handles pre-roll).
- **Continuous lock is a PI controller** (`updatePhaseCorrection`) on beat-phase
  error, output = tempo nudge (no seeks, no jumps). Proportional snaps phase;
  **integral cancels systematic tempo bias** (analysed BPM ≠ true grid spacing,
  or a clamped sync tempo %) so decks don't drift apart. Gains: kP 14 (%/beat),
  kI 9 (%/(beat·s)), clamp ±6% (±15%+kP 30 during reSync boost); anti-windup clamps
  the integral to the nudge ceiling; `m_phaseIntegral` + `m_phaseClock` reset on
  pause / master-loss / seek-align / sync-off. (The old pure-P + 0.02-beat deadband
  left a steady bias → sawtooth drift; a too-low kI=2 left a ~15 s settling creep
  that read as "aligns then slowly drifts". Higher gains converge in <10 s even in
  the extreme 100→200 BPM clamp case, then hold ±0.01 beats with no drift.)
  `currentSyncMaster()` helper wraps the `s_syncMutex` lookup.
- **UI: sync master highlighted.** `DeckControl.qml` SYNC button shows "MASTER" in
  gold (`#ffb000`) when `engine.syncMaster`, plain green "SYNC" otherwise, so the
  tempo/beat reference deck is obvious.
- Tempo match is exact: follower pct = `(masterBpm/baseBpm - 1)*100` →
  `getCurrentBpm()==masterBpm`. Master tempo-fader moves already propagate.
- **Verified live (2 decks, 100 vs 189 BPM, 1.89× stretch stress case):** on follower
  sync, bpmB→189.474 (=master); bar-diff collapses from −1.7 beats to ~0; over **38 s
  of locked playback it stays within ±0.005 beats (~1.5 ms) with NO growing trend** —
  the PI integral eliminates the drift.

## Previous task (stability / quantize / threading)
1. **Quantize for cues (CDJ-3000 style).** Two parts, both needed:
   a. **Store-time snap** — with `m_quantizeEnabled`, the stored point snaps to
      `quantizedBeatAt()` at SET time (`storeHotCue`, `cueButtonPress` both branches).
   b. **Trigger-time deferral (the part that was missing).** While *playing* with
      quantize on, a cue press does NOT jump immediately — it is deferred to the
      **next beat boundary** so the jump lands phase-locked on the grid, exactly
      like a CDJ. Source beat → target beat are an integer number of beats apart.
      - State + helpers in `DjEngine.h`: `m_pendingCueJump{Active,FireSec,TargetSec,LastPos}`,
        `scheduleQuantizedCueJump` / `serviceQuantizedCueJump` / `cancelQuantizedCueJump` /
        `performCueJump` (shared immediate-jump logic) / `nextBeatBoundaryAfter`.
      - `nextBeatBoundaryAfter` (`DjEngine_Loop.cpp`): first grid line > sec
        (upper_bound), else extrapolate via local beat length / bpm fallback.
      - `triggerHotCueJump` (`DjEngine_HotCue.cpp`) + `cueButtonPress` playing branch
        (`DjEngine_MainCue.cpp`): if quantize+playing+pos≥0 → `scheduleQuantizedCueJump`,
        else `performCueJump` immediately (paused = instant, as before).
      - Serviced every 4 ms control tick: `onTimer` calls `serviceQuantizedCueJump`
        in the playing branch; fires when transport pos reaches `fireSec` (or a loop
        wrapped first). Cancelled on scrub (`onTimer` top), pause/stop
        (`freezeTransportAt`), seek (`setPosition`), track reset.
      - Timing is control-tick accurate (~one audio block ≈ 10–15 ms late at trigger
        instant); landing position is exact (hard-set to the on-grid target). Verified
        live: stored cue 2.17→snap 2.2167s; trigger at 5.17s deferred to 5.383s (=+6
        beats) → phase-locked. Sample-accurate audio-thread firing is a future option.
2. **Hardware-adaptive thread management** (scales x86 ↔ ARM64, no hard-coding):
   - `DjEngine_TransportLoad.cpp`: global `QSemaphore trackLoadGate` sized
     `clamp(hw/2, 1, 6)` bounds concurrent background loads across all 4 decks
     (each opens several decoders + scans audio); loader threads also drop to
     `nice 10` on Linux (`lowerCurrentThreadPriority`) so audio/UI keep priority.
   - `WaveformAnalyzer.cpp`: the existing analysis cap is now
     `maxConcurrentAnalyses()` = `clamp(hw/3, 1, 4)` instead of the fixed `2`
     (analyzer threads already run at `juce::Thread::Priority::background`).
3. **Loops** already engage immediately (`applyLoopRangeToAudioSource`); the
   perceived loop "hang" was the waveform scroll freeze fixed earlier.
4. **Pre-existing smoke-test crash fixed** (`tests/smoke/mixer_dsp_smoke.cpp`):
   `MixerDspSource` carries large fixed delay/echo/brake buffers; stack-allocating
   it in the test overflowed the stack → SIGSEGV. Now `make_unique` like the real
   engine. `ctest` green again. (App was always safe — `DjEngine` heap-allocates.)

## Previous task
Mixer channel strip (EQ/GAIN/SC/channel fader) still dead + scrolling waveform
"hangs" at loops/hotcues — **both fixed**.

### Mixer channel strip — two compounding QML bugs (`MixerSection.qml`)
1. **Grouped-alias signal handler never fires.** Knob cells wired audio via
   `knob.onValueChanged: …` where `knob` is `property alias knob: innerKnob`. A
   grouped-alias *signal handler* at the use site is silently dropped, so the
   knobs never called `mixerControl`. Fix: `KnobCell` now exposes a real
   `signal moved(real value)` emitted by a **direct** `onValueChanged: kc.moved(value)`
   on the inner `Knob`; use sites use `onMoved: (v) => mixer.setXxxValue(channelId, v)`.
2. **Self-referential `channelId: channelId`.** In `ChannelSide`, bindings like
   `channelId: channelId` / `cueActive: cueActive` / `mirrored: mirrored` /
   `deckName: deckName` on child components (`KnobStackColumn`, `FaderColumn`,
   `ChannelHeader`) that *own a property of the same name* resolve the RHS to the
   child's own (empty) property → `channelId` arrived as `""`, so
   `deckForChannelId("")` returned null and nothing reached a deck (the channel
   fader was dead for the same reason). Fix: gave `ChannelSide` `id: side` and
   qualified every such binding as `side.<prop>`.
   Verified live: forcing a knob value logged `setEqHigh "deckA" 0.5 deck=0x…`
   (was `setEqHigh "" 0.5 deck=0x0`). The C++/`MixerControl`→deck path was always
   fine (startup `applyAllControls` reached valid decks).

### Scrolling waveform loop/hotcue "hang" (`ScrollingWaveformItem.cpp`)
The monotonic playback anchor only treated large **forward** deltas as a seek;
a large **backward** jump (loop wrap to loop-in, hotcue/cue jump) fell through to
"hold last center" → waveform froze at loop-out until playback caught up. Fix:
`largeSeek = std::abs(delta) > 2px` follows real jumps in **either** direction;
only sub-2px motion opposing the play direction is suppressed (jitter guard).

## Previous task
"Whole mixer does nothing" — **root cause was audio device, not mixer QML**. Fixed.

### Root cause
Saved output device `DDJ-FLX10 Analog Surround 4.0` (controller unplugged) failed to
open. The fallback chain in `applyAudioDeviceSettingsExpected` (`DjEngine_Settings.cpp`)
could return *no error* yet leave **no open device** ("Retrying with default output
device" → still not ready). The code then **restored the broken saved device** and
returned failure → no active audio → every mixer/deck control was inaudible, so the
mixer looked dead.

### Fix
Added a last-ditch recovery before the restore-previous block: when no device is ready
after all targeted fallbacks, call `initialiseWithDefaultDevices(0, 2)` and, on success,
reset to safe stereo routing (`OutputRoutingConfig{}`, master ch1, booth/HP disabled) +
show a fallback message instead of restoring the unusable device. Verified in logs:
`No usable output after fallbacks; forcing system default device` → `Backend output
latency ... sr 48000` (device open at startup; no more "restoring previous device" /
"Play requested without active audio device").

> Verified in the live binary after a clean `./build-fast`.

## Build layout (consolidated 2026-06-27)
Single build dir only: **`build/`**, produced by **`./build-fast`** (preset `linux-dev-fast`,
Ninja, RelWithDebInfo, QML cachegen off). The old `build/` (stale ASAN) and `build-dev/`
were deleted; `CMakePresets.json`, `scripts/build-fast-linux.sh`, `.gitignore`, README and
`.cursorrules` all point at `build/`. Always build with `./build-fast`; run `./build/bin/BrockDJ`.
Do not recreate `build-dev/`.

## Previous task
Mixer EQ / SC / volume faders fix — **completed**.

## What changed
1. **`MixerControl`** created **before** `engine.load()` and exposed as context property **`mixerControl`** (not `MixerApi` singleton — `qmlRegisterSingletonInstance` on URI `DJSoftware` breaks `QML_ELEMENT` types like `ScrollingWaveformItem`).
2. **`applyAllVolumes()`** — CrossfaderBar syncs CF state then applies stored fader levels; no longer overwrites faders from stale `volA`–`volD` defaults.
3. **`MixerSection.qml`** / **`CrossfaderBar.qml`** call **`mixerControl`** directly for trim, EQ, filter, fader, cue.
4. **Smoke test** (`tests/smoke/mixer_dsp_smoke.cpp`) verifies `MixerDspSource` trim attenuation and high-EQ boost on HF sine.

## Manual verification (user)
1. Play track on deck A — VU moves
2. Turn **GAIN** down — VU level drops
3. Turn **HI** full cut — treble dulls
4. Move **channel VOL** to zero — deck silent with CF centered
5. Crossfader still blends A/B

## Architecture note
- Volume path: `mixerControl.setChannelFader` → `MixerControl::applyChannelVolume` → `DjEngine::setVolume` × CF multiplier
- EQ/trim/filter: `mixerControl.setTrim/setEq*/setFilter` → `DjEngine` → `MixerDspSource`

## Current task: cue/loop extraction

`DeckCueLoopController` is now the single owner of main-cue interaction state, eight hot cues, active loop/in/out/beat length, eight saved loops, pending quantized cue jumps and cue/loop track generation. `DjEngine_CueLoopFacade.cpp` preserves the existing QML/MIDI API, Qt signals, SQLite persistence and actual transport/audio application. Load/eject resets the controller with the `DeckTrackLoader` generation, invalidating deferred jumps. The former Loop/HotCue/MainCue/SavedLoops implementation files are consolidated.

## Current task: DeckTrackLoader extraction

`DeckTrackLoader` now owns one joinable low-priority worker, a latest-request slot, generation/state/cancel/shutdown and a private decoder manager. It prepares canonical identity, two playback readers, structured metadata, waveform cache/overview, cover bytes/image and auto-cue without touching QML, SQLite or the audio callback. `DjEngine_TransportLoad.cpp` queues one `QPointer`-guarded result to the Qt owner, revalidates generation, hydrates existing DB analysis/cues, applies TrackData, starts missing analysis and hands readers to the unchanged playback pipeline. The old detached `[this]` loader and `DjEngine` load mutex/generation fields are removed. Next architecture task: foundations for `AudioPage`, `AudioCacheHandle`, `AudioPageCache` and `AudioCacheWorker`; no scratch/playback integration in that task.

## Current task: global AudioPageCache foundation

ApplicationRuntime owns one shared cache: 16,384-sample planar immutable pages, canonical path/size/mtime keys, shared versioned handles, five bounded priority queues, one joined decoder worker, 256 MiB default budget, worker-only LRU eviction, guarded atomic lookup and atomic stats. Normal playback is not connected; scratch integration is described below.

## Current task: cache-only ScratchResampler

Each deck now receives the application cache and opens/releases one versioned handle per prepared track. ScratchResampler no longer contains an AudioFormatReader/AudioSource or disk/stream reload functions. Its preallocated ~0.5 s window is filled only from guarded immutable pages, including cross-page Hermite neighborhoods and mono duplication. Direction-aware current/near-page requests use bounded priorities. Misses retain the old window where valid and apply a deterministic 128-sample hold/fade-out; recovery fades in. Automated stress keeps `diskReadsFromAudioThread == 0`. Normal playback remains on BufferingAudioSource/Hermite transport.

## Blocked next task: TimeStretch/RubberBand realtime cleanup

The requested prerequisite is not present: normal playback still uses `AudioFormatReaderSource` → `BufferingAudioSource` → `ReverseStreamAudioSource`, there is no cached-playback test, and no `decoderCallsFromAudioThread` proof. The RubberBand audit confirms callback-unsafe work (`getNextAudioBlock` → `resetRealtimePipeline(true)` → `RubberBand::reset` + `prewarmStretcher`, plus ratio setters), but the prompt explicitly forbids starting this refactor until cached playback is complete. Next task must first migrate only normal playback read-ahead to AudioPageCache.

## Current task: DeckTransport extraction (2026-07-13)

`DeckTransport` is now the domain owner for play intent, audible/held/slip-background position, seek/rate/reverse/slip, negative pre-roll, EOF, length and track/state generations. It sends commands only through narrow `DeckAudioGraph` methods and publishes a pointer-free consistent atomic snapshot. `DjEngine` retains its public QML/MIDI/controller surface, signals, validation, history, sync decisions and cue/loop persistence; its timer delegates transport transitions to `updateControlState()`. Direct `transport()`/`playback()` source access from all `DjEngine_*.cpp` files is removed. Next: extract `DeckSyncController`/`SyncCoordinator`; do not combine it with a general control-clock rewrite.
