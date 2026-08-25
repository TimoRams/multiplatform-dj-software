# Regression Checklist

Use this checklist after changes to realtime audio, threading, cache, device
routing, deck transport, analysis, QML shell, controller integration, or
lifecycle code. Record date, platform, audio backend/device, sample rate,
buffer size, build type, and pass/fail notes.

## Automated baseline

Run the repository entry points:

```bash
cmake -S . -B build
cmake --build build -j
./test-fast
```

The complete CTest suite covers the following relevant focused groups:

| Area | Tests |
| --- | --- |
| UI/QML | `ui_scale`, `waveform_zoom`, `ui_layout`, `qml_component`, `waveform_render_stability` |
| Mixer/audio graph | `smoke`, `mixer_dsp`, `deck_audio_graph`, `master_bus`, `audio_routing_contract` |
| Transport/sync | `deck_transport`, `sync_coordinator`, `control_clock`, `render_pressure_policy`, `cue_loop_controller` |
| Cache/stretch | `audio_page_cache` (deferred/synchronous reader retirement), `scratch_cache` (including 192/48 kHz aliasing), `scratch_motion` (trajectory/spectrum/release), `cached_playback`, `time_stretch` (paused-keylock bypass) |
| Analysis/waveform | `analysis_lifetime`, `analysis_snapshot`, `library_analysis_manager`, `progressive_waveform_publication`, `waveform_motion`, `waveform_line_*` |
| Devices/controllers | `audio_device_service`, `alsa_midi_line_parser`, `midi_14bit_accumulator`, `flx10_jog_routing`, `flx10_display_protocol`, `parameter_store` |
| Workers/lifecycle | `posix_signal_handler`, `audio_thread_scheduling`, `database_worker`, `media_io_scheduler`, `track_loader`, `dj_engine_api_contract` |

Callback-related changes must also confirm that `AudioEngineRealtimeStats`,
`DeckAudioPipeline::RealtimeStats`, `PlaybackCacheStats`, `ScratchCacheStats`,
`TimeStretchRealtimeStats`, and `DeckChannelProcessor::RealtimeStats` retain
zero violation counters.

`deck_audio_graph` includes a concurrent callback versus 200-generation
install/clear stress. For lifetime-boundary changes, also run that target under
ThreadSanitizer where the platform toolchain supports it.

## Manual core scenarios

### Multi-deck loading and replacement

- Load four local tracks with different lengths, sample rates, and analysis
  states, first sequentially and then rapidly across A/B/C/D.
- Replace A with B/C/D while loading and while analysis is active.
- Repeat with a long compressed file on slow/removable storage. Replacing the
  track must remain responsive while an in-flight decoder finishes; explicit
  eject must wait until the backing file handle is actually closed.
- Verify that only the newest generation publishes metadata, cover, waveform,
  cues, beatgrid, and audio; UI remains responsive and shutdown joins workers.

### Four-deck playback

- Play all four decks with open faders and the crossfader centered.
- Exercise mono/stereo tracks and 44.1/48/96 kHz device rates.
- Verify finite output, plausible pre-/post-fader meters, no dropouts, and
  stable transport/waveform motion.

### Buffer-size matrix

- Test device buffers 64, 128, 256, 512, 1024, 2048, and any larger size the
  backend exposes.
- At each size, play at least two decks, switch keylock, scratch, seek, loop,
  reverse, and change FX.
- Verify that blocks above 2048 are chunked without silence or buffer growth.

### Keylock and tempo

- Enable keylock on two to four decks and make large positive/negative tempo
  changes, including rapid backend or keylock toggles.
- Verify finite audio, plausible reported latency, bounded transition time,
  and no hard click or long dropout when a prepared pipeline activates.
- Keylock is a per-block routing decision, not part of the pipeline identity:
  toggling it, entering or leaving scratch, and loading a track must never
  rebuild a stretch pipeline, so none of them may stall or shift pitch.
- Toggle keylock, release a scratch, and load-then-play a track at every buffer
  size. With `BROCKDJ_AUDIO_DIAGNOSTICS=1`, `keylockSeedsInCallback` should stay
  at zero on small buffers; if it rises, the seed is landing in the callback and
  transitions will crackle.
- Pause loaded keylocked decks while other decks play/scratch. Paused decks must
  report zero active keylock latency/processing and resume through a bounded
  seed transition without a dropout.

### Scratch and cache recovery

- On the FLX10, audition slow forward/reverse drags, baby scratches, scribbles,
  chirps, rapid zero crossings, full-force 8x forward/reverse throws, fast
  backspin release, immediate re-grab, pre-roll, track start, loop wrap, paused
  scratch, and release into both playing and paused transport.
- Repeat at 128, 256, and 512 samples with 44.1/48/96 kHz devices and
  44.1/48/96/192 kHz tracks. Use bright transients and sustained high-frequency
  material so turn clicks, pitch stairs, and fold-back are not masked.
- Repeat while analysis runs, two/four decks play, keylock is active, and a
  track loads on another deck.
- Verify immediate visual response, plausible playhead, faded cache starvation
  rather than a callback stall, and zero callback disk/decoder counters.
- Record level-matched output for comparison with the chosen Serato/Rekordbox
  setup. Judge attack, turn point, fast-rate bandwidth, release tail, position
  feel, and latency separately; a generic "sounds better" result is not enough
  to tune estimator or DSP constants.
- With `BROCKDJ_AUDIO_DIAGNOSTICS=1`, confirm whether a click coincides with
  `scratchStarvation`, `scratchDropped`, or callback/device XRun growth before
  changing DSP or handoff behavior.
- Use `docs/architecture/scratch-engine-quality.md` as the implementation and
  measurement reference; update it whenever a scratch constant or owner moves.

### FX and mixer controls

- Rapidly change effect types, wet/dry, primary parameters, Sound Color FX,
  EQ, trim, channel faders, crossfader position/curve/assignment, master gain,
  and limiter state.
- Verify no crash, lasting silence, non-finite samples, discontinuous stale
  state, or callback violation counter.

### Cue, loop, slip, reverse, and sync

- Combine main cue hold/release, hot cues, saved loops, loop resize,
  beat-jumps, quantize, slip, reverse, sync enable/disable, master handoff,
  re-sync, scratch, and track replacement.
- Verify generation-correct state, audible/visual return from slip, stable
  beat/bar alignment, and no old-track cue/loop persistence.

## Device and controller scenarios

### Audio device changes

- Test ALSA and JACK on Linux, CoreAudio on macOS, and ASIO/WASAPI as available
  on Windows.
- Change sample rate, buffer, and stereo-pair routing during stopped and active
  playback. Hot-unplug/reconnect an external device.
- Verify controlled fallback/error reporting, no stale routing, no callback
  after endpoint destruction, and clean shutdown.
- On Linux, verify **RT SCHEDULER** reports `sched-fifo-active` or
  `already-realtime`; if it reports `permission-denied`, correct the installed
  PAM `rtprio` policy before using the system for a live set.

### FLX10, MIDI, and Link

- Run the detailed scratch matrix above, then verify FLX10 displays, waveform
  upload, keepalive, VU/LED feedback, deck assignment, and shutdown.
- Verify generic MIDI 7-/14-bit controls, duplicate suppression, and mixer
  state round trips.
- Test Link with zero, one, and multiple peers; leader/follower changes must
  not silently alter the internal sync-master policy.

## Analysis, library, and rendering scenarios

### Analysis replacement and shutdown

- Load a new track during analysis, eject/reload rapidly, and close the app
  during envelope, rhythm, key, and artifact phases.
- Verify no stale completion, wrong waveform/metadata, detached worker, crash,
  or teardown hang.

### Long-track progressive rendering

- Load long compressed files with no cache and observe first playable audio,
  progressive scrolling waveform, overview, beatgrid, and final cache publish.
- Verify immutable chunks become visible without flicker, excessive UI stalls,
  or a final old-generation replacement.

### Library/database/media I/O

- Exercise large folder scans, cover extraction, playlist/history/tag updates,
  analysis persistence, backup, quick/full checks, cancellation, and shutdown.
- Repeat with missing files, invalid images/audio, permission failures,
  disk-full simulation, and a slow/network filesystem where practical.
- Verify SQLite connections remain on their worker/owner thread and temporary
  artifacts are published or removed deterministically.

## UI and lifecycle scenarios

- Run desktop and AIO layouts at 1280x800 and 1024x600, plus scale 80–140%
  and minimum/maximum waveform zoom.
- Play and scratch while continuously resizing, minimizing, and restoring the
  window. Verify **UI RENDER** steps through `reduced`/`audio-first` under
  pressure, resize work settles after 120 ms, and callback/device XRun counts
  do not increase.
- Open/close settings, mapping editor, library, development controls, startup,
  status, and exit overlays. Check keyboard and touch input.
- Exercise the first and repeated transitions into four-deck mode, AIO settings,
  Source, desktop settings, and the mapping editor. These surfaces are loaded
  on demand; verify correct sizing/focus, no blank persistent state, and clean
  destruction when the owning surface closes.
- Close normally, during load/analysis/database work, and via SIGINT/SIGTERM on
  POSIX. Verify one shutdown request, no late callback, no repeated recovery
  warning, and no QML use-after-destruction warning.

For AIO/desktop-sensitive UI changes also run:

```bash
./scripts/desktop-regression-checklist.sh
```
