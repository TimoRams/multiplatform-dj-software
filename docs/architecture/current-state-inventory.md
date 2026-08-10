# BrockDJ Current-State Inventory

This document is a reviewed description of the production tree as of 2026-08-10. It intentionally
contains only stable architectural facts and a small set of useful size indicators. It is maintained
by hand alongside architectural changes; generated file classifications were removed because their
domain labels and reference lists became stale immediately after refactors.

## Scope and Metrics

The counts below exclude vendored code in `libs/`, generated build trees and packaging output.

| Metric | Current count |
| --- | ---: |
| Production C++ headers (`.h`, `.hpp`) | 107 |
| Production C/C++ sources (`.c`, `.cpp`) | 92 |
| QML components | 34 |
| Production source and QML files | 233 |
| Production source and QML lines | 65,326 |
| Test source/header files | 40 |
| `engine/facade` implementation files | 8 |
| CTest cases on Linux | 39 |

These numbers are orientation points, not an architectural contract. Source ownership and runtime
boundaries below are the parts that should stay current.

## Source Domains

| Directory | Responsibility | Main owner or boundary |
| --- | --- | --- |
| `src/analysis` | tempo, beat, downbeat, phrase and waveform analysis | worker-local analysis state; immutable result publication |
| `src/app` | startup, runtime lifetime, settings and QML-facing application services | Qt application thread and `ApplicationRuntime` |
| `src/audio` | callback, deck pipelines, mixing, output routing and streaming cache | `AudioEngine`; audio callback plus bounded workers |
| `src/controllers` | controller integration and FLX10 HID protocol | Qt/control thread plus device I/O boundary |
| `src/database` | serialized database work | `DatabaseWorker` thread |
| `src/domain` | cross-domain track and analysis value types | immutable snapshots where data crosses threads |
| `src/engine` | stable `DjEngine` QML facade and deck/scratch/sync control logic | Qt/control thread; commands and snapshots cross to audio |
| `src/fx` | deck/master effects and limiter | audio callback; prepared on control thread |
| `src/io` | bounded media I/O scheduling | `MediaIoScheduler` workers |
| `src/library` | database facade, models, analysis scheduling, preview and covers | Qt models, database worker and media workers |
| `src/link` | Ableton Link integration | Link session bridged to control-clock state |
| `src/midi` | MIDI enumeration, mapping, feedback and FLX10 bridge | MIDI callbacks normalized onto control state |
| `src/platform` | operating-system adapters | narrow platform boundary |
| `src/qml` | desktop/AIO composition and reusable visual components | Qt Quick scene and render thread |
| `src/rendering` | waveform analysis orchestration and Qt Quick render items | analysis workers and render-thread snapshots |
| `src/waveform` | immutable progressive waveform line data | producer/consumer snapshot boundary |

## Runtime Ownership

`ApplicationRuntime` owns the long-lived services and records their destruction order. The important
audio path is:

```text
AudioPageCache workers
        |
        v
DeckAudioPipeline x4
        |
        v
AudioEngine callback
   |             |
   v             v
MasterMixer   HeadphoneBus
       \       /
        v     v
   AudioOutputRouter -> hardware outputs
```

`AudioEngine` is the sole application audio source registered with the JUCE device manager. It owns
four `DeckAudioPipeline` instances, the program and headphone buses, the output router and the
optional auxiliary endpoint used by library preview. Each deck pipeline owns cached playback,
render-mode routing, time stretch and its `DeckChannelProcessor`. Callback code consumes prepared
state and bounded snapshots; disk I/O, decoding and object preparation stay off the callback.

`ControlClock` drives periodic control-domain work. `SyncCoordinator` and the deck sync controllers
publish control decisions without becoming audio callback owners. The exact thread rules and shutdown
ordering are documented in [realtime-safety.md](realtime-safety.md) and
[thread-ownership.md](thread-ownership.md).

## DjEngine Facade

`src/engine/DjEngine.h` is the stable Qt/QML compatibility surface. Its implementation is grouped by
responsibility under `src/engine/facade/`:

| File | Responsibility |
| --- | --- |
| `Core.cpp` | lifecycle, clock, mixer, library hydration, tempo and keylock facade |
| `CueLoop.cpp` | cue/loop persistence and `DeckCueLoopController` bridge |
| `Diagnostics.cpp` | latency composition and diagnostic views |
| `Fx.cpp` | deck FX commands |
| `Scratch.cpp` | scratch session and deck-pipeline handoff |
| `Settings.cpp` | audio-device settings and queries |
| `Sync.cpp` | sync snapshots and actions |
| `Transport.cpp` | transport, track loading, reverse/slip and PFL facade |

The public contract is protected by `dj_engine_api_contract`. The facade monolith is a separate
refactoring concern and is not part of repository cleanup.

## Waveform and Analysis

`WaveformAnalyzer` manages analysis worker lifetime and publishes immutable `AnalysisResult` values;
`AnalysisWorkingData` remains worker-local. `WaveformEnvelopePass` is the expensive waveform pass,
while `WaveformAnalysisOrchestrator` coordinates cache and preview integration. Independently tested
algorithms such as `TempoEstimator`, `BeatTracker`, `DownbeatDetector`, `BeatGridFitter` and
`PhraseAnalyzer` remain separate boundaries.

`TrackData` currently exposes full RGB frames, overview RGB frames and immutable progressive
`WaveformLineStore` chunks. This duplication is intentional while cached analysis, Qt rendering and
FLX10 projection consume different representations. Removing an RGB representation requires a format
and consumer migration; it is not safe file cleanup.

## QML Shape

`main.qml` composes `PerformanceWorkspace`, settings/development windows and the selected desktop or
AIO presentation. `DeckControl` is shared across four decks. `EnlargedWaveform` is shared by desktop
and performance waveform screens. The two UI modes should share behavior and differ only in
composition; mode-specific application logic is a regression risk.

The largest QML files are review candidates, not deletion candidates: `Library.qml` (6,213 lines),
`SettingsPanel.qml` (1,756), `SettingsWindow.qml` (1,693), `TopHeader.qml` (1,564) and
`DeckControl.qml` (1,314). Component extraction needs a dedicated QML binding and lifetime review.

## Large Implementation Files

The principal non-QML split candidates are `FxProcessor.cpp` (1,834 lines),
`MidiFlx10Bridge.cpp` (1,734), `WaveformEnvelopePass.cpp` (1,163), `CueLoop.cpp` (1,122),
`RenderModeRouter.cpp` (944), `MidiDeviceEnumeration.cpp` (936) and `Core.cpp` (844). File size alone
does not authorize a split: real-time ownership, anonymous implementation state and test boundaries
must be preserved.

## Build and Test Shape

The root CMake project currently declares the application and 39 Linux CTest cases. Several test
executables compile overlapping production and JUCE sources, which makes clean test builds expensive.
Consolidating test support may help, but combining tests into fewer executables should only happen
after measuring startup/build time and checking global-state isolation.

The fast regression entry point is `./test-fast`; focused subsets can be run through CTest in
`build-tests`. Manual cross-mode checks live in
[`docs/testing/regression-checklist.md`](../testing/regression-checklist.md).

## Known Follow-Up Work

- Modularize the root `CMakeLists.txt` without changing target membership or QML resource paths.
- Review repeated QML composition in library, settings, header and deck-control surfaces.
- Keep the `DjEngine` facade split and the waveform representation migration as separate refactors.
- Treat `libs/` as externally owned submodules; repository cleanup must not alter them.
