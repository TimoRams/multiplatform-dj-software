# Progress

## Done (recent)
- [x] **UI mode persistence** — `SettingsManager::getUiState/setUiState` (`UI/` keys);
  `main.qml` restores `allInOneMode`/`fourDeckMode`/`show*`/`activeMainTab` on launch and
  persists on change (200 ms debounce). App remembers desktop vs AIO between sessions.
- [x] **Responsive font scale** — `responsiveFontScale` (was stub 1.0) now `clamp(height/800,
  0.84,1.18)`; UI legible on big screens, fits ~10" panels. No change at the 800px default.
- [x] **Compact layout for small screens** — `compactLayout` shrinks FX bar (90→74) and
  crossfader (36→30) so AIO performance fits 1280x800 / 1024x600.
- [x] **TopHeader tab polish** — DESK/AIO + LIB/⚙ get accent underlines on active, brighter hover.
- [x] **VU color tokens** — `UiTheme.vu*`; mixer meters unified (header master meter unchanged).
- [x] **Scratch quality + perf rewrite** — critically-damped position tracker
  (`ScratchResampler::processScratchTracking`) makes slow/precise scratching track the hand
  exactly with no warble/snap-back; single position authority (UI publishes display only,
  audio owns read head); time-based ~0.5s scratch window so slow scratch reads from RAM
  instead of decoding on the audio thread. Loop-scratch + inertia keep the rate path.
- [x] **Preroll scratch snap fix** — negative pre-roll scratch keeps virtual playhead;
  no snap to file start on grab, drag, inertia, or release.
- [x] **Crossfader MIDI via MixerControl** — `crossfader` param → `MixerParameterBridge`
  → `setCrossfaderPosition()`; CrossfaderBar UI mirror only (no duplicate audio apply).
- [x] **Dead waveform types removed** — `WaveformItem` + `DeckBoundQuickItem` deleted
  (unused; QML uses `RgbWaveformItem` / `ScrollingWaveformItem`).
- [x] **MixerControl MIDI fader sync** — `deckX_vol` routed through `MixerParameterBridge`
  → `setChannelFader()`; CrossfaderBar QML UI-only for vol props.
- [x] **EnlargedWaveform paused refresh** — 33 ms → 66 ms (~15 fps) when deck paused.
- [x] **MixerControl MIDI mix state sync** — `syncMixFromNormalized()` + bridge calls after
  `apply*` so internal mix state matches parameterStore/MIDI moves.
- [x] **`progressChanged` throttle** — `notifyProgressIfNeeded()` in transport tick (~20 Hz);
  atomic playhead still 250 Hz; `OverallWaveform` uses FrameAnimation + atomic.
- [x] **QML binding audit** — DeckControl/FxBar/FxUnit/PerformancePads/CrossfaderBar: no
  grouped-alias or self-ref binding bugs; OverallWaveform playhead fixed for throttle.
- [x] **Mixer command-path cleanup** — `DeckChannels.h` shared lookup/scaling; MIDI gain/EQ/filter
  only via `MixerParameterBridge` (`apply*`); duplicate block removed from `MidiFlx10Bridge`;
  `FxManager` filter dedup + `applyFilter` on mode switch; dead `updateGain()` removed.
- [x] **Real-time mixer performance** — silent `apply*()` path for knob/fader drag (no Qt
  NOTIFY storm); VU NOTIFY throttled ~20 Hz; EQ/filter IIR coeffs smoothed + capped ~60 Hz
  on audio thread.
- [x] **DB recovery warning only on real risk** — `session_dirty` + `session_closed_cleanly`
  Meta keys; warning only when abrupt exit AND pending library writes; Ctrl+C with no
  writes silent. SIGINT/SIGTERM → graceful quit + WAL checkpoint.
- [x] **Sync code cleanup** — `wrapUnitPhase`, `std::ranges::find_if` for downbeats.
- [x] **Serato-style multi-deck sync** — align beatgrids on the **bar/downbeat**, not just BPM +
  sub-beat phase. New `getBarPhase()`; `snapPhaseToMaster`/`reSync`/`alignToSyncMasterOnPlay`
  bar-align via seek (`applySyncSeekOffset`).
- [x] **Sync drift fixed (PI phase-lock)** — continuous `updatePhaseCorrection` is now a PI
  controller; the integral term cancels systematic tempo bias so synced decks stop drifting
  apart over time (was pure-P + deadband). Gains retuned (kP 14 / kI 9, ±6%) to kill the
  ~15 s settling creep ("aligns then slowly drifts"); verified locked ±0.01 beats over 50–90 s
  including the extreme 100→200 BPM case, no growth, no oscillation.
- [x] **Sync master highlighted** — `DeckControl.qml` SYNC button shows gold "MASTER" when the
  deck is the sync master, plain green "SYNC" otherwise.
- [x] **Quantize cue *trigger timing*** — while playing with quantize on, hot cue / main cue
  presses are deferred to the next beat boundary (`scheduleQuantizedCueJump` →
  `serviceQuantizedCueJump` in `onTimer`, `nextBeatBoundaryAfter` in `DjEngine_Loop.cpp`) so the
  jump lands phase-locked on the grid. Cancelled on scrub/pause/seek/reset. Verified live.
- [x] **Hardware-adaptive thread management** — `QSemaphore` gate caps concurrent track loads at
  `clamp(cores/2,1,6)` + Linux `nice 10` on loader threads; analysis cap now `clamp(cores/3,1,4)`
  (was fixed 2). Scales from low-core ARM64 to many-core x86; keeps audio/UI responsive under load.
- [x] **Smoke-test crash fixed (pre-existing)** — `MixerDspSource` heap-allocated in
  `tests/smoke/mixer_dsp_smoke.cpp` (stack alloc overflowed the stack via large delay buffers).
  `ctest` passes again.
- [x] **Mixer channel strip fix (real root cause)** — `MixerSection.qml`: (1) replaced silent
  grouped-alias `knob.onValueChanged:` with a `KnobCell.moved` signal + `onMoved:`; (2) fixed
  self-referential `channelId/cueActive/mirrored/deckName: <same>` bindings via `id: side` +
  `side.<prop>`. Knobs/EQ/SC and channel faders now reach the deck (verified in logs).
- [x] **Scrolling waveform loop/hotcue hang fix** — `ScrollingWaveformItem.cpp` anchor now
  follows large jumps in *either* direction; only sub-2px reverse jitter is suppressed.
- [x] **Audio device fallback fix** — unplugged saved device (DDJ-FLX10) no longer leaves the app
  with no output; `DjEngine_Settings.cpp` now forces `initialiseWithDefaultDevices` + safe stereo
  routing instead of restoring the broken device. Was the real cause of "mixer does nothing".
- [x] Full codebase restructure (bootstrap, lifecycle, split MIDI/library/waveform/FLX10, smoke tests, CI macOS)
- [x] FLX10 jog display fix, `.gitignore` for `.cursor/` (with memory bank exception)
- [x] Multiple mixer fix attempts (parameterStore bridge, direct engine bindings)
- [x] **Mixer routing fix** — `MixerApi` singleton, `applyAllVolumes()`, QML wired to C++ facade
- [x] **Mixer DSP smoke test** — trim + high EQ verified in `tests/smoke/mixer_dsp_smoke.cpp`

## In progress
- [ ] User confirmation: mixer EQ / SC / volume faders + waveform loop scroll feel right in live app
- [ ] Cursor hide on knob/fader drag (guards added; verify on macOS)

## Known issues
- Watch for grouped-alias signal handlers (`alias.onXxx:`) and self-referential `prop: prop`
  bindings in new QML — qualify with parent `id` (MixerSection pattern).
