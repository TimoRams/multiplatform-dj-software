# Audio Routing Refactor Status

The production path now follows the canonical contract:

`Cache -> DeckAudioPipeline -> MasterMixer -> MasterTap -> AudioOutputRouter -> Hardware`

`DeckAudioPipeline` selects Direct, Keylock, or Scratch and publishes separate pre-fader
program, PFL, and post-fader tail buffers. `MasterMixer` owns channel summing,
crossfader policy, Master Gain, and the one final master limiter. `HeadphoneBus` owns PFL,
cue/master blend, Headphone Gain, and its safety limiter. `AudioEngine` is the only audio
device callback and snapshots all routing parameters once per block.

Master, Booth, and recording observe the same post-limiter MasterTap. Booth therefore
follows software Master Gain; a separate Booth level is hardware-owned. Unassigned or
unavailable devices and output pairs render silence. The device service does not select
or retry a fallback device.

Control-to-audio parameters use a coherent triple-buffer mailbox. Transport commands use
a bounded SPSC queue with an explicit drop-newest counter; seek commands are coalesced by
generation so a controller flood cannot lose the latest requested position. Audio
callbacks use preallocated buffers and never emit UI signals, log output, file I/O, or
decoder work. Library preview audio uses the same page cache and a lock-free reader handoff
before joining the canonical pre-master path; format probing stays on a worker thread.

The executable contract is `audio_routing_contract`. Related realtime and continuity
coverage lives in `master_bus`, `mixer_dsp`, `deck_audio_graph`, `deck_transport`,
`scratch_cache`, and `time_stretch`.

The phase-10 workload runs for a bounded duration with four real deck pipelines, mixed
44.1/96 kHz sources, simultaneous Keylock, Scratch, per-deck FX, Master FX, PFL, Master,
and alternating 64/128-sample callbacks:

```sh
BROCKDJ_AUDIO_SOAK_SECONDS=7200 BROCKDJ_AUDIO_SOAK_STRICT=1 \
  build-tests/BrockDJ_deck_audio_graph_tests
```

Strict mode fails on callback budget misses, sustained starvation, or any realtime/cache-I/O
diagnostic counter and therefore must run with the same realtime scheduling as the hardware
callback. Run that command under the project's allocation profiler or realtime sanitizer
for the required independent callback-allocation verification; the normal CTest keeps the
same workload short enough for development.
