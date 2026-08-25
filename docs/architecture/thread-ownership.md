# Thread and Lifetime Ownership

This is the current ownership map for cross-thread state. Atomics and immutable
snapshots transfer values; they do not create a second owner.

## Application lifetime

`ApplicationRuntime` owns the services and product objects. The important
lifetime chain is:

```text
AudioDeviceService
  -> AudioPageCache
  -> AudioEngine (owns four DeckAudioPipeline instances)
  -> DjEngine facades borrow their assigned pipelines
  -> QML, controller, library, and monitoring consumers
```

Shutdown stops producers first, unregisters the device callback, resets the
auxiliary endpoint registration, closes the device, releases deck readers,
destroys decks and `AudioEngine`, then destroys the cache. General media-I/O
consumers are destroyed before `MediaIoScheduler` rejects work and joins.

`AudioPageCache` must outlive `AudioEngine`, every `DeckAudioPipeline`, and all
`AudioCacheHandle`/`AudioPageReadGuard` users. `AudioDeviceService` remains
alive until the device callback is unregistered and closed.

## Ownership table

| Component/state | Owner | Execution boundary | Lifetime rule |
| --- | --- | --- | --- |
| Audio device manager/configuration | `AudioDeviceService` | Qt/device control; may probe, allocate, and block | unregister/close before audio graph destruction |
| Device callback and deck pipelines | `AudioEngine` | JUCE callback plus control-side prepare/retire | callback removed before engine destruction |
| Master sum, limiter, meter | `MasterMixer` inside `AudioEngine` | audio callback; prepared from control boundary | no callback allocation or external owner |
| Cue/master headphone mix | `HeadphoneBus` inside `AudioEngine` | audio callback | consumes prepared deck PFL and master tap only |
| Physical channel mapping | `AudioOutputRouter` inside `AudioEngine` | audio callback reads prepared routing snapshot | device queries are control-only |
| Per-deck source chain | one `DeckAudioPipeline` | callback processes; control installs/clears | pipeline gate drains the complete callback, including command consumption, before source retirement |
| Playback policy and position | `DeckTransport` | one Qt/control writer; audio publishes atomic playhead | snapshots contain values only |
| Cue/loop state | `DeckCueLoopController` | Qt/QML/MIDI/controller | persistence and transport commands remain outside callback |
| Scratch session policy | `ScratchSession`/`ScratchController` | control-to-audio atomic commands | `RenderModeRouter` owns callback rendering state |
| Time-stretch pipelines | `TimeStretchProcessor` | preparation worker publishes; callback claims active slot | worker joined and slots destroyed outside callback |
| Stretcher seeding | `TimeStretchProcessor` seed handshake | callback publishes an output snapshot, worker performs the seek, callback consumes it | exactly one of the two owns the stretcher at a time; the callback bridges on the Direct path meanwhile |
| Channel EQ/filter/FX state | `DeckChannelProcessor` | controls publish commands/snapshots; callback consumes | filter histories remain audio-thread-only |
| Cached pages | `AudioPageCache`/`AudioCacheWorker` | worker publishes immutable pages; callback holds guards | unpublish pages before deferred worker-side free; cache outlives all guards |
| Decoder reader | cache entry plus worker lease | cache control atomically detaches; decoder worker holds a temporary `shared_ptr` and retire stack | normal handover/destruction is deferred to the worker; eject/shutdown waits for active reader calls and file closure |
| Deck facade | `DjEngine` | Qt/QML/control unless explicitly atomic | facade borrows services/pipeline; owns deck controllers/loaders |
| Sync role and actions | `DeckSyncController` | Qt/control | one controller per deck; no facade/source pointer |
| Cross-deck sync registry | `SyncCoordinator` | Qt/control | fixed deck slots unregister before coordinator shutdown |
| Periodic scheduling | `ControlClock` | Qt main thread | registrations reset before clock destruction |
| Track loading | `DeckTrackLoader` | Qt submission; one joined loader worker | reject/cancel/join before deck dependencies are cleared |
| Analysis working state | one `WaveformAnalyzer` run | analysis worker only | `AnalysisWorkingData` dies with the joined run |
| Analysis result | immutable value/mailbox | worker publishes, Qt owner drains | accept only matching identity/generations/path |
| Render-visible waveform | `TrackData`/`WaveformLineStore` snapshots | Qt owner publishes, render reads immutable handles | no worker mutation of live render containers |
| Library analysis queue | `LibraryAnalysisManager` | Qt owner schedules; analyzer worker executes | bounded queue, one active job, joined cancellation |
| Database commands/results | `DatabaseWorker` | dedicated joined DB thread | its QSQLITE connection never crosses threads |
| General file/image work | `MediaIoScheduler` | dedicated joined I/O thread | consumers destroyed before scheduler shutdown |
| POSIX signal delivery | `PosixSignalHandler` | signal handler writes pipe; Qt notifier drains | restore handlers and close descriptors during teardown |

## ControlClock contract

`ApplicationRuntime` owns one `ControlClock`. It is started after registration
and audio wiring, stopped before target destruction, and never called from the
audio callback. Its fixed-capacity registrations are non-owning; every
`ControlClock::Registration` must be reset before the clock is destroyed.

Logical ordering is scratch/fast deck work, transport, all sync inputs, one
coordinator update, all sync actions, UI/waveform, feedback/display, meters,
statistics, and housekeeping. A delayed tick performs one bounded update and
may shed non-critical UI/slow groups; it never runs an unbounded catch-up loop.

## Transport and sync

`DeckTransport` has one established Qt/control writer. QML, MIDI, controller,
cue/loop, and sync commands arrive on that boundary. The audio callback writes
only the injected playhead atomic. Adding arbitrary-thread transport mutators
would violate the current snapshot protocol and requires an explicit queue.

Track installation and clearing enter `DeckAudioPipeline`'s sequentially
consistent swap gate before mutating callback-visible source lifetime. A
callback either owns the old chain until its complete block returns or observes
the gate and writes bounded silence. The gate belongs at the pipeline entry,
not inside `RenderModeRouter`, because `consumeCommands()` also accesses the
playback source and transport.

`SyncCoordinator` stores no `DjEngine*`, and `DeckSyncController` stores no
QML, database, `TrackData`, or audio-source pointer. Commands are rejected when
master or target-track generations no longer match.

## Analysis and rendering

`WaveformAnalyzer` cancellation invalidates its generation and joins without
detaching. Worker callbacks carry values and cannot call QObjects directly.
The owner validates request identity, analyzer generation, manager generation,
and canonical path before publication or persistence.

The immutable result mailbox is latest-only. Progressive waveform publication
uses generation-checked immutable chunks; rendering consumes snapshot handles
and owns Qt Quick scene-graph objects on the render boundary.

## Database and media workers

`DatabaseWorker` owns its SQLite connection, queries, command queues, result
queue, and backup temporary files. `MediaIoScheduler` owns general file,
TagLib, image, scan, and artifact work. Both use bounded priority queues,
generation rejection, cancellation values, and deterministic join.

`LibraryDatabase` retains a Qt-owned compatibility connection for schema,
startup, and small CRUD calls. Those calls may block the UI and must never be
called from the audio callback or moved to a worker without replacing the
connection/API boundary.

## QML object lifetime

The root `PerformanceWorkspace` keeps `Library` eager because startup and FLX10
hidden-library behavior require its stable identity. Mutually exclusive heavy
surfaces use explicit lifetime gates: the selected waveform mode and selected
deck rows use synchronous `Loader`s, while AIO settings and Source load
asynchronously. The standalone settings and mapping-editor windows are dynamic
objects destroyed with their owning QML surface. Code must not retain an item
pointer across one of these loader boundaries. Production deck rows do not
instantiate the hidden visual `MixerSection`; mixer parameter ownership remains
in `MixerControl`, while the visible development mixer creates that component
only inside its own window.
