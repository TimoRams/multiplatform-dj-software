# Progress

## Done (recent)
- [x] **Quantize for hot cues + main cue** — snap stored point to beat grid when quantize is on
  (CDJ-3000 style), in `DjEngine_HotCue.cpp` / `DjEngine_MainCue.cpp`. Loops were already quantized.
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
- Watch for the same QML pitfalls elsewhere (DeckControl/FxBar/PerformancePads): grouped-alias
  signal handlers (`alias.onXxx:`) silently don't fire, and `prop: prop` bindings self-reference
  when the child owns a property of the same name — qualify with the parent's `id`.

## Not started / backlog
- Commit/push mixer fix (only when user asks)
- Optional: deduplicate `MixerParameterBridge` vs `MidiFlx10Bridge` gain/EQ handlers
