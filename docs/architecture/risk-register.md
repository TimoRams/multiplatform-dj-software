# Risk Register

This register contains current open, accepted, or hardware-verification risks.
Closed implementation history belongs in version control and tests, not in the
active register.

| Priority | Risk | Current boundary/evidence | Required follow-up | Status |
| --- | --- | --- | --- | --- |
| P0 | Realtime callback regression | `AudioEngine`, `DeckAudioPipeline`, cache, scratch, stretch, mixer, and FX expose zero-violation counters and stress tests. A future call path could still reintroduce allocation, I/O, locks, or preparation. | Apply `realtime-safety.md` to every callback-related review and keep all counters at zero. | guarded |
| P1 | Track handover may stall the Qt/control thread | `DeckAudioPipeline::installPreparedTrack()` and `clearTrack()` detach and retire cached sources outside RT. Prior profiling observed a slow retirement under stress. | Profile release builds with long/compressed files; if confirmed, design deferred non-RT retirement without weakening page-guard lifetime. | open |
| P1 | Physical audio backends remain incompletely verified | Automated tests cannot prove ALSA/JACK/CoreAudio/ASIO callback jitter, hot-unplug behavior, or audible transition quality. | Run the hardware matrix in `docs/testing/regression-checklist.md`. | open |
| P1 | Physical FLX10/MIDI/Link timing remains unverified | Protocol/unit tests cover parsing, routing, display values, and scheduled callbacks, not device latency or refresh behavior. | Verify FLX10 state/waveform updates, MIDI feedback, and multi-peer Link on hardware. | open |
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
| P2 | Large QML surfaces duplicate concepts | `SettingsPanel.qml` and `SettingsWindow.qml` are near-parallel; `Library.qml`, `TopHeader.qml`, and `DeckControl.qml` are monolithic. | Inventory bindings and visual behavior before extracting shared components. | open |

## Invariants retained from resolved incidents

- FX type changes use bounded generation-tagged commands applied at block
  boundaries; producer threads do not reinitialize callback-owned buffers.
- Scratch and normal playback have no synchronous reader/decoder fallback.
- Time-stretch slots are atomically claimed before non-atomic configuration is
  read; preparation and destruction remain off the callback.
- Filter coefficients are prepared as complete finite snapshots and activated
  in preallocated banks.
- Analyzer and loader workers are never detached; cancellation invalidates
  generations and joins before dependency reuse.
- POSIX signal handlers never call Qt.
- Reverse direction is applied once in cached PCM reading; resampling ratios
  remain positive.
- Progressive waveform chunks are immutable and generation-checked before UI
  publication.
