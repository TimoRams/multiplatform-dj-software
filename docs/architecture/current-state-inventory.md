# BrockDJ Current-State Inventory

This hand-maintained inventory describes the production source tree after the
2026-08-24 performance and stability audit. Counts exclude vendored `libs/`,
build trees and packaging output.

## Scope and metrics

| Metric | Count |
| --- | ---: |
| C++ headers | 113 |
| C++ sources | 93 |
| QML components | 35 |
| C++/QML files | 241 |
| C++/QML lines | 81,640 |
| Top-level source domains | 12 |

Counts are orientation points, not a target. Ownership and dependency direction
are the architectural contract.

## Source domains

| Directory | Responsibility | Main owner or boundary |
| --- | --- | --- |
| `src/analysis` | tempo, beat, downbeat, key and phrase analysis | worker-local state -> immutable results |
| `src/app` | startup, lifetime, settings and QML-facing application state | Qt application thread |
| `src/audio` | callback, deck pipelines, buses, routing, device and page cache | audio callback plus bounded decoder worker |
| `src/controllers` | generic MIDI, feedback and FLX10 MIDI/HID/display | device/control threads |
| `src/deck` | `DjEngine`, transport, loading, cues, scratch and sync | Qt/control -> audio commands |
| `src/domain` | cross-domain track state and value types | immutable snapshots where possible |
| `src/fx` | selectable audio effects | prepared state -> audio callback |
| `src/library` | persistence, models, cover/preview, analysis and media I/O | Qt owner plus joined workers |
| `src/link` | Ableton Link integration | Link session -> control clock |
| `src/platform` | narrow OS adapters | platform boundary |
| `src/qml` | presentation organized by UI domain | Qt Quick scene/render thread |
| `src/waveform` | waveform extraction, store, cache, LOD and renderers | workers -> immutable store -> scene graph |

`src/CMakeLists.txt` is the production-source entry point. Domain CMake files
only contribute sources/resources to the existing application target.

## Audio ownership

```text
AudioPageCache -> DeckAudioPipeline x4 -> AudioEngine
                                     -> MasterMixer
                                     -> HeadphoneBus
                                     -> AudioOutputRouter -> hardware
```

The protected audio owners remain distinct. The page cache publishes immutable
decoded pages; the callback never decodes or waits. A pipeline-owned callback
gate protects the complete source chain during track replacement. Decoder
readers use shared worker leases, so a normal replacement does not wait for an
in-flight codec or device read; explicit eject/shutdown still waits for the
backing handle to close. The limiter is internal audio output protection shared
by master and headphone buses, not a selectable FX unit.

Scratch remains one mode inside `RenderModeRouter`, not a parallel deck graph.
FLX10 cumulative motion reaches the callback through a coherent native
snapshot; `ScratchResampler` owns its C2 trajectory, bounded position servo,
preallocated source window and band-limited bidirectional read. Its living
architecture and measured gates are in
`docs/architecture/scratch-engine-quality.md`.

## Deck facade

`src/deck/DjEngine.h` remains the stable QML/controller facade. Its methods are
implemented in four responsibility-sized files:

| File | Responsibility |
| --- | --- |
| `DjEngine.cpp` | lifecycle, settings, diagnostics and basic facade state |
| `DjEngineTransport.cpp` | transport, track load and synchronization actions |
| `DjEngineCues.cpp` | cue, loop, beat-jump and persistence bridge |
| `DjEnginePerformance.cpp` | scratch and FX performance controls |

Direct includes replaced the former umbrella header. `DeckTransport`,
`DeckTrackLoader`, `DeckCueLoopController`, scratch and sync remain separate
because they have real ownership or test boundaries.

## Analysis and waveform

Public analysis values live in `AnalysisTypes.h`; beat implementation helpers
live in `analysis/internal/BeatAnalysis.*`. `AnalysisJobQueue` remains separate
because it owns scheduling state.

Waveform data and rendering now form one domain:

```text
WaveformAnalyzer / WaveformCache
    -> WaveformLineStore + WaveformTypes
    -> WaveformLodPyramid
    -> render/WaveformTileRasterizer
    -> render/ScrollingWaveformItem + OverviewWaveformItem
```

The current playhead-demand, immutable chunk, ViewKey, single overview and
detail-tile behavior is preserved. Analysis/orchestration lives under
`waveform/internal/`; scene-graph code lives only under `waveform/render/`.

## Library and persistence

`LibraryDatabase.cpp` owns setup/lifecycle; `LibraryPersistence.cpp` groups the
track, cue and playlist CRUD implementation. `DatabaseWorker` retains a joined
SQLite thread under `library/persistence/`. `MediaIoScheduler` remains a
separate joined worker but belongs to the library/media domain instead of an
otherwise empty top-level folder.

## Controllers

Generic MIDI lives in `controllers/midi/`; all FLX10-specific behavior lives in
`controllers/flx10/`. FLX10 HID transport remains separate from display
encoding and MIDI dispatch. Controller bridges translate between existing state
owners and do not own a second mixer or transport state.

## QML shape

`main.qml` is the only root component. Reusable and product surfaces are grouped
under `components`, `deck`, `waveform`, `mixer`, `performance`, `library`,
`settings`, `shell` and `development`. The files remain in one `DJSoftware`
module, preserving unqualified component use and resource URLs.

Heavy mutually exclusive QML surfaces are lifetime-gated. Desktop settings and
the mapping editor are created on first use; AIO settings and Source use
asynchronous loaders; two-/four-deck waveform surfaces and the C/D control row
exist only in their selected mode. `Library.qml` remains eager because startup
hydration and hidden-controller routing currently use its stable object identity.

Large QML files remain dedicated review candidates. They are not split solely
to reduce line count because bindings, focus, popup parents and component
lifetime require visual parity tests. `SettingsPanel.qml` and
`SettingsWindow.qml` are still near-parallel and are tracked in the risk
register rather than being merged without those tests.

## Build and test shape

Use `./build-fast` for the application. Final validation must also configure a
fresh `BUILD_TESTING=ON` tree and run full CTest; existing binaries are not an
acceptable result after source moves. Manual UI/controller/audio checks remain
in `docs/testing/regression-checklist.md`.
