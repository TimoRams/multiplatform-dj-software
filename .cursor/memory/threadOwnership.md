# Thread Ownership

Last updated: 2026-07-13

| Object/state | Owner/lifetime | Allowed callers | Blocking/allocation | Audio-callback rule |
|---|---|---|---|---|
| `AudioDeviceService` | `ApplicationRuntime`; created before decks, destroyed after decks/master bus | Qt application/control thread | enumeration, device open/close, JACK probing and configuration may block/allocate | never call service configuration/enumeration from callback |
| `AudioDeviceService::ConfigurationSnapshot` | service, Qt owner thread | service apply/device-notification path; deck/QML readers on Qt thread | trivial | callback does not read it |
| `DjMasterBus` routing/master atomics | master bus/static atomics; callback unregistered before destruction | service/bootstrap publish on Qt thread; audio callback reads | publishing is trivial atomic work | callback read-only, no device query |
| `DjEngine` facade | runtime, one per deck; destroyed before service | QML/MIDI/control thread unless method says atomic/audio-only | transport/device changes outside callback | `getAudioSource`/PFL and DSP processing follow explicit audio path |
| `DeckAudioGraph` and its cached playback, transport, scratch, time-stretch, mixer/FX chain | uniquely owned by each `DjEngine`; `AudioPageCache` outlives it | prepare/release/install/clear and controls on control/device boundary; `getNextAudioBlock` on callback | prepare/install/clear may allocate or destroy outside RT; processing must not block/allocate | realtime rules apply strictly; no Qt or `DjEngine` backreference |
| Device enumeration/query helpers | service/control path | Qt owner thread only | may scan hardware, spawn probe manager, lock caches | forbidden |
| `DeckTrackLoader` | value member of each deck; worker joined before deck dependencies are destroyed | submission/cancel on Qt owner; private worker performs file/decoder/metadata/cover/cache work; completion queued to owner | worker may block/allocate; state/generation are atomic and queue mutex is never used by audio callback | all loader APIs and result application forbidden from audio callback |
| `WaveformAnalyzer` | remains deck-owned during this transition | started/stopped and result persisted on owner thread; analysis on its joined worker | decode/analysis may block on worker; shutdown join may wait for bounded key analysis | forbidden |
| prepared `TrackLoadResult` | loader worker until move into queued owner callback | created on loader worker, applied exactly once on Qt owner after generation/path check | owns readers and potentially large waveform/cover payload | never visible to callback until existing source handover |
| Sync global registry | currently static `DjEngine`; future `SyncCoordinator` | Qt 4 ms control/UI with `s_syncMutex` | short mutex only outside callback | forbidden from callback |
| `DeckCueLoopController` | one value member per `DjEngine` | Qt owner/control thread: QML, MIDI and hardware dispatch | QString/beat-grid snapshot work may allocate; persistence remains in facade | never call controller mutation/QString/containers from callback; audio reads only already-applied loop state in dedicated sources |
| cue/loop persistence | `LibraryDatabase`, invoked by `DjEngine_CueLoopFacade` | Qt owner/control thread | SQLite may block | forbidden |

Shutdown order: stop new UI changes → each deck rejects and joins `DeckTrackLoader` → unregister master callback → close service device → join analyzer → release deck readers → destroy decks → destroy master bus → destroy service. Queued loader results carry a `QPointer` and mismatching generation, so they cannot apply after loader shutdown. Qt service connections disconnect automatically when each deck is destroyed.

## Audio page cache foundation

| Component | Owner/access | RT rule |
|---|---|---|
| `AudioPageCache` | one `ApplicationRuntime`; control open/release, RT read/request | only lookup/request/stats are RT-safe |
| `AudioCacheWorker` | cache-owned joined worker | decoder/allocation/eviction/delete allowed |
| immutable `AudioPage` | worker publish, guarded RT read | never mutate after publication |
| bounded priority queues | multi-producer, single consumer | fixed capacity, no allocation or wait |
| `AudioPageReadGuard` | reader stack | atomic counter only; prevents worker free |

Stop audio readers and destroy guards before joining/destroying the cache.

Scratch lifetime: `ApplicationRuntime::audioPageCache` outlives all decks and their `ScratchDeckBridge`/`ScratchResampler`. Track apply swaps the handle under the existing transport-swap gate; eject invalidates the resampler before releasing the handle. The audio callback only copies from guarded pages into the already allocated half-second scratch window. No control mutex, reader or worker API is reachable from scratch processing.

## DeckAudioGraph public-method contract

| Method | Allowed thread | Allocation/blocking | Audio state |
|---|---|---|---|
| constructor/destructor | application/control, callback stopped for destruction | allocation/destruction allowed | builds/destroys complete chain |
| `prepareToPlay` | device/control boundary | allocation and bounded worker preparation allowed | prepares mixer, time stretch, scratch and transport chain |
| `releaseResources` | control after callback stop | release/join work allowed | releases complete chain |
| `installPreparedTrack` | Qt/control handover, never callback | allocates cached source; detaches/destroys old source; no file decode | closes swap gate and atomically changes the audible source boundary |
| `clearTrack` | Qt/control handover, never callback | destroys source/releases handle | invalidates generation and leaves prepared empty graph |
| `getNextAudioBlock` | JUCE audio callback only | no allocation, blocking, object destruction, Qt, disk or decoder | delegates to stable mixer endpoint |
| narrow transport commands/snapshot | `DeckTransport` on existing Qt/MIDI control path | atomic publication plus existing graph handover behavior | domain ownership extracted; concrete audio objects remain graph-owned |
| `realtimeStats` | diagnostics/control after or outside callback | no blocking | aggregates current/retired playback plus scratch/stretch/mixer violations |

Normal deck playback now has the same lifetime boundary. `CachedPlaybackAudioSource` is deck-owned, holds an `AudioCacheHandle`, and is installed into `AudioTransportSource` on the Qt/control thread. The callback only holds `AudioPageReadGuard`s and submits bounded page requests; misses advance the track timeline while a 128-sample fade moves to/from silence. `AudioTransportSource` remains responsible for track-rate to device-rate conversion. Track replacement/eject disconnects the transport before destroying the cached source and releases the handle outside the callback. The former `AudioFormatReaderSource`/`BufferingAudioSource` and deck read-ahead thread no longer exist.

Time stretch lifetime: each deck-owned `TimeStretchAudioSource` owns two `Pipeline` slots and one joinable worker. `prepareToPlay` synchronously prepares the initial slot before playback; the worker exclusively constructs/configures/prewarms inactive slots and publishes `Ready`. The audio callback processes only `Active`, validates track/config generations, atomically switches at a block boundary and marks the former slot `Empty` afterward. Worker mutex/CV access and all object destruction are forbidden to the callback. Shutdown rejects configurations, invalidates work, wakes and joins the worker, then releases RubberBand/buffers outside audio processing.

Mixer filters: QML/MIDI/control setters own clamped targets and serialize the four related values with a control-only mutex. They build complete trivial snapshots tagged with parameter and device generations into two fixed slots. The audio callback never takes that mutex; it atomically claims `Ready`, rejects stale sample-rate generations, configures the inactive preallocated biquad bank, and crossfades for 128 samples. Filter histories are audio-thread-only. Device `prepareToPlay` publishes a new device generation before playback resumes.

## DeckTransport contract (2026-07-13)

`DjEngine` owns one ordinary `DeckTransport`, which borrows the deck-owned `DeckAudioGraph`. QML, MIDI, controller, cue/loop and sync calls remain on the Qt/control path and issue domain commands to `DeckTransport`. It owns play intent, audible/held/background positions, rate, reverse, slip, pre-roll/end state and track/state generations. The 4-ms owner-thread timer calls `updateControlState()`; it no longer implements transport transitions itself. The audio callback writes only the injected atomic playhead sink. Readers use the pointer-free atomic sequence snapshot and never dereference graph/source objects. Install/clear and JUCE transport commands remain control-boundary operations and may not run in the callback.

## Sync ownership after extraction (2026-07-13)

| Operation | Owner | Thread / boundary |
|---|---|---|
| Track/transport/beat snapshot collection | `DjEngine` facade | Qt/control, current 4-ms timer |
| Enabled/role, PI phase state and own-deck pending action | `DeckSyncController` | Qt/control only |
| Registration, enable order, master/generations and Link snapshot | `SyncCoordinator` | Qt/control only |
| Tempo/seek application | owning facade through its own `DeckTransport` | Qt/control to RT-safe graph boundary |
| Audio blocks | `DeckAudioGraph` and sources | audio callback; no sync coordination |

Coordinator construction precedes deck construction. Each controller explicitly registers in a
fixed slot, unregisters during deck destruction, and is gone before coordinator shutdown. The
Coordinator stores no `DjEngine*`; controllers store no engine/TrackData/QML/DB/audio-source pointer.

## ControlClock ownership and timer audit (2026-07-13)

`ApplicationRuntime` owns exactly one Qt-main-thread `ControlClock`, constructed before Link,
monitoring, decks and controller feedback. It is started only after all registrations and audio
wiring are complete. Shutdown stops it first, then unregisters/destroys targets, then destroys the
coordinator and clock. Communication with the callback remains existing atomics, snapshots and
bounded graph commands; the clock never runs in or schedules the audio thread.

| Task | Before | Owner/thread | Actual work | Now / required rate |
|---|---:|---|---|---:|
| Deck fast/scratch | 4 × 250 Hz precise | each `DjEngine`, Qt main | scratch physics/jog decay | shared base, 250 Hz |
| Transport snapshot | included in 4 × 250 Hz | each deck, Qt main | graph snapshot, EOF/slip/history | 125 Hz |
| Sync | included in 4 × 250 Hz | each deck, Qt main | input/master/commands | 125 Hz, coordinator once |
| Waveform/QML position | included in deck timer + 66 ms paused QML | deck/QML main | coalesced progress/repaint | 60 Hz / paused 15 Hz |
| MIDI VU/blink | 33/500 ms | feedback, Qt main | deduplicated VU and blink | 30 Hz / divided 2 Hz |
| FLX10 state/wave/keepalive | 5/50/500 ms precise | controller, Qt main | dynamic display/trickle/session | 60/20/2 Hz |
| Link state/publish | 16/50 ms | Link/QML main | snapshot/publish | 60/20 Hz |
| Meter | included in deck timer | deck, Qt main | atomic level publication | 30 Hz |
| Preview position | 80 ms | preview, Qt main | transport property snapshot | 10 Hz while active |
| System monitor | 500 ms | monitor, Qt main | `/proc` status | 2 Hz |
| DB mirror/backup | 3000/1500 ms | database, Qt/DB boundary | integrity and file backup | remains separate |

Single-shot analysis persistence (400 ms), track/QML/settings debounces, MIDI jog-release/startup,
FLX10 bounded 2 ms upload chunks, rendering `FrameAnimation`, and waveform analysis coalescers remain
separate because they are event-bound, protocol-bounded, or render-frame work rather than periodic
control scheduling.

## DjMasterBus ownership and retirement (2026-07-13)

| Object / operation | Owner | Thread / boundary | Lifetime rule |
|---|---|---|---|
| `DjMasterBus` and sole `AudioSourcePlayer` | `ApplicationRuntime` | device prepare/release/callback; lifecycle register/unregister | callback is removed before registrations and endpoints |
| Four `DeckRegistration` tokens | `ApplicationRuntime`, declared after decks | Qt bootstrap/lifecycle only | reset token before destroying its graph; token cannot outlive bus |
| `DeckAudioGraph` endpoint | owning `DjEngine` | control prepares/commands; audio callback processes | slot publication follows prepare; null + reader drain precedes destruction |
| Preview `AuxRegistration` | `ApplicationRuntime`, declared after preview player | Qt bootstrap/lifecycle only | same generation and reader-drain rule as decks |
| Slot pointer/generation snapshots | `DjMasterBus` | callback reads atomically once per external block | seq-cst reader-entry/null-publication handshake; registration mutex is never acquired by callback |
| Registration mutex and retirement wait | `DjMasterBus` | control/lifecycle only | may serialize or yield outside RT; never call registration APIs from callback |
| Master/cue/routing/meter values | static/member atomics | control publishes, callback reads/writes, ControlClock/UI reads | scalar relaxed snapshots; no Qt signal per block |

Signal order remains `DeckAudioGraph` post-fader/FX output → four-slot sum → preview → master gain →
pre-limiter peak/clip snapshot → BrickwallLimiter → master routing. PFL is captured inside each graph
before its fader/crossfader gain; selected PFL feeds headphone cue, all PFL feeds booth, and optional
limited master feeds headphone master cue. Extra device channels are cleared and only configured
1-based stereo pairs are written; graph endpoints deliver stereo (mono tracks are duplicated earlier).
