# BrockDJ Regression Checklist

Last updated: 2026-07-12

Use this checklist for manual stability passes after realtime, threading, cache, audio-device or deck-engine changes. Fill the result field with pass/fail, date, platform, audio device, block size and any relevant commit/branch.

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
