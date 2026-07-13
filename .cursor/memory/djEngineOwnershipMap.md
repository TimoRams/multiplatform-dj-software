# DjEngine Ownership Map

Last audited: 2026-07-13

## DeckAudioGraph extraction

Before this extraction, `DjEngine` directly owned the cache handle and the complete chain:
`CachedPlaybackAudioSource -> AudioTransportSource -> ScratchDeckBridge/ScratchResampler ->
TimeStretchAudioSource -> MixerDspSource`. The mixer's existing order remains Color FX before EQ,
then EQ/color filter, stop effects before the channel fader, channel gain/fader, and beat/pad FX.
`DjEngine::getAudioSource()` returned the mixer to `DjMasterBus`.

`DeckAudioGraph` now uniquely owns every object and the cache handle in that chain, builds the
chain in one constructor, delegates JUCE prepare/release/block processing at its mixer endpoint,
retains realtime counters across source handovers, and validates audio generations. It depends
only on the injected application-owned `AudioPageCache`; it has no `DjEngine`, Qt, metadata,
database, device-service, or master-bus backreference. `DjEngine` retains one
`std::unique_ptr<DeckAudioGraph>` and transitional transport/product logic. Facade implementations
forward through that graph directly rather than storing five non-owning source aliases.

Track handover is `DeckTrackLoader -> DjEngine UI generation/path validation -> AudioPageCache::openTrack
-> DeckAudioGraph::installPreparedTrack`. The graph closes the scratch transport-swap gate, stops
and detaches the transport, retires the old source/handle outside RT, installs the cached source,
publishes track generation to time stretch and scratch, then opens the gate. Stale or invalid
audio generations are rejected and their handles released. Eject invalidates through the current
loader generation before releasing readers.

`DjMasterBus` remains application-owned but now registers `DeckAudioGraph` only through the narrow
`IDeckAudioEndpoint`. `DjEngine::audioEndpoint()` is a bootstrap boundary; the bus neither stores nor
calls the facade. Four application-owned RAII tokens, fixed slot generations and callback-reader
retirement make graph lifetime explicit. Lifecycle stops the callback and resets tokens before graph
destruction.

This map covers every out-of-line `DjEngine` method and every mutable state bundle in `DjEngine.h`. Symbols grouped in one row have the same current owner, target owner, thread model and migration risk. `DjEngine` remains the public QML facade until each target component is introduced.

## Method ownership

| Symbol(s) | Current file | Current responsibility / owner | Target component | Thread access | Main dependencies | Migration risk |
|---|---|---|---|---|---|---|
| `DjEngine`, `~DjEngine`, `prepareForShutdown` | `DjEngine_Core.cpp` | facade construction, wiring and lifecycle / `DjEngine` | QML facade | Qt owner; joins analysis/read-ahead during teardown | all deck components, injected `AudioDeviceService` | high |
| `setCoverArtProvider`, `setLibraryCoverService`, `setLibraryDatabase`, `persistCurrentAnalysisToLibrary` | `DjEngine_Core.cpp` | library/provider wiring and analysis persistence / `DjEngine` | DeckTrackLoader | Qt owner; DB calls may block | raw provider/DB pointers, `TrackData` | high |
| `onTimer`, `tickTransportPlaying`, `tickTransportStopped` | `DjEngine_Core.cpp` | 4 ms control clock coordinating transport/scratch/sync/UI / `DjEngine` | Unclear; future deck control coordinator | Qt owner/control tick; reads audio atomics | almost every deck subsystem | very high |
| `getPreRollSeconds`, `notifyProgressIfNeeded`, `notifyVuMetersIfNeeded`, `emitPlaybackStateChanged` | `DjEngine_Core.cpp`, inline | QML constants and throttled notifications / `DjEngine` | UI-/Visual-State | Qt owner; reads audio atomics | QML signals, mixer meters | medium |
| `applyVolume`, `setVolume`, `applyTrim`, `setTrim`, `applyEqHigh/Mid/Low`, `setEqHigh/Mid/Low`, `applyFilter`, `setFilter`, `applyPolarityInverted`, `setPolarityInverted`, `applyMixerEq`, `applyMixerFilter` | `DjEngine_Core.cpp` | per-deck mixer facade/state / `DjEngine` | DeckAudioGraph + QML facade | Qt/MIDI producer; DSP consumes atomics in audio callback | `MixerDspSource`, Qt signals | high |
| `hydrateLibraryStateForTrack` | `DjEngine_CoreMeta.cpp` | owner-thread cached library state / `DjEngine` facade | DB application phase after DeckTrackLoader | Qt owner only | DB, TrackData | medium |
| `loadTrack`, `applyPreparedTrack` | `DjEngine_TransportLoad.cpp` | submit/result facade / `DeckTrackLoader` owns job | implemented target ownership | Qt submit/apply; joined loader worker prepares result | QPointer, transport handover, analyzer, DB | high transition |
| `resetTrackLoadState`, `updateTrackDuration`, `attachCacheToTransport`, `releaseTransportReaders`, `ejectTrack` | `DjEngine_Transport.cpp` | cache-source/track ownership transitions / `DjEngine` | DeckTrackLoader + DeckTransport | Qt owner; audio graph lifetime | cache handle, analyzer, scratch bridge | high |
| `getProgress`, `getDuration`, `getPosition`, `getVisualPosition`, `getVisualPositionQml`, `loopPreviewOutPosition`, `getPlayheadPositionAtomic`, `isPlaying`, `getTrackData`, `currentCoverImage` | `DjEngine_Transport.cpp` | QML/read-only deck facade / `DjEngine` | QML facade + UI-/Visual-State | Qt/QML; atomic visual reads | transport, TrackData, cover service | medium |
| `togglePlay`, `play`, `pause`, `ensureTransportRunningForPlayIntent`, `freezeTransportAt`, `setPosition`, `setSnapAnchor`, `armSnapFromTransportPosition`, `armVisualSeekSettle`, `syncScratchBridgeToTransport` | `DjEngine_Transport.cpp` | play intent, seek and transport state / `DjEngine` | DeckTransport | Qt owner/control; changes audio-source state outside callback | transport, scratch, sync, injected device service | very high |
| `vuLevelL/R`, `preFaderVuLevelL/R`, `clipDetected`, `gainReduction`, `audioEndpoint` | `DjEngine_Transport.cpp` | meter facade plus explicit bootstrap endpoint / `DjEngine` | DeckAudioGraph + QML facade | audio callback writes; UI reads atomics; bootstrap borrows graph once | `DeckAudioGraph`, `DjMasterBus` | medium |
| `setCueEnabled`, `masterCueEnabled`, `headphoneMix`, `setMasterCueEnabled`, `setHeadphoneMix` | `DjEngine_TransportExtras.cpp` | PFL/master monitoring facade / `DjEngine` | DeckAudioGraph/QML facade; global master controls ultimately AudioDeviceService-adjacent | Qt/MIDI writes atomics read by callback | `DjMasterBus`, mixer source | high |
| `setQuantizeEnabled`, `setReverse`, `setSlip`, `returnToSlipPosition` | `DjEngine_TransportExtras.cpp` | deck transport modes / `DjEngine` | DeckTransport | Qt/control; audio graph transition | reverse source, transport, loop/cue | very high |
| `setDownbeatAtPosition`, `setDownbeatAtCurrentPosition`, `nudgeBeatgridMs`, `nudgeBeatgridBeats`, `doubleBpm`, `halveBpm`, `setManualBpm` | `DjEngine_CoreTempo.cpp` | user beatgrid/tempo edits / `DjEngine` | DeckSyncController + DeckTrackLoader persistence | Qt owner; emits metadata/sync signals | TrackData, DB, sync coordinator | high |
| `updateSpeedAndPitch`, `setKeylock`, `applyTempoPercent`, `setTempoPercent`, `setTempoRangePercent` | `DjEngine_CoreTempo.cpp` | per-deck tempo/keylock control / `DjEngine` | DeckTransport + DeckAudioGraph + DeckSyncController | Qt/control writes audio-source configuration outside callback | time stretch, scratch, sync statics | very high |
| `setTightDoubleSyncEnabled`, `tightDoubleSyncEnabled`, `sameTrackFileAs`, `keylockLatencySeconds`, `updateTightDoubleAlignment` | `DjEngine_Sync.cpp` | tight-double global policy and per-deck alignment / static + deck | SyncCoordinator | Qt/control; global mutex/atomic | other decks, transport, keylock | high |
| `updateSyncMasterLocked`, `propagateMasterTempoLocked`, `currentSyncMaster` | `DjEngine_Sync.cpp` | cross-deck registry/master election / static `DjEngine` state | SyncCoordinator | Qt/control under `s_syncMutex` | raw deck pointers | very high |
| `getBeatPhase`, `getBarPhase`, `getBeatPosition` | `DjEngine_Sync.cpp` | beatgrid-derived QML/sync queries / `DjEngine` | DeckSyncController + QML facade | Qt/control; TrackData mutex | transport, TrackData | medium |
| `updatePhaseCorrection`, `applySyncSeekOffset`, `alignToSyncMasterOnPlay`, `snapPhaseToMaster`, `setSyncEnabled`, `reSync` | `DjEngine_Sync.cpp` | follower PI lock, phase seek, sync lifecycle / `DjEngine` + statics | DeckSyncController + SyncCoordinator | 4 ms Qt control; cross-deck mutex | transport, tempo, other decks | very high |
| `beatgridLocked`, `setBeatgridLocked`, `loadMainCueForCurrentTrack`, `persistMainCuePoint`, `resetMainCueButtonState`, `startMainCueHoldPreview`, `cueButtonPress`, `cueButtonRelease` | `DjEngine_MainCue.cpp` | main cue and beatgrid lock / `DjEngine` | DeckCueLoopController | Qt/control; QTimer callbacks | transport, DB, TrackData | high |
| `hotCues`, `isValidHotCueIndex`, `clearHotCueState`, `loadHotCuesForCurrentTrack`, `persistHotCueSlot`, `isHotCuePad`, `isLoopCuePad`, `hasStorableLoopRegion`, `storeHotCue`, `storeCuePad`, `performCueJump`, `scheduleQuantizedCueJump`, `cancelQuantizedCueJump`, `serviceQuantizedCueJump`, `triggerHotCueJump`, `triggerCuePad`, `triggerHotCue`, `clearCuePad`, `clearHotCue`, `setHotCueColor` | `DjEngine_HotCue.cpp` | hot-cue storage, persistence and quantized trigger / `DjEngine` | DeckCueLoopController | Qt/control tick; analyzer seek hint | transport, loop state, DB | high |
| `activateLoopRange`, `beatIntervalAt`, `updateFxBeatSyncPosition`, `quantizedBeatAt`, `nextBeatBoundaryAfter`, `beatDurationAround`, `startLoopAt`, `setLoopIn`, `setLoopOut`, `toggleLoop4Beats`, `setLoop4Beats`, `toggleLoopThreeQuarter`, `halveLoopLength`, `doubleLoopLength`, `clearLoop`, `deactivateLoop`, `reactivateLoop`, `beatJump`, `applyLoopRangeToAudioSource`, `clearLoopRangeOnAudioSource` | `DjEngine_Loop.cpp` | active loop/beat navigation / `DjEngine` | DeckCueLoopController | Qt/control; changes audio source outside callback | transport, scratch bridge, FX beat timing | very high |
| `savedLoops`, `isValidSavedLoopIndex`, `clearSavedLoopState`, `loadSavedLoopsForCurrentTrack`, `persistSavedLoopSlot`, `storeSavedLoop`, `triggerSavedLoop`, `clearSavedLoop` | `DjEngine_SavedLoops.cpp` | saved-loop slots and DB persistence / `DjEngine` | DeckCueLoopController | Qt owner | DB, active-loop methods | medium |
| `scratchLoopCtx`, `terminateScratchSession`, `updateScrubPlayheadAnchor`, `tickScratchPhysics`, `decayJogNudge`, `syncReverseReaderToHold`, `applyScratchNeutralRouting`, `restorePostScrubPlaybackState`, `pauseForScrub`, `scratchBySeconds`, `setScrubPosition`, `platterAngleDegrees`, `resumeAfterScrub`, `applyScratchReleaseJog`, `finishScrubWithoutInertia`, `applyJogNudge` | `DjEngine_Scratch.cpp` | scratch/jog state machine / `DjEngine` | DeckAudioGraph (future scratch subcomponent) | UI/control and audio callback bridge | ScratchSession/Bridge, reverse/time stretch, loop | very high |
| `setFxEffectType`, `setFxWetDry`, `setFxExternalDelayTime`, `setFxPrimaryParam`, all `setFxSlot*`, `setPadFx`, `clearPadFx`, `activateStopEffect`, `deactivateStopEffect`, `start/stopVinylBrake`, `start/stopEchoOut`, `start/stopBackspin`, `start/stopRollOut`, `setFxSCKnob`, `setFxSCParam` | `DjEngine_Fx.cpp` | per-deck FX facade/state / `DjEngine` | DeckAudioGraph + QML facade | Qt/MIDI commands; audio callback consumes atomic command | MixerDspSource/FxProcessor | high |
| `applyAudioDeviceSettings` overloads, `setOutputFirstChannel` | `DjEngine_Settings.cpp` | retained QML forwarding only / `DjEngine` | QML facade → AudioDeviceService | Qt owner; may block/open hardware in service | injected service | low |
| `getAvailableAudioDeviceTypes`, `getAvailableAudioOutputDevices`, `getAvailableOutputChannelPairs`, `getCurrentAudioDeviceType`, `getCurrentAudioOutputDevice`, `getCurrentAudioSampleRate`, `getCurrentAudioBufferSize`, `isJackServerRunning`, `jackServerStatus`, `lastAudioDeviceError`, `audioDeviceFallbackMessage` | `DjEngine_SettingsQuery.cpp` | retained QML forwarding only / `DjEngine` | QML facade → AudioDeviceService | Qt owner; scans/probes may block | injected service | low |
| `setMasterVolume`, `setAntiClip` | `DjEngine_Settings.cpp` | master DSP facade / static master bus state | QML facade; eventual master-output controller | Qt/MIDI atomic writes, audio callback reads | DjMasterBus | medium |
| `refreshHardwareLatency` | `DjEngine_TransportLatency.cpp` | per-deck visual latency cache/logging using global device snapshot / `DjEngine` | UI-/Visual-State + AudioDeviceService snapshot | Qt owner; never audio callback | service manager, atomics | medium |
| `buildLatencySnapshot`, `totalLatencyMs`, `latencyBreakdown`, `audioPerformanceStats` | `DjEngine_TransportLatency.cpp` | combines global device + per-deck keylock + global limiter statistics / `DjEngine` | QML facade/UI visual state; split global part to service later | Qt/QML; reads audio atomics | service, time stretch, DjMasterBus | high |

## State ownership

| Field(s) | Current owner | Per-deck/global | Target component | Thread access | Dependencies | Risk |
|---|---|---|---|---|---|---|
| `m_audioDeviceService` | borrowed reference in `DjEngine` | one global service, one reference per deck | QML facade dependency | Qt/control only; service manager callback is separate | runtime-owned service | low |
| loader decoder manager, worker, generation, state and pending request | `DeckTrackLoader` | per deck | implemented target ownership | joined loader worker; atomic owner-thread queries | JUCE readers, metadata, cover/cache | medium |
| `CachedPlaybackAudioSource` and active cache handle | `DjEngine` playback pipeline | per deck, pages globally shared | future DeckAudioGraph | Qt source handover; guarded page reads in callback | global `AudioPageCache`, JUCE transport | high |
| `transportSource`, `reverseWrapSource`, `m_playRequested`, reverse/slip/pre-roll/play-history state | `DjEngine` | per deck | DeckTransport | Qt control + audio source callback | readers, scratch, sync | very high |
| `scratchBridge`, `timeStretchSource`, `mixerSource`, `m_scratch`, scratch/jog fields | `DjEngine` | per deck | DeckAudioGraph | audio callback + 4 ms control + UI commands | transport and loop | very high |
| TimeStretch prepared pipelines/config generations/worker | `TimeStretchAudioSource` | per deck | implemented audio subcomponent | worker prepares inactive slot; callback processes/switches; Qt publishes atomics | RubberBand, ScratchDeckBridge source | medium |
| Mixer filter targets/coefficient snapshots/filter histories | `MixerDspSource` | per deck | implemented audio subcomponent; future DeckAudioGraph member | control builds snapshots; callback owns histories/switch | QML/MIDI facade, sample-rate generation | medium |
| `sourcePlayer` | `DjEngine` | per deck but unused for global callback path | Unclear / remove after verification | none observed | legacy JUCE source player | low |
| `timer`, notification clocks/last values, snap/visual/pre-roll fields, `m_atomicPlayheadPos`, `m_pixelsPerSecond` | `DjEngine` | per deck | UI-/Visual-State/control coordinator | Qt owner plus atomic QML reads | transport/audio meters | high |
| `m_trackData`, `m_analyzer`, analysis timer | `DjEngine` | per deck | DeckTrackLoader | analyzer worker + Qt owner | DB, waveform renderers | high |
| track metadata, IDs, cover URL/image flags, segments, raw provider/DB pointers | `DjEngine` | per deck | DeckTrackLoader; facade exposes snapshots | loader + Qt owner | LibraryDatabase/services | high |
| hot-cue slots, saved-loop slots, main-cue state, active-loop state, pending cue jump, quantize flag | `DjEngine` | per deck | DeckCueLoopController | Qt owner/4 ms control | transport, DB, beatgrid | high |
| tempo/range/keylock | `DjEngine` facade/product state | per deck | retained facade + DeckTransport/DeckAudioGraph | Qt/control; audio ratio handoff | time stretch, controller output | high |
| enabled/master role, target BPM, generation, phase error/nudge/integral/clocks/error/actions | `DeckSyncController` | per deck | implemented target ownership | Qt/control only | pointer-free snapshot, own facade boundary | medium |
| registration slots, enable order, master/generation, tight-double policy, Link snapshot | application-owned `SyncCoordinator` | global | implemented target ownership | Qt/control only | controllers; no engine pointers | medium |
| mixer values, polarity, cue-enabled and FX active flags | `DjEngine` | per deck | DeckAudioGraph + facade snapshot | Qt/MIDI producer; audio callback consumer | MixerDspSource | high |
| latency atomics/snapshot/log fields | `DjEngine` | per deck, although hardware part is global and duplicated four times | UI-/Visual-State; global snapshot in AudioDeviceService later | Qt owner + atomic reads | service, keylock, limiter | medium |

## Audio-device architecture before extraction

1. `sharedAudioDeviceManager()` in `AudioDeviceUtils.cpp` created a function-static manager; no application object explicitly owned it.
2. Every deck stored a reference named `deviceManager` to that same singleton-like object.
3. `DjEngine_Settings.cpp`, `SettingsQuery.cpp`, `TransportLatency.cpp`, `Core.cpp` and play recovery accessed it directly.
4. Error/fallback strings, routing helper atomics and latency log snapshots were duplicated or split between deck instances and globals.
5. Backend, output device, sample rate, buffer size and master/headphone/booth routing are application-global. Track sample rate, keylock latency, visual position and deck DSP latency are per deck.
6. `DjMasterBus` owns the sole `AudioSourcePlayer`; bootstrap registered it as the sole manager audio callback after all decks were added.
7. Device open/reopen causes JUCE to call `DjMasterBus::prepareToPlay`, which prepares each deck source with the actual rate/block size.
8. QML calls device functions through `deckA` (fallback `deckB`) and listens to deck error/fallback properties.
9. Device scans and channel probes can allocate/block and are control-thread-only. The audio callback only reads already-published routing/master atomics.
10. Device disappearance was handled by the apply fallback chain and play-time default-device recovery; the saved-device recovery behavior is retained.

## AudioDeviceService after extraction

- `ApplicationRuntime` uniquely owns `AudioDeviceService` before constructing the master bus and four decks.
- `SettingsManager::getAvailableAudioDeviceTypes()` also forwards to the injected service; it no longer creates a temporary second manager.
- The service uniquely owns `juce::AudioDeviceManager`, routing configuration, error/fallback state and an idempotent sample-rate/buffer snapshot.
- Each deck receives a lifetime-bounded constructor-injected reference. Runtime explicitly destroys decks and master bus before the service.
- `DjMasterBus` remains the sole callback owner; bootstrap registers/unregisters it using `audioDeviceService.manager()`.
- Service `configurationChanged` refreshes all deck latency/speed handoffs on the Qt thread. Error/fallback signals are re-emitted by each facade for unchanged QML compatibility. Routing is forwarded once to the master bus.
- Device enumeration, opening, closing, recovery and JACK probing are forbidden in the audio callback. They run from startup/settings/play-recovery control paths and may allocate/block.
- Remaining intentional facade files: `DjEngine_Settings.cpp` is 25 lines and `DjEngine_SettingsQuery.cpp` is 32 lines. `DjEngine_TransportLatency.cpp` remains because it combines global hardware latency with per-deck keylock and global limiter latency.

## Cue/loop ownership after extraction (2026-07-12)

| Function group | Previous implementation | Runtime state owner now | Facade dependencies / behavior retained |
|---|---|---|---|
| Main cue set/press/hold/release/preview/play | `DjEngine_MainCue.cpp` plus transport timer | `DeckCueLoopController::MainCueState` | `DjEngine_CueLoopFacade.cpp` applies seek/play intent, persists SQLite values and emits existing Qt signals; Qt owner/control thread |
| Eight hot cues, labels and colors | `DjEngine_HotCue.cpp` | controller fixed-size array | facade loads/saves through `LibraryDatabase`, applies immediate/deferred transport jumps, preserves QML/MIDI pad API and feedback |
| Active loop, loop-in/out, beat length and activation | `DjEngine_Loop.cpp` plus transport/scratch consumers | `DeckCueLoopController::ActiveLoopState` | facade computes from transport snapshot, publishes prepared bounds to reverse/scratch audio sources; Core performs wrap-around without DB or allocation |
| Eight saved loops, labels and colors | `DjEngine_SavedLoops.cpp` | controller fixed-size array | facade keeps persistence and transport application; performance pads continue to multiplex hot cues/saved loops |
| Quantized pending cue jump | hot-cue fields in `DjEngine` | controller generation-tagged command | controller decides boundary completion/wrap; facade performs the actual seek |

Quantize input is a read-only beat-grid/BPM snapshot concept. Existing dynamic markers, BPM fallback, negative pre-roll and local beat duration semantics are preserved. A track load/eject advances `DeckTrackLoader` generation then calls `beginTrack()`, atomically at control-flow level clearing cues, saved/active loops and stale delayed commands. The old four implementation files were consolidated into `DjEngine_CueLoopFacade.cpp`; the controller has no `DjEngine*`, QObject base, database, transport, or audio callback ownership.

## Track loading before and after DeckTrackLoader (2026-07-12)

Previous path: `DjEngine::loadTrack()` synchronously validated a JUCE file, incremented deck fields, stopped analysis, cleared UI state, and launched a detached thread capturing raw `this`. That thread serialized through `m_loadMutex`, created buffered/direct/overview/auto-cue readers, loaded waveform cache, decoded cover art, then scheduled three independently generation-checked Qt lambdas. Metadata/SQLite hydration and pipeline attachment ran on the Qt owner thread. Destruction could not join the detached worker; generation prevented stale application but not raw-object lifetime access.

Current path:

1. QML (`Library.qml`, `DeckControl.qml`, `EnlargedWaveform.qml`) and existing controller dispatch still call `DjEngine::loadTrack(QString)` on the Qt owner thread.
2. The facade submits the path to its value-owned `DeckTrackLoader`; the current analyzer is joined only after a valid replacement is ready, so invalid requests do not disturb the loaded track.
3. The loader's single persistent, low-priority, joinable worker validates/canonicalizes the path; creates two playback readers; reads JUCE/TagLib/ID3 metadata; loads waveform cache or instant overview; extracts cover bytes/`QImage`; and scans the first ten seconds for auto-cue. No QML signals or database objects are touched there.
4. A move-only `TrackLoadResult` contains generation, canonical identity, metadata snapshot, prepared readers, waveform payload, cover and auto-cue. Only the latest request invokes completion.
5. A `QPointer`-guarded queued callback reaches `applyPreparedTrack()` on the Qt owner thread, which rechecks generation, publishes fatal errors through `trackLoadError`, swaps the existing playback sources, hydrates SQLite analysis/cues, applies TrackData once, starts missing analysis and emits the existing signals.
6. Eject/cancel invalidates the generation. Shutdown rejects new work, clears pending work, joins the loader, then joins analysis and releases playback readers.

`DeckTrackLoader` owns the loading process, request queue, generation, cancellation state, worker and its private decoder manager. It does not own the active transport/read-ahead graph, `TrackData`, SQLite connection, analyzer implementation or QML properties. `DjEngine_TransportLoad.cpp` is now the result-application facade; `DjEngine_CoreMeta.cpp` contains only owner-thread library hydration.

## Audio cache integration boundary

`ApplicationRuntime` owns exactly one `AudioPageCache`; it has no QML or DB dependency. Each `DjEngine` borrows it and holds one per-track handle shared by normal playback and scratch.

Track apply opens the cache handle, installs it into `CachedPlaybackAudioSource` and `ScratchResampler` inside the existing transport-swap gate, and releases it on eject/destruction outside the callback. Normal playback no longer owns `AudioFormatReaderSource`, `BufferingAudioSource`, `ReverseStreamAudioSource`, or a read-ahead thread. `AudioTransportSource` retains transport state and track/device sample-rate conversion; the cache retains original-rate PCM only. Loop/reverse live in the cached source, slip remains deck transport state, negative pre-roll clamps at sample zero, and stale generations fail guarded lookup. Scratch and playback request and read the same immutable pages.

## Transport ownership after DeckTransport (2026-07-13)

Before extraction, `DjEngine` fields and methods jointly owned play request, reverse/slip, track length/rate, audible/scrub/pre-roll/snap positions and timer transitions, while many facade files reached through `DeckAudioGraph::transport()`/`playback()`. Position truth changed between JUCE transport, atomic scratch playhead and negative held position.

Now `DeckTransport` owns play intent, audible/held/background position, playback rate, reverse, slip, pre-roll, EOF, length, track generation and snapshot generation. It has no `DjEngine*`, QObject, QML, metadata, cue database, cache page or concrete DSP/source dependency. Its only audio collaborator is the narrow `DeckAudioGraph` command/snapshot surface. Install keeps rate/reverse/product intent, resets positions/pre-roll/end, and adopts only a newer generation; clear resets play and position data and invalidates through the supplied generation. Reverse and slip-enabled product settings are intentionally retained across a successful install; active cue/loop state is separately reset by `DeckCueLoopController::beginTrack()`.

At the DeckTransport milestone, `DjEngine` still owned Qt signals/API validation, play-history bookkeeping, audio-device recovery, sync policy/static cross-deck coordination, scratch physics, cue/loop decisions/persistence and latency composition. Cue/loop and scratch sent position/loop commands through `DeckTransport`; sync no longer used `AudioTransportSource`, playback source or concrete keylock source. The timer still ran at 4 ms but called one transport update method. The following sync milestone supersedes the static-coordination part of this historical snapshot.

## Sync ownership after DeckSyncController/SyncCoordinator (2026-07-13)

Before extraction, `DjEngine_Sync.cpp` owned a mutex-protected static raw-engine registry,
enable-order vector, master pointer and tight-double atomic. Followers directly read master
TrackData, position, tempo, phases and keylock latency.

Now each engine owns one `DeckSyncController`, while `ApplicationRuntime` owns one
`SyncCoordinator` before the four decks. The facade supplies finite values from its own TrackData
and DeckTransport. Coordinator selects the first still-enabled deck and advances master generation
on handoff or master-track replacement. Controller validates master/track generations and owns PI,
tight-double, error and pending-action state. The facade applies only its own controller actions to
its own transport. QML/MIDI/FLX10 APIs remain unchanged. `DjEngine_Sync.cpp` is now a beat-query and
snapshot/action facade: zero static mutable sync state and zero direct engine-to-engine sync access.

## Scheduling after ControlClock (2026-07-13)

Before this milestone `DjEngine.h` owned a `QTimer` and every deck started a precise 4 ms callback;
the large `onTimer()` mixed scratch physics, cue service, transport, history, sync, FX, position and
meter publication. `DjEngine.h` now owns only a non-owning `ControlClock&` plus an RAII registration.
Named callbacks split fast scratch (250 Hz), transport (125 Hz), sync input/apply (125 Hz), waveform
(60 Hz) and meter (30 Hz). Application-owned coordinator registration runs exactly once between all
deck input and apply phases. `DjEngine` remains the public QML/MIDI/controller facade and no public
API was removed. The number of `DjEngine_*.cpp` files is unchanged; scheduling ownership moved to
`src/app/ControlClock.*`, not into another deck facade file.

## Master-bus boundary after hardening (2026-07-13)

Before, `DjMasterBus` included `DjEngine.h`, stored one `std::vector<DjEngine*>` (up to four raw
pointers), called `getAudioSource()`/`getPflBuffer()` per block, and held a raw preview player.
Registration was implicit and facade lifetime depended on global shutdown ordering. There were zero
explicit `DeckAudioGraph*` members, but up to four transient raw source/graph endpoint pointers had no
local retirement protection.

Now `DjEngine.h` is 658 lines (663 before) and the number of `DjEngine_*.cpp` files remains 13.
`getAudioSource()` and `getPflBuffer()` were removed from the facade; the sole bridge is
`IDeckAudioEndpoint& audioEndpoint()`. The bus has zero `DjEngine`/QML dependencies and zero raw graph
pointers without lifetime protection. Its four raw interface pointers are deliberately non-owning but
are guarded by token ownership, slot generation, null publication and active-reader drain.

Architecture metrics: raw `DjEngine*` storage 1 collection/up to 4 instances → 0; unprotected typed
`DeckAudioGraph*` members 0 → 0 (unprotected transient endpoints up to 4 → 0); maximum callback 4096
with blanket silence above it → arbitrary valid callback chunked at 2048, verified at 8192/16384;
potential callback buffer-growth sites 1 → 0; blocking callback locks 0 → 0; direct master-bus
dependencies on `DjEngine`/QML 1/0 → 0/0. The facade's public QML properties and invokables are
unchanged.

## Database/media boundary (2026-07-13)

No `DjEngine` ownership moved in this task: `DjEngine.h` remains 658 lines and the number of
`DjEngine_*.cpp` units remains 13. ApplicationRuntime owns the general `MediaIoScheduler`; the
LibraryDatabase owns its database worker. DeckTrackLoader and AudioCacheWorker remain deck/cache
specialists. DjEngine receives cover/library value results through existing facade services and has
no SQLite connection, DatabaseWorker pointer, general file scheduler ownership or worker join duty.
