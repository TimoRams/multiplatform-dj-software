# BrockDJ Regression Checklist

Last updated: 2026-07-13

Use this checklist for manual stability passes after realtime, threading, cache, audio-device or deck-engine changes. Fill the result field with pass/fail, date, platform, audio device, block size and any relevant commit/branch.

Automated DeckAudioGraph coverage (2026-07-13): the dedicated target runs empty, mono/stereo,
44.1/48/96 kHz and 64–8192-sample cases; play/pause/seek/reverse/loop; scratch enter/move/exit;
keylock/tempo and mixer targets; stale and A→B→C→D handovers; four simultaneous graphs with a
small cache; finite-output checks; controlled clear/destruction; and all aggregate realtime
violation counters at zero. Hardware output, subjective clicks, device callback jitter and actual
master-bus removal timing remain manual items.

Sanitizers (2026-07-13, Linux): dedicated graph target passed combined ASAN+UBSAN and TSAN stress.
LeakSanitizer aborted because the managed runner uses ptrace; the same ASAN+UBSAN binary passed with
`detect_leaks=0`. No sanitizer finding was suppressed. Full release CTest result: 12/12 passed.

## 1. Four tracks nacheinander und gleichzeitig laden

- Vorbereitung: Vier lokal verfügbare Tracks mit unterschiedlicher Länge und Analysezustand bereithalten; App frisch starten.
- Durchführung: Zuerst A, B, C, D nacheinander laden. Danach vier neue Tracks schnell hintereinander auf A, B, C, D laden.
- Erwartetes Verhalten: UI bleibt bedienbar, nur aktuelle Generation je Deck wird übernommen, keine falschen Metadaten/Wellenformen.
- Relevante Logs oder Metriken: Track-load Logs, Analysefortschritt, Warnungen zu stale generation, CPU-Spitzen.
- Ergebnis: offen

Automatisierte Abdeckung (2026-07-12): `analysis_lifetime` prüft Generation/Fehlerzustand,
Abbruch unmittelbar nach Start, Destruktor-Join und verworfene Completion. Manuelle Tests mit
echten A→B→C→D-Deckwechseln und vollständigem GUI-Shutdown bleiben offen.

## 2. Alle vier Decks gleichzeitig abspielen

- Vorbereitung: Vier geladene analysierte Tracks, Master-Ausgang hörbar, vier Decks sichtbar.
- Durchführung: Alle Decks starten, Fader öffnen, Crossfader mittig lassen.
- Erwartetes Verhalten: Keine Dropouts, VU pro Deck und Master plausibel, Transportanzeigen laufen stabil.
- Relevante Logs oder Metriken: Master callback overruns, CPU-Last, Audio-Device-Status.
- Ergebnis: offen

## 3. Keylock auf mehreren Decks aktivieren

- Vorbereitung: Mindestens zwei Tracks mit deutlicher Tempoänderung laden.
- Durchführung: Keylock auf zwei bis vier Decks aktivieren, Tempo stark nach oben/unten bewegen, Wiedergabe laufen lassen.
- Erwartetes Verhalten: Keine harten Klicks beim Umschalten, keine langen Aussetzer, Latenzanzeige bleibt plausibel.
- Relevante Logs oder Metriken: RubberBand/keylock Logs, callback worst usec, hörbare Dropouts.
- Ergebnis: offen

## 4. Während laufender Analyse scratchen

- Vorbereitung: Einen noch nicht analysierten langen Track laden und Analyse starten lassen.
- Durchführung: Während Analysefortschritt sichtbar ist, auf demselben und auf einem anderen Deck scratchen.
- Erwartetes Verhalten: Scratch reagiert sofort, Analyse läuft weiter oder bricht sauber ab, keine Crashes.
- Relevante Logs oder Metriken: Analysefortschritt, ScratchResampler Warnungen, CPU-Last.
- Ergebnis: offen

## 5. Schnelle Vorwärts- und Rückwärts-Scratches ausführen

- Vorbereitung: Track geladen, Wiedergabe oder Pausenzustand mit aktivem Jog/Scratch testen.
- Durchführung: Mehrfach schnelle Vorwärts- und Rückwärtsbewegungen ausführen, inklusive Richtungswechsel nahe Trackanfang.
- Erwartetes Verhalten: Keine Knackser durch Cache-Miss, Playhead bleibt plausibel, kein Sprung auf falsche Position.
- Relevante Logs oder Metriken: Scratch window reloads, callback overruns, hörbare Dropouts.
- Ergebnis: offen

## 6. FX während laufender Wiedergabe schnell wechseln

- Vorbereitung: Mindestens zwei Decks laufen, FX sichtbar, Wet/Dry hörbar eingestellt.
- Durchführung: Effektarten auf beiden FX-Units schnell wechseln, Parameter bewegen, Sound Color FX nutzen.
- Erwartetes Verhalten: Keine Abstürze, keine dauerhafte Stille, keine extremen Pegelsprünge.
- Relevante Logs oder Metriken: FX CPU profile, callback worst usec, Qt warnings.
- Ergebnis: offen

## 7. Während einer Analyse einen neuen Track laden

- Vorbereitung: Analyse eines langen Tracks auf Deck A starten.
- Durchführung: Noch während der Analyse einen anderen Track auf Deck A laden.
- Erwartetes Verhalten: Alte Analyse wird gestoppt oder ignoriert, neue Trackdaten/Wellenform gehören zum neuen Track.
- Relevante Logs oder Metriken: Generation guards, analyzer completion callbacks, TrackData progress.
- Ergebnis: offen

## 8. Anwendung während einer laufenden Analyse schließen

- Vorbereitung: Analyse eines langen Tracks starten.
- Durchführung: App über UI schließen und zusätzlich einmal per SIGTERM/Ctrl+C testen.
- Erwartetes Verhalten: Sauberer Shutdown ohne Crash, keine hängenden Threads, keine falsche Recovery-Warnung ohne DB-Schreibvorgang.
- Relevante Logs oder Metriken: Analyzer stop logs, shutdown logs, DB clean-shutdown marker.
- Ergebnis: offen

Automatisierte Signalabdeckung (2026-07-12): `posix_signal_handler` sendet echte SIGINT/SIGTERM
einschließlich Burst und prüft genau einen Qt-Shutdownwunsch, vollständig geleerte Pipe,
nonblocking/CLOEXEC-Flags, geschlossene Deskriptoren und sichere Neuinitialisierung. Headless
Linux-App-Läufe mit Ctrl+C und SIGTERM beendeten sich ohne Crash oder Deadlock; Audio/MIDI waren
in der Sandbox nicht verfügbar und eine laufende echte Analyse wurde dabei nicht simuliert.

## 9. Audiointerface während des Betriebs trennen oder wechseln

- Vorbereitung: Externes Audiointerface oder Controller-Ausgang verbinden, Track läuft.
- Durchführung: Interface trennen oder in den Einstellungen auf ein anderes Gerät wechseln.
- Erwartetes Verhalten: App bleibt stabil, Audio fällt kontrolliert auf Default/saubere Fehlermeldung zurück, Routing bleibt konsistent.
- Relevante Logs oder Metriken: AudioDeviceManager Fehler, fallback logs, device/routing status.
- Ergebnis: offen

Automatisierte Service-Abdeckung (2026-07-12): `audio_device_service` prüft eine einzige
Manager-/Konfigurationsinstanz für mehrere Deck-Views, globale Sample-Rate-/Buffer-Sichtbarkeit,
Normalisierung, idempotente Signale, Routing und borrowed Lifetime. Headless Linux startete vier
injizierte Decks und beendete sauber; echte Hardwarelisten, Wiedergabe und Hot-Unplug bleiben offen.

## 10. Loops, Hot Cues, Slip und Sync kombiniert verwenden

- Vorbereitung: Zwei analysierte Tracks mit Beatgrid laden, Sync aktivieren, Hot Cues und Loop setzen.
- Durchführung: Loop aktivieren, Hot Cues triggern, Slip nutzen, Sync ein/aus und Master-Handoff testen.
- Erwartetes Verhalten: Keine eingefrorene Wellenform, Sync bleibt musikalisch plausibel, Slip kehrt korrekt zurück.
- Relevante Logs oder Metriken: Sync master state, phase correction, waveform/playhead jumps.
- Ergebnis: offen

## 11. Anwendung während einer laufenden Datenbank- oder Backupoperation schließen

- Vorbereitung: Library-Änderung auslösen, z. B. Trackbewertung/Playlist/Analysepersistenz, Backup-Timer abwarten bis Sync möglich läuft.
- Durchführung: App direkt danach schließen.
- Erwartetes Verhalten: Keine Crashs, DB öffnet beim nächsten Start ohne unnötige Warnung; bei echtem Abbruch erscheint eine präzise Recovery-Warnung.
- Relevante Logs oder Metriken: `scheduleBackupSync`, deferred backup sync, WAL checkpoint, mirrored database status.
- Ergebnis: offen

## 12. Blockgrößen von 64 bis größer als 1024 testen

- Vorbereitung: Audio backend wählen, das verschiedene Buffergrößen erlaubt; möglichst 64, 128, 256, 512, 1024 und größere Werte testen.
- Durchführung: Pro Blockgröße mindestens zwei Decks abspielen, Keylock umschalten, scratchen und FX wechseln.
- Erwartetes Verhalten: Keine Stille bei großen Blöcken, keine Dropouts bei kleinen Blöcken außer erwartbarer CPU-Grenze des Systems.
- Relevante Logs oder Metriken: Audio callback worst usec/overruns, MasterBus block-size warnings, CPU load.
- Ergebnis: offen

## 13. Cue/Loop-Controller nach Extraktion

- Automatisch: `ctest --test-dir build --output-on-failure`; `cue_loop_controller` prüft acht Slots, ungültige/NaN-Grenzen, Pre-Roll, dynamisches Beatgrid und BPM-Fallback, Loop-Aktivierung, gespeicherte Loops, schnelle Befehlsfolgen und Trackgeneration.
- Manuell offen: Main-Cue Hold/Release und Cue+Play; Hot-Cue QML/MIDI-Feedback; Loop halbieren/verdoppeln/Reverse/Scratch/Slip; gespeicherte Loop-Persistenz; Trackwechsel während eines wartenden quantisierten Sprungs; Neustart mit SQLite-Daten.
- Erwartung: unveränderte öffentliche `DjEngine`-API und Signale; keine Cue-/Loop-Zustände des alten Tracks; keine Datenbank- oder Controllerarbeit im Audiocallback.

## 14. DeckTrackLoader nach Extraktion

- Automatisch: `track_loader` erzeugt Mono-/Stereo-WAVs mit 44,1/48 kHz und prüft erfolgreiches Laden, Reader/Metadaten, leere/fehlende/beschädigte Dateien, A→B→C→D-Generationen, Cancel, expliziten Shutdown und Destruktor-Join.
- Manuell: Während Track A lädt Track B wählen; vier Decks schnell laden; denselben Track auf zwei Decks; Wechsel während Wiedergabe/Analyse; beschädigte und danach gültige Datei; Beenden während Load; Cover/Metadaten/Waveform/Cues prüfen; MIDI-Load pro Deck.
- Erwartung: Nur die aktuelle Generation wird sichtbar; kein altes Cover/Metadata/Analyseergebnis; keine detached Threads; vorhandenes Trackwechsel- und Wiedergabeverhalten ohne Crash oder dauerhafte Stille.
- Ergebnis: automatische Loader- und Gesamttests bestanden; echte QML-, Audiohardware- und MIDI-Prüfung offen.

## 15. Globaler AudioPageCache-Grundbau

- Automatisch: Page-Arithmetik, Mono/Stereo, kurze/exakte/partielle Pages, Miss→Request→Hit, Sharing/Release, Generation, beschädigte Datei, Budget/Eviction, fester Zufallsstress und Shutdown.
- Erwartung: `residentBytes <= budget`, finite immutable PCM-Pages, kein stale Publish, RT-Miss ohne Decode.
- Manuell: keine Audioauswirkung, da Scratch und Playback noch nicht integriert sind.

## 16. Cache-basierter Scratch-Pfad

- Automatisch: Mono/Stereo, Page-Grenze, partielle Page, 64–8192 Samples, langsam/schnell vor/rückwärts, Positions-Tracker, Loop-Wrap, Pre-Roll, Miss/Fade/Recovery, Handlewechsel und 2.000 deterministische Sprünge.
- Musswert: `ScratchCacheStats::diskReadsFromAudioThread == 0` vor und nach Stress.
- Manuell offen: Jogwheel-Gefühl, Backspin, Loop/Slip/Keylock/FX, vier Decks, kleiner Cache, Shutdown während Scratch.

## 17. Cache-basiertes normales Playback

- Automatisch (2026-07-12): `cached_playback` prüft Mono/Stereo, 44,1/48/96/192 kHz, Blockgrößen 64–8192, Page-Grenzen, partielle Endpage/EOF, vorwärts/rückwärts, Loop-Wrap, Pre-Roll-Clamp, Miss/Fade/Recovery, Sharing und finite Samples.
- Musswerte bestanden: `diskReadsFromAudioThread == 0` und `decoderCallsFromAudioThread == 0`; der Source-Code besitzt keinen Reader-/Decoder-Einstiegspunkt oder Fallback.
- Messung: synthetischer Cache-Hit, 512 Samples, 1000 Blöcke: 5,34 µs Mittelwert in diesem Headless-Build. Kein belastbarer Vergleich zur entfernten Buffering-Pipeline erhoben.
- Manuell offen: echte Audiohardware, hörbare Loop/Reverse/Slip-Übergänge, Tempo/Keylock, vier Decks unter Last, Hot-Unplug und Shutdown während Wiedergabe.

## 18. RubberBand-Doppelpipeline

- Automatisch (2026-07-12): `time_stretch` prüft 44,1/48/96/192 kHz, Mono/Stereo-Ausgabe, 64–8192 Samples, Bypass, Keylock, Tempoänderungen, Koaleszierung, Pipeline-Aktivierung, Scratch-Rückkehr, finite Ausgabe und deterministischen Wechselstress.
- Musswerte: Callback-Zähler für Prepare, Reset, Prewarm, Bufferwachstum und Lockversuche jeweils null; alle zehn CTest-Targets bestanden.
- Headless-Messung: 512-Sample-Block über 1000 Aufrufe, 3,29 µs Mittelwert und 10,72 µs Maximum auf diesem Build; kein Hardware-Callback-Vergleich.
- Manuell offen: hörbarer 256-Sample-Crossfade, extreme Tempo-/Pitchqualität, kurze Loops, Seek/Trackwechsel während laufender Hardwareausgabe und vier Decks mit Controller-Scratch.

## 19. Mixer-EQ und Sound-Color-Snapshots

- Automatisch (2026-07-12): `mixer_dsp` prüft stabile/finite Low-Shelf-, Peak-, High-Shelf-, Low-/High-Pass-Koeffizienten bei 44,1/48/96/192 kHz, 64–8192 Samples, LP-Dämpfung, schnelle kombinierte Reglerbewegungen und parallele Control-/Audiozugriffe.
- Musswerte: Callback-Zähler für Koeffizientenbau, Prepare, Bufferwachstum, Locks und Objektkonstruktion jeweils null; alle elf CTest-Targets bestanden.
- Headless 512-Sample-Messung: 22,46 µs Mittelwert, 135,75 µs Maximum. Hardware-/Vierdeckvergleich und subjektive EQ-/Filterabstimmung bleiben offen.

## 20. DeckTransport

- Automatisch (2026-07-13): ohne Track, idempotentes Play/Pause, EOF/Seek-Clamp, schnelle Seeks, Reverse, Slip-Hintergrund/Rückkehr, negative Pre-Roll-Position und Nullübergang, Loop-Control-Update, Generation/Stale-Install/Clear, A→B→C→D, parallele Snapshot-Leser und deterministischer Vierdeck-Stress.
- Musswerte: finite pointerfreie Snapshots, monotone State-Generation, korrekte Trackgeneration, keine stale Anwendung und alle aggregierten RT-Verstoßzähler null.
- Manuell offen: echte QML-Waveform/Remaining-Time, FLX10/MIDI-Feedback, hörbare Cue/Loop/Slip/Reverse/Scratch-Übergänge, vier Decks auf Audiohardware, Hot-Unplug und Shutdown unter Controllerlast.
- Sanitizer: ASAN/UBSAN ohne Befund; LeakSanitizer im ptrace-Runner technisch nicht verfügbar; TSAN nach Korrektur des gefundenen TimeStretch-Ready-Slot-Races ohne Befund.

## Sync controller/coordinator regression (2026-07-13)

- [x] First enabled master, follower enable, explicit deterministic request, promotion on disable/remove.
- [x] Master pause/EOF/no-track retains the existing product role.
- [x] Master-track and target-track generation invalidation; rapid A→B→C→D replacement.
- [x] Equal/different/half/double/invalid BPM yield only finite bounded actions.
- [x] Beat/bar phase, arrange deadzone/bound, PI nudge, reSync, Scratch/Reverse/Slip/Loop.
- [x] Fixed-seed four-deck stress; release, ASAN+UBSAN and TSAN pass.
- [x] Integrated four `DeckAudioGraph` + four `DeckTransport` + four controllers + one coordinator;
  Play/Pause/Seek/Scratch/Reverse/Slip/Loop/master/Link changes keep every RT counter at zero.
- [ ] Hardware/QML: SYNC LEDs, FLX10 feedback, audible handoff, same-file tight double.
- [ ] Hardware/manual: Link leader/follower enable/disable and peer phase.

## ControlClock regression (2026-07-13)

- [x] stopped/start/duplicate start/stop, zero/one/four targets and explicit unregister.
- [x] 250/125/60/30/10/2 Hz rate ratios in deterministic manual-clock mode.
- [x] transport→all sync inputs→one coordinator→all applies→UI→feedback→slow ordering.
- [x] finite/capped delta, delayed tick, no catch-up avalanche and slow/UI shedding.
- [x] position epsilon and MIDI value deduplication.
- [x] Four cached graphs/transports/sync controllers, scratch/loop/slip/reverse/master/Link changes,
  fixed seed, bounded callbacks, controlled unregister/stop and all RT counters zero.
- [x] Main target and 15 CTest targets pass; combined ASAN/UBSAN passes (LSan unavailable under ptrace).
- [x] Dedicated ControlClock and four-deck integration targets pass TSAN without a report.
- [ ] Manual: FLX10 display/keepalive/upload, MIDI LEDs/VU, Link peers, visual 60 Hz waveform and
  suspend/resume on a release GUI build.

## DjMasterBus regression (2026-07-13)

- [x] Zero/one/two/four decks; invalid/occupied/duplicate slots; movable and repeated-reset tokens;
  stale generation rejection; prepare/release/re-prepare and registration rejection after shutdown.
- [x] 64, 128, 256, 512, 1024, 2048, 4096, 8192 and 16384 sample callbacks are finite and retain a
  non-silent tail; large callbacks use 2048 chunks without callback buffer growth.
- [x] Unity/sum/fader/crossfader-derived post-fader gains, mute, master gain, pre-fader one/multi-cue,
  booth, master/cue mix, peak/clip, limiter/high input, silence and NaN/Infinity isolation.
- [x] Concurrent callback and token retirement prove no calls after reset; registration cycle and
  controlled shutdown are deterministic.
- [x] Fixed-seed four real `DeckAudioGraph` stress covers varying blocks, Play/Pause, track replacement,
  scratch, keylock, EQ/FX, fader/crossfader targets, cue and registration swaps.
- [x] Required `MasterBusRealtimeStats` values are zero; `oversizedCallbacks` alone increases for
  blocks over 2048. Graph/cache/time-stretch/mixer RT counters remain zero.
- [x] Main Fast-Build and all 16 CTest targets pass. Master-bus plus graph targets pass ASAN+UBSAN
  (`detect_leaks=0` only because ptrace blocks LSan) and TSAN without a report.
- [ ] Manual hardware: ALSA/JACK/CoreAudio/ASIO block sizes and hot-unplug; audible cue/crossfader/
  limiter transitions, mono/multi-output device routing, callback jitter and shutdown under load.

## Database and media I/O regression (2026-07-13)

- [x] Main target, `BrockDJ_database_worker_tests`, `BrockDJ_media_io_scheduler_tests` build.
- [x] DB start/repeated start, stop/repeated stop, CRUD, FIFO, batch commit/rollback, cancellation,
  stale generation, quick/full check, backup temp publication and connection open/close thread hashes.
- [x] Media cover/thumbnail, path validation, bounded folder scan, cancellation, generation switch,
  missing path, invalid image and repeated stop.
- [x] No `detach()` remains in DB/library/media paths; no periodic `integrity_check`; AudioCacheWorker
  remains independent; full release CTest suite has 18 passing targets.
- [x] Both worker targets pass combined ASAN+UBSAN (`detect_leaks=0` due runner ptrace) and TSAN.
- [ ] Portable disk-full/permission and network-share behavior; GUI stress with a production-scale library.

## Analysis snapshot and queue regression (2026-07-14)

- [x] Main build and all 20 CTest targets pass.
- [x] Snapshot validation rejects malformed/NaN results; owner-thread apply and identity/request-generation stale rejection are covered.
- [x] Bounded queue covers priority, deduplication/promotion, fairness and 10/100/1k/10k enqueue/dequeue measurements.
- [x] Analysis snapshot, queue and lifetime targets pass ASAN+UBSAN with `detect_leaks=0` (ptrace runner limitation).
- [ ] TSAN target run and production GUI test: rapid deck load/eject/reload during visible waveform rendering.
- [ ] Profile/route the final WaveformCache artifact I/O to `MediaIoScheduler` if it can delay analysis throughput.
