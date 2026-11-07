# Live functionality milestone (P0/P1)

This implements the first milestone of the XDJ-AZ / PRIME 4+ comparison roadmap.
The independent sampler and recording shortcut are covered by the subsequent
[sampler milestone](sampler-milestone.md). Touch Cue/Slicer, stems, network
sources, lighting and event zone/microphone processing remain later milestones.

## Audio and protection

- Auxiliary endpoints declare Master or Headphones destination. Library preview
  uses Headphones exclusively, before headphone gain/limiting, independently of
  the CUE/Master blend. It never feeds master, booth or the recording tap.
- A missing or overlapping headphone output rejects preview with a visible error.
  Preview level is adjustable. Split Cue puts cue/preview mono on the left and
  selected master mono on the right. Mono Master folds L/R equally before output
  and recording.
- Load lock defaults on. The deck facade guards local/external loads and manual
  eject; prepared results are checked again if the deck started during loading.
  Disable load lock explicitly in Live / Recording for an intentional exception.
- USB eject checks every partition on the physical drive for active decks and
  recording files. Native mount/eject remains subject to platform support.
- Unexpected removal of a partially cached imported track pauses that deck;
  fully cached imported tracks may continue. Other decks are unaffected.
- End warning defaults to 30 real seconds remaining, is disabled during loops or
  pause, and can be set to 0..120 seconds.

## Cue and loop data

Auto Cue scans up to 120 seconds for the first sample above a configurable
-80..-20 dB threshold (default -60), retaining a 2 ms lead-in. It is computed by
the loader worker. An existing main cue takes precedence. Disabling Auto Cue
preserves start-of-file behavior for tracks without saved cues.

Memory cues support create, rename, delete and previous/next navigation (32
local entries per track). Saved-loop arming is separate from a running loop.
Entering an armed region during forward play activates it. Explicitly leaving a
loop does not immediately re-enter it while still inside the same region.

Supplemental data is stored in the local library Meta table under a source-aware
performance key. Local tracks retain their existing main/hot-cue and saved-loop
storage; imported tracks store their edited cue/loop snapshots locally, never in
rekordbox databases. Per-track data is loaded on the owner/control thread.

Mappable command IDs are `deckA_memory_store`, `deckA_memory_previous`,
`deckA_memory_next`, `deckA_active_loop_arm1` through `deckA_active_loop_arm8`
(also B/C/D), `recording_toggle` and `recording_track_mark`.
No unverified FLX10 MIDI note assignments are added. Names and loop arming are
available in the on-screen Live / Recording page.

## Recorder and lifetime

The recorder is owned by AudioEngine, exposed as setRecorder, and stopped/joined
before the engine is retired. The audio callback copies final stereo master
frames into a preallocated SPSC ring (524288 frames). WAV conversion, file I/O,
flush and file rotation run on the writer thread. Ring exhaustion stops recording
and reports an error; playback never waits for storage. Device/rate changes stop
rather than corrupting the rate interpretation. WAV output is 24-bit stereo PCM;
values beyond full scale are clipped to PCM range.

Files use timestamp plus unique suffix. Track Mark and automatic splitting before
2 GiB start numbered parts. Headers are refreshed periodically and finalized on
stop. Disk errors retain the complete frames that could be written; this is not
an assurance against every filesystem or power-loss failure. The recording tap
contains only signals that actually pass through the software mixer.

## Validation

Build the application plus `BrockDJ_set_recorder_tests`,
`BrockDJ_deck_audio_graph_tests`, `BrockDJ_qml_component_tests`,
`BrockDJ_track_loader_tests`, `BrockDJ_cue_loop_controller_tests` and
`BrockDJ_master_bus_tests`, then run:

```sh
ctest --test-dir build -R '^(ci_runtime_smoke|startup_close_smoke|startup_early_close_smoke|set_recorder|deck_audio_graph|track_loader|cue_loop_controller|qml_component|master_bus|audio_routing_contract)$' --output-on-failure
```

Recorder tests inspect actual PCM values and WAV sizes, track marks, restart,
rate changes, invalid storage and ring overflow. Graph tests verify silent
master/booth/recording taps during preview, headphone gain and split cue. Loader
tests cover audible onset and Auto Cue disablement. App preflight loads a real
track and verifies facade load/eject protection, memory-cue CRUD, external-track
isolation, supplemental loop/cue restoration, automatic loop entry and manual
exit. Master-bus tests check mono output against both the cue tap and meter,
including cancellation of opposite-polarity channels.

The four previously reproduced envelope holes during the +/-16% tempo sweep at
64/256-frame blocks (44.1/48 kHz) are resolved by the hop-consistent pitch map and
denser overlap described in `keylock-stability.md`. Sweep limits remain unchanged.
The prior `dj_engine_api_contract` invalidation check has been updated to the
documented electronic-analysis policy: reuse valid waveform caches and merge
validated results into loaded decks without replacing their waveforms. The
`analysis_lifetime` test verifies actual cache reuse during explicit reanalysis.

Hardware acceptance still requires real USB removal/eject on Linux, headphone
routing on the target audio interface, extended four-deck recording to removable
storage, and measured render/audio timing. No hardware performance guarantee is
inferred from software-only tests.
