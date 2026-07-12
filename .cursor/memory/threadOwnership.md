# Thread Ownership

Last updated: 2026-07-12

| Object/state | Owner/lifetime | Allowed callers | Blocking/allocation | Audio-callback rule |
|---|---|---|---|---|
| `AudioDeviceService` | `ApplicationRuntime`; created before decks, destroyed after decks/master bus | Qt application/control thread | enumeration, device open/close, JACK probing and configuration may block/allocate | never call service configuration/enumeration from callback |
| `AudioDeviceService::ConfigurationSnapshot` | service, Qt owner thread | service apply/device-notification path; deck/QML readers on Qt thread | trivial | callback does not read it |
| `DjMasterBus` routing/master atomics | master bus/static atomics; callback unregistered before destruction | service/bootstrap publish on Qt thread; audio callback reads | publishing is trivial atomic work | callback read-only, no device query |
| `DjEngine` facade | runtime, one per deck; destroyed before service | QML/MIDI/control thread unless method says atomic/audio-only | transport/device changes outside callback | `getAudioSource`/PFL and DSP processing follow explicit audio path |
| Deck audio graph (`MixerDspSource`, time stretch, scratch bridge) | each deck | prepare/configure on control/device callback boundary; process on audio callback | preparation may allocate; processing must not | realtime rules apply strictly |
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

Normal deck playback now has the same lifetime boundary. `CachedPlaybackAudioSource` is deck-owned, holds an `AudioCacheHandle`, and is installed into `AudioTransportSource` on the Qt/control thread. The callback only holds `AudioPageReadGuard`s and submits bounded page requests; misses advance the track timeline while a 128-sample fade moves to/from silence. `AudioTransportSource` remains responsible for track-rate to device-rate conversion. Track replacement/eject disconnects the transport before destroying the cached source and releases the handle outside the callback. The former `AudioFormatReaderSource`/`BufferingAudioSource` and deck read-ahead thread no longer exist.

Time stretch lifetime: each deck-owned `TimeStretchAudioSource` owns two `Pipeline` slots and one joinable worker. `prepareToPlay` synchronously prepares the initial slot before playback; the worker exclusively constructs/configures/prewarms inactive slots and publishes `Ready`. The audio callback processes only `Active`, validates track/config generations, atomically switches at a block boundary and marks the former slot `Empty` afterward. Worker mutex/CV access and all object destruction are forbidden to the callback. Shutdown rejects configurations, invalidates work, wakes and joins the worker, then releases RubberBand/buffers outside audio processing.
