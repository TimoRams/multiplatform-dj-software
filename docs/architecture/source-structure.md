# BrockDJ Source Structure

The production source tree is organized by product domain. `src/CMakeLists.txt`
is the single entry point; each domain contributes sources to the existing
`BrockDJ` target without creating artificial static libraries.

## Dependency direction

```text
domain values
    -> analysis / waveform / audio
    -> deck
    -> controllers / library
    -> app
    -> qml
```

Cross-cutting device or operating-system code enters through `platform/`,
`controllers/`, or an explicit audio-device boundary. Core C++ never looks up
QML objects and callback code never performs file, database, controller, or Qt
scene work.

## Domains

### `app/`

- Responsibility: process bootstrap, ordered shutdown, settings, control clock,
  and QML-facing application state.
- Owns: `ApplicationRuntime`, settings persistence, application lifecycle and
  lightweight UI controllers.
- May depend on: every production domain while composing the application.
- Must not own: audio DSP, media decoding, library persistence or controller
  protocol state.
- Public entry point: `ApplicationBootstrap.h`.

### `audio/`

- Responsibility: callback-safe playback, per-deck processing, mixing, cue bus,
  device output routing and bounded decoded-page caching.
- Owns: `AudioEngine`, `DeckAudioPipeline`, `MasterMixer`, `HeadphoneBus`,
  `AudioOutputRouter`, `AudioDeviceService`, `AudioPageCache` and
  `TimeStretchProcessor`.
- May depend on: DSP libraries and simple domain values.
- Must not depend on: QML, library models, controller I/O or analysis workers.
- Callback invariant: no allocation, blocking lock, file I/O, logging or Qt
  signals. `audio/internal/` contains reusable callback DSP, not user FX.

### `deck/`

- Responsibility: the public `DjEngine` facade and one deck's transport,
  loading, cue/loop, scratch and synchronization behavior.
- Owns: `DjEngine`, `DeckTransport`, `DeckTrackLoader`,
  `DeckCueLoopController`, `ScratchController` and deck sync state.
- May depend on: audio commands, analysis/waveform values and library APIs.
- Must not own: the application audio graph or controller device transports.
- Public entry point: `DjEngine.h`; its implementation is grouped into four
  responsibility-sized translation units rather than an umbrella include.

### `analysis/`

- Responsibility: BPM, beatgrid, downbeat, key and phrase extraction.
- Owns: analysis results, validation and worker-local algorithms.
- May depend on: domain values, JUCE readers and analysis libraries.
- Must not depend on: QML or render-thread types.
- Public API: `AnalysisTypes.h`, `AnalysisValidation.h` and
  `AnalysisJobQueue.h`. Algorithm helpers live under `analysis/internal/`.

### `waveform/`

- Responsibility: waveform analysis, immutable line storage, cache/LOD data and
  Qt Quick waveform rendering.
- Owns: `WaveformAnalyzer`, `WaveformCache`, `WaveformLineStore`, LOD creation,
  tile rasterization and scrolling/overview items.
- May depend on: domain/analysis values and Qt scene-graph APIs in `render/`.
- Must not perform: analysis or file I/O on the render thread.
- Public data types are consolidated in `WaveformTypes.h`; render-only math is
  in `render/WaveformRenderMath.h`.

### `library/`

- Responsibility: track/playlist/cue persistence, library models, cover art,
  preview playback, library analysis scheduling and bounded media I/O.
- Owns: `LibraryDatabase`, `DatabaseWorker`, `MediaIoScheduler`, models and
  cover/preview services.
- May depend on: domain, analysis, waveform cache and audio preview APIs.
- Must not expose a QSQLITE connection across threads or block the audio
  callback.
- Persistence implementation is grouped in `LibraryDatabase.cpp`,
  `LibraryPersistence.cpp` and `library/persistence/DatabaseWorker.*`.

### `controllers/`

- Responsibility: controller lifecycle, MIDI mapping/feedback and FLX10 MIDI,
  display and HID transport.
- Owns: `ControllerIntegrationManager`, generic MIDI state and hardware
  protocol/transport code.
- May depend on: deck/audio control facades and settings.
- Must not own: playback or mixer DSP state. Bridges translate between existing
  APIs only.
- `controllers/midi/` is generic MIDI; `controllers/flx10/` contains every
  FLX10-specific path. HID transport remains a separate thread/hardware boundary.

### `fx/`

- Responsibility: selectable deck/master effects and their reusable DSP
  primitives.
- Owns: `FxManager`, `FxProcessor` and `fx/dsp/` primitives.
- Must not contain output-bus safety DSP; the brickwall limiter lives under
  `audio/internal/` with its actual consumers.

### `domain/`

- Responsibility: cross-domain track state and small shared values.
- Owns: `TrackData` and `DomainTypes.h`.
- Must remain independent of app and QML composition.

### `link/` and `platform/`

- `link/` owns Ableton Link session integration.
- `platform/` owns narrow OS adapters such as signal handling and Windows
  header compatibility.
- Neither directory is a dumping ground for generic helpers.

### `qml/`

- Responsibility: presentation and user interaction only.
- Organized into `components/`, `deck/`, `waveform/`, `mixer/`,
  `performance/`, `library/`, `settings/`, `shell/` and `development/`.
- All components remain in the `DJSoftware` module, so moving files among these
  folders does not create duplicate QML types or state owners.
- `main.qml` is the shell and the only root-level QML file.

## Runtime ownership summary

```text
AudioPageCache
  -> DeckAudioPipeline x4
  -> AudioEngine
  -> MasterMixer / HeadphoneBus
  -> AudioOutputRouter
  -> hardware

WaveformDemand
  -> WaveformAnalyzer / WaveformCache
  -> WaveformLineStore
  -> LOD / tile rasterizer
  -> scrolling + overview scene nodes
```

The deck, MIDI, FLX10 and QML layers are command/display facades. They do not
own parallel playback or waveform engines.
