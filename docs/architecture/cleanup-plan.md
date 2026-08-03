# Cleanup Plan

This is deliberately a small-step plan. It does not create another playback engine, master mixer,
waveform engine, or QML shell.

## Prioritized Merge List

| Priority | Current files | Target | Reason | Risk |
| --- | --- | --- | --- | --- |
| Done | settings, transport-extras, core-meta, core-tempo, audio-device query, and track-load facades | their owning implementation files | six source files deleted in the first consolidation pass | validated |
| Done | root-level `DjEngine_*.cpp` files | `src/engine/facade/` by responsibility | stable public header retained; transport loading merged with transport | validated |
| 2 | duplicated `SettingsWindow.qml` / `SettingsPanel.qml` sections | shared settings content | largest clear QML duplication | high |

## Move List

| Current location | Proposed location | Reason | Prerequisite |
| --- | --- | --- | --- |
| Done: `MetadataUtils.*` | `src/engine/deck/` | metadata parsing now lives with `DeckTrackLoader` | build and track-loader tests pass |
| Done: `AudioDeviceUtils.*` | `src/audio/device/` | device probing/routing now lives beside `AudioDeviceService` | build and device-service tests pass |
| `src/rendering/WaveformCache.*` | `src/waveform/` | cache is waveform data, not renderer | preserve includes during one move |

## Delete List

`TrackInfoDisplay.qml` had no source or QML instantiation beyond QML-module registration and was removed.
Dynamic QML loading was checked before deletion; the QML module list, build, and focused UI tests pass.

`AioWaveformInfoSlot.qml` was instantiated twice with literal `visible: false`; its two instances,
module entry, and placeholder file were removed. `DevelopmentControlsWindow.qml` is reachable
through a user setting and is not dead code.

Keep `EnlargedWaveform`, `OverallWaveform`, `RgbWaveformItem`, and `ScrollingWaveformItem` until
their distinct data contracts and UI placements converge. Keep `FxUnit`/`FxBar` while the performance
beat-FX surface remains a separate interaction mode.

## Simple Target Tree

```text
src/
  app/                 application, settings, UI facades
  audio/
    device/            device service and device probing
    cache/             page cache and decoder worker
  engine/
    facade/            Qt/QML `DjEngine` implementation by responsibility
    deck/              transport, loader, cue/loop, graph
    scratch/            scratch domain state
    sync/               deck and global sync
    dsp/                callback-only DSP algorithms
  waveform/             waveform data, cache, renderer-facing stores
  analysis/             tested analysis algorithms and result values
  rendering/            Qt Quick renderer implementations only
  library/, database/, io/, controllers/, midi/, fx/, qml/, platform/
```

This keeps the current one- or two-level structure. It removes misplaced helpers but does not turn
every concept into a new folder.

## Safe Refactor Sequence

| Phase | Scope | Do not touch | Tests | Expected reduction | Risk |
| --- | --- | --- | --- | ---: | --- |
| Done | Merge six facade/query companions | public QML names | API, device, smoke, transport tests | 6 files | validated |
| Done | Move device/metadata helpers and group engine facade/DSP | behavior and platform probes | full build and focused integration tests | 0 files | validated |
| 2 | Remove proven dead QML | active deck/workspace layout | QML plus desktop/AIO regression | 1-2 files | medium |
| 3 | Unify shared settings QML pieces | settings behavior | QML visual/manual regression | material lines | high |
| 4 | Canonical waveform migration | analysis mailbox and HID output | waveform/analysis/FLX10 tests | later | high |

Rollback boundary: every phase is one coherent merge or move with no behavior change. Do not combine
file consolidation with audio-routing, QML redesign, or ownership extraction.

## Measurable Goals

- Keep the eight responsibility-named `engine/facade` files behind the unchanged public header.
- Keep `engine/{facade,deck,scratch,sync,dsp}`; they reflect real domains.
- Reduce active waveform render data to one canonical line snapshot plus explicitly derived compact
  and HID/cache projections.
- Reduce duplicated settings UI through shared content, not a second settings model.
- Bundle test executables only after shared test support proves an incremental-build benefit.
