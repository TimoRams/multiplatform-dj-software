# BrockDJ Realtime Rules

Last updated: 2026-07-13

## AudioPageCache contract

- `tryGetPage()` and `requestPage()` use fixed tables/atomics only: no file, decoder, Qt, log, allocation, mutex or wait.
- A miss returns an empty guard and never performs fallback I/O.
- Hold `AudioPageReadGuard` for the full read span; worker eviction unpublishes first and frees only after guards leave.
- ScratchResampler has no reader/stream fields or fallback. Its only sources are the preallocated local window and `AudioPageReadGuard`; misses enqueue bounded requests and use the 128-sample starvation fade.
- `ScratchCacheStats::diskReadsFromAudioThread` must remain zero in all automated and manual scratch tests.
- `CachedPlaybackAudioSource` may only read immutable guarded pages and enqueue bounded requests. It has no reader, decoder, buffering source, worker wait, or fallback I/O; its miss/recovery fade is fixed at 128 samples.
- `PlaybackCacheStats::{diskReadsFromAudioThread,decoderCallsFromAudioThread}` must remain zero. The cached-playback test verifies both after hit, miss and recovery paths.
- RubberBand objects, ratios, FIFOs and buffers may be created/mutated only by `prepareToPlay` or the joined TimeStretch preparation worker. The callback may process an active slot and atomically adopt a matching `Ready` slot, but may not call RubberBand reset/setters/prewarm or resize buffers.
- `TimeStretchRealtimeStats` prepare/reset/prewarm/growth/lock counters must all remain zero in automated transition and stress tests.
- Mixer EQ/color coefficients are control-prepared immutable value snapshots. `MixerDspSource::getNextAudioBlock` may copy five-value biquad sets into the inactive preallocated bank and crossfade, but may not call JUCE coefficient factories, `prepare`, resize, lock, construct or destroy DSP objects.
- `MixerDspSource::RealtimeStats` coefficient-build/prepare/growth/lock/construction counters must all remain zero.

These rules apply to all code that can execute from the JUCE audio callback, including `DjMasterBus::getNextAudioBlock()`, deck audio sources, scratch/keylock sources, mixer DSP and FX processors.

`DeckAudioGraph::getNextAudioBlock()` is now the stable per-deck callback boundary. It may only
delegate into the already prepared mixer endpoint. Track install/clear, cache-handle release,
source destruction, transport source swaps and generation invalidation are control-thread
operations. `DeckAudioGraph::realtimeStats()` preserves retired cached-playback violations across
track changes so extraction cannot hide a counter by destroying the old source.

## Verboten im Audio-Callback

- Heap-Allokationen.
- Datei- oder Decoderzugriffe.
- Datenbankzugriffe.
- Blockierende Mutexe, Semaphoren, Conditions oder Locks.
- Thread-Starts oder Thread-Joins.
- Qt-Signale mit unklarem oder direktem Ausführungskontext.
- Objektzerstörung oder Ownership-Wechsel mit Destruktorarbeit.
- Logging, das Locks, Formatierungsspitzen oder Dateizugriffe verursachen kann.
- DSP-`prepare()`-Aufrufe.
- Neuinitialisierung großer Buffer.
- Synchrone RubberBand-Prewarm- oder Reset-Vorgänge.

## Erlaubte Kommunikation

- Atomics für kleine triviale Zustände.
- Vorbereitete immutable Snapshots.
- Bounded lock-free oder wait-free Command Queues.
- Double Buffering mit blockweiser Übergabe.
- Vorallokierte Buffer.
- Generation-basierte Übergaben, bei denen stale Arbeit verworfen wird.

## Zielwerte nach `prepareToPlay()`

```text
0 Heap-Allokationen im Audio-Callback
0 Datei- und Decoderzugriffe im Audio-Callback
0 Datenbankzugriffe im Audio-Callback
0 blockierende Locks im Audio-Callback
```

## Review-Regel

Jede Änderung an `getNextAudioBlock()`, `processBlock()`, `prepareToPlay()`-Übergaben, FX-, Scratch-, Keylock- oder MasterBus-Code muss explizit gegen diese Regeln geprüft werden. Wenn eine Ausnahme unvermeidbar erscheint, muss sie als Risiko in `.cursor/memory/riskRegister.md` dokumentiert werden, bevor sie in den regulären Pfad gelangt.

## Transport snapshots and commands

- `DeckTransportSnapshot` contains values only and is published through an atomic sequence protocol; UI/sync/waveform reads do not touch concrete sources.
- Graph transport commands are control-thread operations and must not be called from `getNextAudioBlock()`.
- The only audio-to-transport position channel is the pre-injected `std::atomic<double>` playhead sink. It adds no allocation, Qt signal, lock or lifetime transfer to the callback.
- Track and snapshot generations guard handovers; stale installs must not mutate the current generation.

## Sync control boundary (2026-07-13)

- Snapshot collection, master choice, BPM matching, bar arrange and PI phase math are control-only.
- Cross-deck data is finite and pointer-free. Commands carry master and target-track generations.
- Controllers prepare bounded scalar actions; only the owning facade uses its own `DeckTransport`.
- Fixed four-deck slots avoid update allocation/quadratic traversal. Never traverse decks, choose a
  master, calculate BPM/phase, lock or emit Qt signals in `getNextAudioBlock()`.

## ControlClock boundary (2026-07-13)

- `ControlClock` is Qt-main/control-thread only. It may read published scalar snapshots and issue the
  same bounded control commands as existing UI/MIDI paths; it must never be called by the callback.
- Per logical tick order is fast scratch, transport, all sync inputs, coordinator once, all sync
  applies, waveform/UI, feedback/display, meters, statistics and housekeeping.
- A delayed tick is coalesced to one update with finite delta capped at 100 ms. No catch-up loop is
  permitted. Severe lateness may skip UI/feedback/slow groups, never the current transport/sync pass.
- No decoder, file, database integrity operation, object destruction, unbounded allocation or
  control/audio lock may be added to a fast callback group.

## DjMasterBus callback boundary (2026-07-13)

- The callback may read only the four fixed `IDeckAudioEndpoint` slots and the single auxiliary
  endpoint under `EndpointReadGuard`. The guard is a seq-cst atomic reader count, not a mutex; its
  ordering with seq-cst null publication closes the weak-memory retirement race.
- Register/unregister, endpoint prepare/release, generation changes, token destruction and reader
  draining are control/lifecycle operations. A token must be reset before endpoint destruction.
- Internal processing is always bounded to `kProcessingChunkSize == 2048`. External valid blocks are
  processed as consecutive chunks; 4096, 8192 and 16384 must never trigger buffer growth or a blanket
  silent return.
- `m_deckScratch`, `m_masterBuf` and `m_previewScratch` are exactly stereo × 2048 and are allocated in
  `prepareToPlay()`. No callback `setSize`, vector growth, registration mutation or blocking lock is
  permitted. The limiter is also prepared for 2048 outside the callback and retains continuous state
  across chunks.
- Endpoint post-fader audio and PFL must contain at least the requested chunk. Invalid PFL increments
  instrumentation; NaN/Infinity is detected and isolated so one deck cannot poison the master.
- A callback larger than 2048 may increment `oversizedCallbacks`; the six required violation counters
  and `silentOversizedCallbacks` must remain zero in automated tests.
