# Risk Register

This register contains current open, accepted, or hardware-verification risks.
Closed implementation history belongs in version control and tests, not in the
active register.

| Priority | Risk | Current boundary/evidence | Required follow-up | Status |
| --- | --- | --- | --- | --- |
| P0 | Realtime callback regression | `AudioEngine`, `DeckAudioPipeline`, cache, scratch, stretch, mixer, and FX expose zero-violation counters and stress tests. A future call path could still reintroduce allocation, I/O, locks, or preparation. | Apply `realtime-safety.md` to every callback-related review and keep all counters at zero. | guarded |
| P1 | Physical audio backends remain incompletely verified | Automated tests cannot prove ALSA/JACK/CoreAudio/ASIO callback jitter, hot-unplug behavior, or audible transition quality. | Run the hardware matrix in `docs/testing/regression-checklist.md`. | open |
| P1 | Physical FLX10 scratch quality and latency remain unverified | Automated tests now cover exact tick travel, USB receive jitter, C2 trajectory continuity, 8x release, 192 kHz alias rejection, and 128/256/512-sample buffers. They cannot prove firmware timestamps, driver jitter, end-to-end latency, hand feel, or subjective Serato/Rekordbox parity. | Run and capture the level-matched FLX10 matrix in `scratch-engine-quality.md` and `docs/testing/regression-checklist.md`; record backend, buffer, sample rates and XRuns. | open |
| P1 | Remaining FLX10 display/MIDI/Link hardware behavior is unverified | Protocol/unit tests cover parsing, routing, display values, and scheduled callbacks, not physical refresh behavior or multi-peer networking. | Verify state/waveform updates, MIDI feedback, deck assignment and multi-peer Link on hardware. | open |
| P1 | `ControlClock::Registration` is non-owning | Registration tokens hold a clock back-pointer; correctness depends on reset-before-clock-destruction ordering. | Preserve stop → unregister → destroy ordering and lifecycle tests. | accepted |
| P1 | `AudioEngine::AuxRegistration` is non-owning | The preview token holds `AudioEngine` and endpoint pointers. | Reset the token before preview endpoint or engine destruction; retain concurrent retirement tests. | accepted |
| P1 | UI event-loop stalls delay all control groups | One 250 Hz `ControlClock` intentionally serializes control work. Blocking UI/database work delays transport/sync visibility even though late ticks coalesce. | Keep I/O, decoding, integrity work, and unbounded loops out of clock callbacks; monitor late/worst statistics. | accepted |
| P1 | Qt-owned compatibility database calls may stall UI | `LibraryDatabase` still performs schema/startup and small legacy CRUD through its owner-thread connection. Large or contended operations can delay result application. | Move only measured problem queries to immutable `DatabaseWorker` commands; do not cross-thread a QSQLITE connection. | open |
| P1 | Final analysis artifact I/O can reduce throughput | Final `WaveformCache` serialization remains coupled to analysis completion. Slow disks can delay the next job. | Profile first; if material, route an immutable artifact job through `MediaIoScheduler`. | open |
| P1 | Shutdown pumps Qt events during teardown | `ApplicationLifecycle::shutdownApplication()` calls `deleteLater()` and bounded `processEvents()`, which can re-enter queued code. | Replace with explicit ownership teardown when QML root lifetime can be proven without event pumping. | open |
| P1 | QML startup failure has limited fallback behavior | Bootstrap smoke tests validate resources, but device/backend objects may exist before a real window proves usable. | Define explicit startup states and test QML load/render failure reporting. | open |
| P1 | Time-stretch preparation can be superseded | Rapid tempo, backend, keylock, or track changes can invalidate an in-progress inactive slot. Generation checks discard stale work, temporarily retaining the previous/bypass configuration. | Keep claim-before-read and latest-generation behavior; rate-limit only if profiling shows pressure. | accepted |
| P1 | Cache starvation produces intentional faded silence | Cache misses are callback-safe, but an undersized budget or slow worker can starve playback/scratch. | Tune with production media and expose actionable cache diagnostics before changing the no-fallback contract. | accepted |
| P2 | Released cache metadata persists until shutdown | `AudioPageCache` retains small slot metadata so stale handles remain safe while PCM is evicted. Many unique tracks can grow metadata. | Add control-thread epoch reclamation only if production measurements justify the complexity. | accepted |
| P2 | `DjEngine` remains a broad public facade | The API spans transport, cue/loop, mixer, FX, scratch, sync, metadata, and diagnostics across responsibility-named implementation files. | Continue only as a separately reviewed facade/ownership refactor; preserve the public QML/controller contract. | deferred |
| P2 | Large QML surfaces duplicate concepts | Heavy mutually exclusive surfaces are now lifetime-gated, but `SettingsPanel.qml` and `SettingsWindow.qml` remain near-parallel; `Library.qml`, `TopHeader.qml`, and `DeckControl.qml` remain monolithic. Textual splitting alone does not reduce runtime work. | Add desktop/AIO visual parity and popup/focus ownership tests before extracting shared settings content; split other files only at measured reusable boundaries. | open |

## Invariants retained from resolved incidents

- FX type changes use bounded generation-tagged commands applied at block
  boundaries; producer threads do not reinitialize callback-owned buffers.
- Scratch and normal playback have no synchronous reader/decoder fallback.
- Scratch cumulative tick travel owns position; velocity filtering may not alter
  total platter travel. Physical input/release is bounded at 8x, the tracker has
  private 10x catch-up headroom, and its prebuilt absolute-rate filter covers
  track/device sample-rate conversion as documented in
  `scratch-engine-quality.md`.
- The complete `DeckAudioPipeline` callback holds the track-lifetime lease;
  normal handover atomically detaches a decoder reader without waiting, while
  explicit eject/shutdown waits for file-handle closure.
- Paused normal transport bypasses the time-stretcher; scratch continues through
  the direct render path and keylock re-entry uses the bounded seed handshake.
- Time-stretch slots are atomically claimed before non-atomic configuration is
  read; preparation and destruction remain off the callback.
- Keylock is a per-block routing decision, never part of the pipeline identity:
  toggling it, entering or leaving scratch, and loading a track must not queue a
  pipeline rebuild.
- The stretcher is seeded either by the pipeline worker, which owns it
  exclusively for the duration, or by the callback when the block budget can
  absorb it — never by both.
- Filter coefficients are prepared as complete finite snapshots and activated
  in preallocated banks.
- Analyzer and loader workers are never detached; cancellation invalidates
  generations and joins before dependency reuse.
- POSIX signal handlers never call Qt.
- Reverse direction is applied once in cached PCM reading; resampling ratios
  remain positive.
- Progressive waveform chunks are immutable and generation-checked before UI
  publication.
