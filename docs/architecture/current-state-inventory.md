# BrockDJ Current-State Inventory

This hand-maintained inventory describes the production source tree after the
2026-08-11 architecture consolidation. Counts exclude vendored `libs/`, build
trees and packaging output.

## Scope and metrics

| Metric | Count |
| --- | ---: |
| C++ headers | 100 |
| C++ sources | 84 |
| QML components | 34 |
| C++/QML files | 218 |
| C++/QML lines | 69,895 |
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
decoded pages; the callback never decodes or waits. The limiter is internal
audio output protection shared by master and headphone buses, not a selectable
FX unit.

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

Large QML files remain dedicated review candidates. They were not split during
this behavior-preserving source cleanup because bindings, focus, popup parents
and component lifetime require visual testing.

## Build and test shape

Use `./build-fast` for the application. Final validation must also configure a
fresh `BUILD_TESTING=ON` tree and run full CTest; existing binaries are not an
acceptable result after source moves. Manual UI/controller/audio checks remain
in `docs/testing/regression-checklist.md`.
