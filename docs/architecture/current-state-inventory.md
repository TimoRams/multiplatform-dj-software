# BrockDJ Current-State Inventory

Generated against `main` at `a1ab1215152b91bad21f322f90b4508682870851` on 2026-08-03. The
working tree was clean before this inventory. This report describes the current code; it does not
propose a parallel runtime architecture.

## Scope and Metrics

| Metric | Count |
| --- | ---: |
| Repository files, excluding `.git` and `build` | 6,582 |
| Production C++ headers | 98 |
| Production C/C++ sources | 89 |
| QML components | 36 |
| Test source files | 32 |
| CMake files, including vendored projects | 53 |
| Project scripts | 8 |
| Production source/QML files classified | 223 |
| `engine/facade` implementation files | 8 |
| CMake executable test targets | 30 |
| CMake targets total | 31 |
| CTest cases | 32 |
| Files that mention `std::thread`, `std::jthread`, or `QThread` | 17 |
| Files under 40 / 80 / 120 lines | 44 / 83 / 105 |

`scripts/architecture_inventory.py` regenerates
[`file-classification.csv`](file-classification.csv) from the current `src` tree. The CSV contains
one row per C++ header/source and QML file, a conservative classification, a suggested merge target
where evidence is already strong, and same-name source references. It is an inventory aid, not a
substitute for include-graph analysis.

## Domain Map

| Domain | Files | Lines | Main state owner / thread boundary |
| --- | ---: | ---: | --- |
| Application | 34 | 6,844 | `ApplicationBootstrap`/`ApplicationRuntime` plus the Qt/QML deck facade; Qt owner thread |
| Settings | 2 | 734 | `SettingsManager`; Qt owner thread |
| Audio Device | 4 | 1,290 | `AudioDeviceService`; control thread, JUCE manager callback boundary |
| Audio Cache / Streaming | 8 | 694 | `AudioPageCache`; worker publishes immutable pages to audio readers |
| Deck Playback | 13 | 2,199 | `DeckTransport` and `DeckAudioGraph`; control plus audio callback |
| Scratch | 10 | 1,557 | `ScratchController`/`ScratchDeckBridge`; control-to-audio atomics |
| Time-Stretch | 2 | 445 | `TimeStretchAudioSource`; preparation worker to audio callback |
| Deck DSP / Mixer | 4 | 851 | `MixerDspSource`; control-to-audio snapshots |
| Effects | 9 | 3,522 | `FxProcessor` per graph plus Qt `FxManager` routing |
| Master Mixing | 3 | 838 | `DjMasterBus`; sole audio device callback owner |
| Sync | 8 | 832 | `SyncCoordinator`/`DeckSyncController`; control clock |
| Analysis | 18 | 1,854 | worker-local `AnalysisWorkingData`, immutable result mailbox |
| Waveform | 16 | 4,412 | `TrackData` plus immutable `WaveformLineStore` snapshots |
| Library | 20 | 5,521 | `LibraryDatabase` and models; Qt/DB worker handoff |
| Database | 2 | 673 | `DatabaseWorker`; dedicated worker thread |
| Media I/O | 2 | 695 | `MediaIoScheduler`; bounded worker queue |
| Controller / MIDI / HID | 25 | 7,036 | MIDI manager and FLX10 transport; Qt/control plus HID writer |
| QML UI | 36 | 20,054 | QML scene; render thread only through Qt Quick items |
| Platform | 3 | 281 | platform adapter boundaries |
| Unknown / mixed | 4 | 1,400 | `TrackData` and cross-domain value types |

The largest sources are `Library.qml` (6,213), `FxProcessor.cpp` (1,778), `SettingsPanel.qml`
(1,725), `SettingsWindow.qml` (1,662), `TopHeader.qml` (1,548), `MidiFlx10Bridge.cpp`
(1,326), `DeckControl.qml` (1,301), `WaveformEnvelopePass.cpp` (1,163), and
`facade/CueLoop.cpp` (1,122). Small files are not automatically defects: the cache handle,
master endpoint interfaces, waveform line values, and platform adapters are legitimate boundaries.

## DjEngine Facade Inventory

`DjEngine` is a Qt/QML compatibility facade around already-extracted runtime owners. Its stable public
header remains at `src/engine/DjEngine.h`; implementation is grouped under `src/engine/facade/`.
It exposes properties/invokables for playback, metadata, cue/loop, audio settings, mixer, FX,
scratch, sync, meters and analysis. That public contract is protected by
`BrockDJ_dj_engine_api_contract_tests`; it must remain stable through file consolidation.

| File | Lines | Actual responsibility | Recommendation |
| --- | ---: | --- | --- |
| `facade/Core.cpp` | 838 | lifecycle, clock, mixer, library hydration, tempo/keylock facade | KEEP as facade core |
| `facade/CueLoop.cpp` | 1,122 | cue/loop persistence and bridge to `DeckCueLoopController` | KEEP; it has real responsibility |
| `facade/Fx.cpp` | 217 | deck FX commands to graph mixer | KEEP until graph command surface exists |
| `facade/Scratch.cpp` | 361 | scratch session and audio-graph handoff | KEEP; real timing domain |
| `facade/Settings.cpp` | 50 | audio-device settings and query facade | KEEP as one concise facade file |
| `facade/Sync.cpp` | 240 | extracted sync owner snapshot/action facade | KEEP; control-clock boundary |
| `facade/Transport.cpp` | 567 | QML transport, track loading, reverse/slip, PFL, and graph bridge | KEEP as one transport facade file |
| `facade/Diagnostics.cpp` | 211 | latency composition and diagnostic view | keep separate from transport commands |

The first consolidation pass is complete: six small facade/query companions were merged and
deleted. The remaining facade is grouped by responsibility in one folder; device/limiter/keylock
latency remains an explicit diagnostics view because it crosses those three domains.

## Waveform and Analysis Inventory

There are three simultaneously maintained waveform representations in `TrackData`:

1. Full RGB frame vector/snapshot, used for cached/full analysis and FLX10 conversion.
2. Overview RGB vector/snapshot plus progressive overview fallback, used by `RgbWaveformItem`.
3. Immutable 1,200-lines-per-second `WaveformLineStore` chunks, used by
   `ScrollingWaveformItem` and also available to the compact renderer.

This is intentional during progressive analysis but remains a duplication cost. The long-term
canonical model should be `WaveformLineStore`; compact overview should be a derived downsample and
the HID projection should be built from a snapshot rather than mutable fallback vectors. Do not remove
the RGB data before the HID and cache formats have a replacement.

`WaveformAnalyzer` owns worker lifecycle and produces immutable `AnalysisResult` objects.
`AnalysisWorkingData` is deliberately worker-local. `WaveformEnvelopePass` is the expensive waveform
algorithm; `WaveformAnalysisOrchestrator` coordinates cache/preview integration. Keep independently
tested algorithms (`TempoEstimator`, `BeatTracker`, `DownbeatDetector`, `BeatGridFitter`,
`PhraseAnalyzer`) separate. Investigate folding the thin orchestrator and internal pass helpers into
the analyzer implementation, but preserve the result mailbox boundary.

## QML Inventory

The product entry is `main.qml -> PerformanceWorkspace`. `DeckControl` is instantiated for all four
decks in `PerformanceWorkspace` and again in `DevelopmentControlsWindow`; it embeds
`DeckTrackInfoPanel`. `EnlargedWaveform` is used by both desktop workspace and
`PerformanceWaveformScreen`; it embeds `BeatgridEditorPanel`. `FxBar` embeds two `FxUnit` instances.

| Component | Evidence | Classification |
| --- | --- | --- |
| `TrackInfoDisplay.qml` | no production QML instantiation found; module entry removed | Deleted after dynamic-loader check |
| `AioWaveformInfoSlot.qml` | two literal-hidden placeholder instances and its module entry removed | Deleted |
| `DevelopmentControlsWindow.qml` | instantiated from `main.qml`, visibility is a user setting | Debug/development KEEP |
| `SettingsWindow.qml` and `SettingsPanel.qml` | two large near-parallel settings surfaces | INVESTIGATE |
| `PerformanceBeatgridPanel.qml` and `BeatgridEditorPanel.qml` | two surfaces for the same domain | INVESTIGATE |
| `PerformanceBeatFxPanel.qml`, `FxBar.qml`, `FxUnit.qml` | mode-specific composition over shared FX state | KEEP temporarily |

`visible: false` is not destruction. The two AIO waveform slots, hidden mixer sections, and hidden
legacy layout objects can still retain bindings, `Connections`, and timers.

## Build and Test Shape

The 30 standalone test executables repeat compile/link setup across UI, waveform, audio cache,
transport, analysis, library and platform tests. The focused tests protect real-time boundaries well.
Do not bundle them before extracting shared test support and measuring build time. A reasonable end
state is six domain executables (`audio`, `analysis`, `waveform`, `library`, `controller`,
`ui/platform`) rather than one monolith.

## Initial Findings

- The `engine` subtree is grouped by `facade`, `deck`, `scratch`, `sync`, and callback-only `dsp`.
  The public `DjEngine` API remains stable without leaving implementation files in the engine root.
- Six query/facade companion files have been removed without changing the public API.
- The largest simplification opportunities by line count are QML settings/library surfaces and the
  FX implementation. Both need separate behavioral inventories, not an engine cleanup patch.
