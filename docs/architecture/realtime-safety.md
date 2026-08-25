# Realtime Safety

This document defines the callback-safety contract for BrockDJ. It describes
the current implementation names and is normative for changes to audio, cache,
scratch, time-stretch, mixer, FX, routing, and transport handoff code.

## Callback baseline

After `prepareToPlay()`, code reachable from `AudioEngine::getNextAudioBlock()`
must not:

- allocate or resize heap-backed storage;
- open, read, write, decode, enumerate, or probe files or devices;
- execute SQLite or general media-I/O work;
- acquire a blocking mutex, semaphore, condition variable, or wait for a worker;
- start or join a thread;
- construct or destroy objects with non-trivial ownership;
- call DSP `prepare()`, rebuild large buffers, or reset/prewarm a time-stretcher;
- log or emit Qt signals whose execution context is not explicitly callback-safe.

Allowed communication is limited to scalar atomics, immutable prepared
snapshots, bounded non-blocking command queues, fixed double buffers, and
generation-checked handoffs. A cache miss produces bounded faded silence; it
must never fall back to synchronous I/O.

The required steady-state diagnostics are:

```text
0 callback allocations or buffer growths
0 callback file or decoder calls
0 callback database calls
0 callback blocking-lock attempts
0 callback DSP prepare/reset/prewarm calls
0 callback object constructions
```

## AudioEngine callback boundary

`AudioEngine` is the application-wide JUCE callback endpoint. It owns four
fixed `DeckAudioPipeline` instances, the optional `IAuxAudioEndpoint`,
`MasterMixer`, `HeadphoneBus`, `AudioOutputRouter`, and all callback scratch
buffers.

- Processing is bounded to `AudioEngine::kProcessingChunkSize == 2048`.
  Larger device callbacks are processed as consecutive chunks and must not
  grow buffers or return blanket silence.
- The callback snapshots deck and auxiliary pointers under the atomic
  `EndpointReadGuard`. The registration mutex is control-thread-only.
- Auxiliary registration, retirement, generation changes, and reader draining
  happen outside the callback. `AuxRegistration` must be reset before its
  endpoint or `AudioEngine` is destroyed.
- Non-finite deck samples are isolated so one deck cannot poison the master
  mix. `invalidEndpointReads`, `staleGenerationReads`,
  `silentOversizedCallbacks`, allocation, growth, and lock counters must stay
  at zero in automated tests.
- `MasterMixer` is the only summing/master-gain/limiter owner. `HeadphoneBus`
  combines pre-fader cue with the limited master tap. `AudioOutputRouter` only
  maps prepared buffers to configured physical channel pairs.

## DeckAudioPipeline and transport handoff

Each `DeckAudioPipeline` owns its cached playback source, JUCE transport,
`RenderModeRouter`, `TimeStretchProcessor`, and `DeckChannelProcessor`.

- Construction, destruction, track installation, track clearing, source
  retirement, and `prepareToPlay()` are control/device-boundary operations.
- `getNextAudioBlock()` first takes a fixed atomic activity lease covering
  command consumption and the complete prepared chain. A control-side track
  swap publishes its gate and drains an already active block; newly entering
  callbacks observe the gate and clear their destination without waiting.
- The lifetime gate must remain at `DeckAudioPipeline` entry. Moving it below
  `consumeCommands()` reintroduces a playback-source use-after-free window.
- Retired cached-playback violation counters are accumulated so replacing a
  track cannot hide a callback-safety violation.
- `DeckTransport` is the control-thread owner of play intent, position, rate,
  reverse, slip, pre-roll, end state, and generations. It sends commands to
  `DeckAudioPipeline`; it never runs transport policy in the callback.
- `DeckTransportSnapshot` contains values only and uses a consistent atomic
  publication protocol. The injected `std::atomic<double>` playhead is the
  only audio-to-transport position channel.
- Track and state generations must reject stale load, seek, sync, and handoff
  work.

Reverse direction has one owner: `CachedPlaybackAudioSource` reverses PCM
order. Hermite and time-stretch ratios remain positive so the stream is not
reversed twice.

## AudioPageCache and cached playback

- `AudioPageCache::tryGetPage()` and `requestPage()` are the only cache
  operations allowed from callback paths. They use fixed storage and atomics;
  a miss returns an empty guard and enqueues bounded work.
- Hold `AudioPageReadGuard` for the complete read span. Eviction unpublishes a
  page before waiting for readers and freeing it on the worker.
- `AudioCacheWorker` owns decoder creation, page filling, eviction, and
  deallocation. It is joined after all callback readers have stopped.
- A decoder call holds a temporary shared reader lease. `releaseTrack()` first
  invalidates the entry and atomically detaches the owner reference.
  `AudioCacheReleaseMode::Deferred` is required for normal load/handover paths;
  it publishes reader destruction to the decoder worker and never waits for an
  in-flight decode. `WaitForReader` is reserved for explicit eject/shutdown
  where the file handle must be closed on return.
- `CachedPlaybackAudioSource` owns no decoder or fallback reader. Its hit,
  miss, starvation, and recovery paths must keep
  `PlaybackCacheStats::{diskReadsFromAudioThread,decoderCallsFromAudioThread}`
  at zero.
- `ScratchResampler` reads only guarded immutable pages and its preallocated
  local window. `ScratchCacheStats::diskReadsFromAudioThread` must remain zero;
  starvation and recovery use the fixed 128-sample fade.
- Its sinc table and ordinary <=192 kHz source window are built/reserved in
  `prepareToPlay()`. A larger-track fallback growth is permitted only during
  track installation behind `DeckAudioPipeline`'s callback gate. Neither the
  table nor the buffer may be rebuilt from `processBlock()` or
  `processScratchTracking()`.
- FLX10 cumulative position and velocity arrive through one coherent
  `RealtimeScratchInput` snapshot. The audio callback makes one read attempt and
  never spins on the controller writer. Motion telemetry is accumulated in
  locals and published with relaxed stores once per block; it never logs from
  the callback.
- The scratch source/output rate is not the normalized controller rate. A
  192 kHz track on a 48 kHz device is already 4 source samples per output sample
  at normal speed; window and anti-alias calculations must retain that ratio.
- `ApplicationRuntime::audioPageCache` outlives `AudioEngine`, all decks, and
  their cache handles.

## Time stretch, mixer, and FX

`TimeStretchProcessor` owns two prepared pipeline slots and one joinable
preparation worker. Construction/configuration of Signalsmith or Rubber Band,
FIFO setup, buffer sizing, latency discovery, reset, and prewarm happen only in
`prepareToPlay()` or that worker. The callback may atomically claim a matching
`Ready` slot and process the active slot. It must claim before reading
non-atomic slot configuration.

The following `TimeStretchRealtimeStats` fields must remain zero during
transition and stress tests: prepare, reset, prewarm, buffer growth, and
blocking-lock calls from the audio thread.

A loaded but paused deck sets `TimeStretchProcessor::setInputPlaybackActive(false)`.
Keylock then renders the direct router path once, which preserves scratch and FX
tail semantics without running a phase vocoder over silence. Resuming marks the
next keylock entry for the existing bounded seed/crossfade handshake.

`DeckChannelProcessor` receives clamped control targets and prepared
`MixerCoefficientSnapshot` values. The callback may install a complete
generation into the inactive preallocated filter bank and crossfade, but may
not call coefficient factories, allocate, prepare, lock, or construct DSP
objects. Its coefficient-build, prepare, growth, lock, and construction
counters must remain zero.

`FxProcessor` effect-type changes use a bounded generation-tagged command
handoff and are applied once at a block boundary. Effect storage is prepared
in advance; rapid effect switching must not resize buffers or mutate
callback-owned non-atomic state from a producer thread.

## Control, sync, analysis, and I/O boundaries

- `ControlClock` runs only on the Qt owner thread. Late ticks coalesce; no
  catch-up loop is allowed. Blocking I/O, decoding, integrity checks, object
  destruction, and unbounded work do not belong in a clock group.
- `DeckSyncController` and `SyncCoordinator` are control-thread-only. Sync
  snapshots are finite and pointer-free; commands carry master and target
  generations. No master selection, BPM/phase calculation, Qt signal, or sync
  lock is allowed in the audio callback.
- Analysis workers operate on `AnalysisWorkingData` and publish immutable
  results through the result mailbox. The Qt owner applies only matching
  identity, request generation, analyzer generation, and canonical path.
  Cancellation invalidates the request and joins before dependencies are
  reused or destroyed.
- Rendering reads immutable waveform snapshots. It must not detach or copy a
  full shared container for every paint.
- `DatabaseWorker`, `MediaIoScheduler`, and `AudioCacheWorker` are independent
  joined workers. Their bounded queues may reject work; no callback may wait
  for them. SQLite connections and `QSqlQuery` stay on their creating thread.
- The POSIX signal handler performs only the async-signal-safe self-pipe write.
  Qt shutdown work happens on the `QSocketNotifier` side.

## Review gate

Any change touching `getNextAudioBlock()`, `prepareToPlay()` handoffs, cache
misses, scratch, keylock/time stretch, mixer, FX, master routing, or callback
lifetime must:

1. identify the producer and consumer thread;
2. state why no forbidden operation becomes callback-reachable;
3. run the relevant focused tests and the complete `./test-fast` suite;
4. record any remaining accepted risk in `risk-register.md`.
