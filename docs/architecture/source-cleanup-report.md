# BrockDJ Source Cleanup Report

Date: 2026-10-03
Baseline: `9e18081d69cf0c8037ed053239726ee147808c74`
Scope: source organization and dependency cleanup while preserving playback,
waveform, controller and persistence contracts. Follow-up UI interaction
corrections are listed explicitly below.

## Deleted

- `ControllerProfile.h`: no production or test consumer.
- Obsolete top-level domain CMake files after their contents were migrated.
- Superseded value/helper headers after their declarations were moved to the
  owning consolidated header.

## Merged

- Four beat-analysis algorithms -> `analysis/internal/BeatAnalysis.*`.
- Beat-grid validation -> `analysis/internal/BeatAnalysis.*`; its duplicate
  smoke fixture was removed in favor of the dedicated beat-analysis tests.
- Analysis value types -> `analysis/AnalysisTypes.h`.
- Waveform line/chunk/batch/normalization values -> `waveform/WaveformTypes.h`.
- Waveform tile, marker and timeline math ->
  `waveform/render/WaveformRenderMath.h`.
- Eight `DjEngine` facade fragments -> `DjEngine.cpp`,
  `DjEngineTransport.cpp`, `DjEngineCues.cpp` and
  `DjEnginePerformance.cpp`.
- Sync maintenance helper -> `deck/sync/DeckSync.h`.
- Library schema/core -> `LibraryDatabase.cpp`; track/cue/playlist persistence
  -> `LibraryPersistence.cpp`.
- Audio page and handle values -> `audio/cache/AudioCacheTypes.h`.
- Track segment and transport-limit values -> `domain/DomainTypes.h`.
- CI smoke/application entry declarations -> `app/ApplicationBootstrap.h`;
  the exit gate now lives with `ApplicationLifecycle`.

## Moved and renamed

- `rendering/` -> `waveform/`, `waveform/internal/` and `waveform/render/`.
- `RgbWaveformItem` -> `OverviewWaveformItem` to name its real role.
- `engine/` -> `deck/`; `.hpp` scratch headers were normalized to `.h`.
- `database/DatabaseWorker` -> `library/persistence/DatabaseWorker`.
- `io/MediaIoScheduler` -> `library/MediaIoScheduler`.
- `midi/` -> `controllers/midi/`; the FLX10 bridge moved to
  `controllers/flx10/Flx10MidiBridge.cpp`.
- FLX10 protocol/display files -> `Flx10Protocol.h` and `Flx10Display.cpp`.
- `fx/BrickwallLimiter` -> `audio/internal/BrickwallLimiter`.
- QML files -> domain folders without changing their module-visible type names.

## Inlined or made private

- Analysis implementation helpers and waveform orchestration/envelope code are
  explicitly internal rather than public top-level APIs.
- The trivial `CursorControl` QML adapter is header-only instead of carrying a
  separate implementation translation unit.
- First-run configuration, UI scaling and waveform zoom now compile with their
  shared `SettingsManager` persistence owner instead of four tiny translation
  units.
- The one-consumer file-manager launch adapter retains its platform boundary as
  a header-only helper.
- Track metadata parsing now lives privately with `DeckTrackLoader`; the former
  public helper pair and its unused ID3v1 fallback were removed.
- FLX10 jog constants now live beside the jog router that owns their semantics.
- The broad `FacadeIncludes.h` dependency umbrella was removed; every remaining
  `DjEngine` implementation declares direct dependencies.

## Kept intentionally

- The protected audio owners remain separate: `AudioEngine`,
  `DeckAudioPipeline`, `MasterMixer`, `HeadphoneBus`, `AudioOutputRouter`,
  `AudioDeviceService`, `AudioPageCache` and `TimeStretchProcessor`.
- `AudioCacheWorker`, `DatabaseWorker`, `MediaIoScheduler` and
  `Flx10HidTransport` retain separate thread, lifecycle or hardware boundaries.
  The small cache worker is now private within `AudioPageCache.cpp`; a distinct
  lifetime does not require a separate public header and translation unit.
- `analysis/internal/AnalysisOrchestrator` remains an internal large
  implementation unit; merging it into `WaveformAnalyzer.cpp` would create a
  God translation unit.
- `VirtualTurntable` remains independently testable, inline simulation logic;
  `ScratchSession` remains the deck-owned state boundary.
- Large MIDI implementation units remain separate around enumeration, mapping,
  dispatch and FLX10 behavior; merging them would exceed useful file size.
- Large QML surfaces retain their ownership boundaries; repeated interaction
  and action logic is consolidated inside the existing components rather than
  split solely to reduce file size.

## Follow-up consolidation

- Removed the unused controller profile and `OverallWaveform.qml`.
- Merged `UiMetrics` into `UiTheme`, desktop settings into the shared
  `SettingsPanel`, and first-run content into the startup component in `AppOverlays`.
- Merged mixer coefficient code into `DeckChannelProcessor`, the one-consumer
  track-ID helper into `DjEngine`, and Rekordbox reader declarations into
  `RekordboxDeviceSource.h`.
- Moved `SystemMonitor` to `platform/`, analysis orchestration to `analysis/`,
  and performance-only QML panels to `qml/performance/`.
- Removed the persisted waveform LOD trailer. Render-cache version 5 stores
  canonical lines only; renderers derive bounded LOD samples on demand.
- Released duplicate full geometry and spectral snapshots after immutable
  waveform lines are installed. The line snapshot itself remains the reusable
  analysis seed, avoiding both per-deck RAM duplication and repeat decoding.
- Removed duplicated synchronous analysis persistence and stale FLX10 display
  generation paths.

## Source consolidation after the full-tree inventory

- Inlined `VirtualTurntable.cpp` into its existing header without changing
  position, angle or atomic access semantics.
- Combined the two UI preference declaration headers into `app/UiPreferences.h`,
  keeping `UiScaleController` and `WaveformZoomController` as distinct QObjects.
- Combined LOD sampling and column aggregation in `WaveformAggregator.*`;
  immutable line storage, canonical analysis and renderer lifetimes remain separate.
- Grouped ALSA input/output into `controllers/midi/AlsaMidiTransport.*`;
  the two backend classes retain independent handles and shutdown contracts.
- Reused `Slider.qml` for the deck tempo slider's interaction, with deck-specific
  background/handle overrides. Cancel, disable and destruction restore a hidden
  cursor; touchscreen drags never hide or teleport the mouse cursor.
- Consolidated Library keyboard/MIDI deck-selection actions while retaining
  distinct local/external loading and USB browsing. USB rows no longer expose
  local favorite/crate/queue swipe mutations.
- Centralized context property names and clear-value semantics in
  `ApplicationLifecycle`; Bootstrap and shutdown share the same typed keys.
- Removed historical duplicate include blocks from `LibraryPersistence.cpp`
  without merging Qt-owned and worker-owned SQL connections.
- Reused an explicit Master/Headphones/Booth role definition and audio row,
  retaining pending/Apply semantics and reporting invalid internal roles.
  Rows are resolved directly from their Repeater, without a second lifetime registry.
- Reused one header view-toggle card, preserving the original menu ordering,
  callbacks, hidden controls and popup structure.

The file merges reduce source fragmentation, not automatically callback cost,
RAM consumption or Raspberry Pi frame latency. Runtime improvements still need
measurements on the target hardware.

## Further domain-module consolidation

- Grouped the separate master and headphone bus classes in `AudioBusMixer.*`,
  with independent limiter and gain histories. Inlined the stateless
  `AudioOutputRouter` and made the cache worker private without removing its join.
- Combined MIDI accumulator/echo policy in `MidiInputState.h`, native scratch
  ingress types in `ScratchInput.h`, and jog-nudge policy with `DeckTransport.h`.
  The small `ParameterStore` stays independently owned and is now inline;
  repeated MIDI events still emit even when their values match.
- Combined feature extraction and phrase analysis in `FeatureAnalysis.*`.
  Their algorithms and the separate analysis orchestrator are unchanged.
- Grouped cover publication in `LibraryCoverService.*`, retaining QML engine
  ownership of the image provider and separate TagLib extraction.
- Grouped neutral waveform data and viewport-demand contracts in `WaveformTypes.h`;
  builder and rendering policies remain separate dependency boundaries.
- Grouped three exclusive effect DSP helpers in `FxPrimitives.h`, keeping
  their separate smoothing/filter/delay state.
- Reused tempo registration and assigned-deck dispatch inside `FxManager`,
  preserving unit/deck properties, aliases, enable semantics and BPM priority.
- Localized five exclusive QML panels in their respective waveform/FX hosts,
  without combining instances or changing their eager/lazy lifetime.
- Flattened MIDI feedback and built-in mapping directories. An explicit
  resource alias preserves the existing FLX10 mapping URL and saved identity.

This round removed 20 source files (232 -> 212). File grouping is distinct from runtime
optimization: it does not establish a CPU, RAM or latency improvement.

## Follow-up consolidation: 212 to 187 files

This follow-up removes 13 C++/QML files and 12 small domain CMake manifests.
The explicit production manifest is now `src/CMakeLists.txt`; source domains,
target dependencies and the controller mapping resource alias remain intact.

- Grouped deck identifiers with other lightweight domain values, and topology
  enums with the existing neutral audio parameter contract.
- Grouped the stateless output router with the independent master/headphone
  classes in `AudioBusMixer.*`.
- Grouped sync values, per-deck controllers and the coordinator in `DeckSync.*`,
  without merging their state or importing Qt/audio-device dependencies.
- Grouped Rekordbox source and identity readers, preserving read-only access,
  SQLCipher guards and contained-path resolution.
- Grouped startup/status/exit as independent local components in `AppOverlays`.
  Shortcuts, platter and development crossfader now live in their exclusive hosts.
- Shared repeated mixer setter dispatch, selected-FLX10 matching and AIO button
  dimensions/gating. Development CUE now follows the same idempotent release
  contract as the AIO strip, including cancellation and engine replacement.

The final production layout has 88 headers, 75 C++ sources, 21 QML files, one
CMake manifest and the unchanged controller README/XML. This is a structural
reduction, not a claimed Raspberry Pi performance improvement.

## Behavioral invariants

- The audio graph and callback constraints are unchanged.
- The local waveform streaming/ViewKey/overview/detail-tile implementation was
  moved intact and remains the only waveform engine.
- Scratch pause intent and FLX10 latest-state/progress fixes remain in the moved
  implementations.
- QML resources remain embedded under `qrc:/DJSoftware/src/qml/`.
- Submodules and repository history were not modified.

## Validation

Each consolidation batch passed `git diff --check`, `./build-fast`, and focused
freshly rebuilt tests. The final clean build and full CTest result are recorded
in the task handoff report rather than frozen into this architecture document.
