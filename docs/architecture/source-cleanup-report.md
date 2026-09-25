# BrockDJ Source Cleanup Report

Date: 2026-09-20
Baseline: `9e18081d69cf0c8037ed053239726ee147808c74`
Scope: source organization and dependency cleanup only; no intended playback,
waveform, controller, persistence or QML behavior change.

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
- Sync maintenance helper -> `deck/sync/SyncTypes.h`.
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
  `Flx10HidTransport` remain separate because each represents a real thread,
  lifecycle or hardware boundary.
- `analysis/internal/AnalysisOrchestrator` remains an internal large
  implementation unit; merging it into `WaveformAnalyzer.cpp` would create a
  God translation unit.
- `VirtualTurntable` remains independently testable simulation logic;
  `ScratchSession` remains the deck-owned state boundary.
- Large MIDI implementation units remain separate around enumeration, mapping,
  dispatch and FLX10 behavior; merging them would exceed useful file size.
- Large QML surfaces were moved but not split because binding/lifetime behavior
  requires a dedicated visual refactor.

## Follow-up consolidation

- Removed the unused controller profile and `OverallWaveform.qml`.
- Merged `UiMetrics` into `UiTheme`, desktop settings into the shared
  `SettingsPanel`, and first-run content into `StartupOverlay`.
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
