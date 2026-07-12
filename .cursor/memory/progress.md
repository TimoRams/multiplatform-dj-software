# Progress

## Done (recent)
- [x] **First controlled `DjEngine` split** — complete method/state ownership map plus
  runtime-owned, constructor-injected `AudioDeviceService`; one manager/configuration for all four
  decks, unchanged QML forwarding API, explicit callback/shutdown order and focused service tests.
- [x] **Async-signal-safe POSIX shutdown handoff** — portable self-pipe with `sigaction`,
  nonblocking/CLOEXEC descriptors, Qt-thread notifier delivery, duplicate suppression, prior-handler
  restoration and focused real-signal tests. Linux full-app Ctrl+C/SIGTERM headless checks passed.
- [x] **Analyzer lifetime and stale completion safety** — explicit job states/generations,
  separate cancel and deterministic join, safe manager `QPointer` callback plus manager/analyzer
  generation and path checks, owner-thread RGB coalescing, and focused lifetime tests. ASan/UBSan
  and TSan test builds pass.
- [x] **FX type handoff made realtime-safe** — packed atomic generation/type command from
  UI/MIDI/control producers; audio thread consumes once before per-block FX routing. Pitch and
  roll resets are allocation-free metadata resets; all resources remain prepared in `prepare()`.
  Added all-effect/multi-block-size and deterministic two-thread parameter/switch stress tests.
- [x] **Stability/realtime preparation docs** — added `.cursor/memory/riskRegister.md`,
  `.cursor/memory/regressionChecklist.md`, and `.cursor/memory/realtimeRules.md`.
- [x] **`DjEngine::onTimer` sync braces/policy** — both `updatePhaseCorrection()` and
  `updateTightDoubleAlignment()` now run only under the shared follower-sync predicate;
  smoke test covers sync-off/master/scratch/release-glide cases.
- [x] **Build hygiene** — removed duplicate `src/domain/TrackData.cpp` from CMake sources.
- [x] **Version source of truth** — CI packaging reads `APP_VERSION` from CMake
  `PROJECT_VERSION` instead of hardcoding a conflicting value.
- [x] **Portable release default** — `-march=native` is no longer enabled by default;
  local native builds require `BROCKDJ_ENABLE_NATIVE_ARCH=ON` on Linux/Intel macOS.
- [x] **AIO CDJ drill-down browse** — split picker + hover preview + tap full-width track list;
  `aioPreviewBrowseEntry` / `aioDrillBrowseEntry` / `aioBrowseUnDrill`; back button in toolbar.
- [x] **AIO library CDJ-style hub** — tile grid navigation, playlist/smart pickers, touch quick buttons on rows.
- [x] **AIO-first strategy** — sprint backlog in `activeContext.md`; shared-first rule in `.cursorrules` /
  `techContext.md`; `scripts/desktop-regression-checklist.sh`; AIO profile flags in `main.qml`.
- [x] **Preview control bar** — scrub slider + stop + time display in library (AIO + desktop);
  `LibraryPreviewPlayer` seek/position API; full-track playback to end.
- [x] **Key match highlighting** — Camelot exact/compatible keys vs loaded decks; row tint + key color.
- [x] **Library track preview** — `LibraryPreviewPlayer` via master bus; ♪ bar, P key, context menu.
- [x] **Sync master by enable order** — first deck to press SYNC stays master; disabling master
  hands off to the next synced deck and retunes followers.
- [x] **AIO library swipe stability** — gesture discrimination vs vertical scroll, snap animation,
  auto-close on list movement.
- [x] **AIO quick-load bar** — tap ▶ A/B/C/D without swipe; auto-return to performance tab.
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

## In progress (AIO sprint)
- [ ] **P0 Library tab flows** — polish history period tabs + filter panel touch sizing
- [ ] **P0 Performance touch** — larger transport/pad targets when `compactLayout` / AIO
- [ ] **P1 Settings tab AIO** — embedded `SettingsPanel` touch-friendly (scroll, tap targets)
- [ ] User confirmation: mixer EQ / SC / volume faders + waveform loop scroll feel right in live app
- [ ] Cursor hide on knob/fader drag (guards added; verify on macOS)

## Desktop regression checklist

Run after each AIO UI change:

```bash
chmod +x scripts/desktop-regression-checklist.sh   # once
./scripts/desktop-regression-checklist.sh
```

Manual (~15 min):
- [ ] Library side-by-side: scroll, preview P + scrub/stop, load to deck
- [ ] Hotkeys: P preview; deck transport unchanged
- [ ] AIO → DESK: mixer + FX bar restore (`setAllInOneMode` stashes `_desktopShowMixer/FxBar`)
- [ ] AIO @ 1280×800 and 1024×600: LIB → load → Performance tab return
- [ ] SYNC + master handoff still works

## Known issues
- P0 realtime/threading risks remain open: scratch cache misses,
  RubberBand reset/prewarm in callback, and mixer filter coefficient work.
- Analyzer lifetime is fixed; a later improvement may publish final analysis metadata as one
  immutable snapshot. Detached general track-load workers remain a separate P1 issue.
- Audio cache redesign and remaining `DjEngine` components are intentionally still open.
- `DeckCueLoopController` extraction completed: domain state has one owner, delayed jumps are
  generation-scoped, the four old implementation files are one facade, and the dedicated
  controller test plus all existing CTest targets pass.
- Recommended next step: extract `DeckTrackLoader` (track replacement, decoder creation,
  metadata/cover, analysis job and generation); do not move `DeckAudioGraph`, audio cache or
  general transport code in that step.
- `DeckTrackLoader` extraction completed: detached raw-`this` loading was replaced by one
  joined worker; generation, latest-request cancellation, decoder/metadata/cover/waveform
  preparation and a move-only result are covered by generated-WAV tests.
- Recommended next step: create the foundations for `AudioPage`, `AudioCacheHandle`,
  `AudioPageCache` and `AudioCacheWorker` with fixed pages, budget, prioritised requests,
  generation and joined shutdown; do not integrate scratch or normal playback yet.
- Watch for grouped-alias signal handlers (`alias.onXxx:`) and self-referential `prop: prop`
  bindings in new QML — qualify with parent `id` (MixerSection pattern).
