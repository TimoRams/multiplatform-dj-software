# MIDI Clock (out / in)

BrockDJ can send and receive MIDI beat clock (24 PPQN) so drum machines,
grooveboxes (Electribe, TR-8, Digitakt …) and DAWs follow the DJ tempo, or so
the sampler/sequencer follows external gear.

## Components

| File | Role |
| --- | --- |
| `src/controllers/midi/MidiClockScheduler.h` | Pure, header-only logic: `MidiClockScheduler` (out) and `MidiClockFollower` (in). No JUCE/Qt; unit tested in `tests/controllers/midi_clock_tests.cpp`. |
| `src/app/MidiClockController.{h,cpp}` | Device I/O, sender thread, persistence, QML properties (`midiClock` context property). |
| `src/audio/SamplerController.cpp` (`tick()`) | Clock arbiter: publishes the master clock and consumes the external clock. |
| `src/qml/settings/SettingsPanel.qml` | "MIDI Clock" section on the *DJ / Sync* page. |

## Clock source priority

`SamplerController::tick()` (125 Hz) picks the musical clock in this order:

1. **Ableton Link** (if enabled and peers connected)
2. **MIDI clock in** (if enabled and pulses are arriving)
3. **Master deck**
4. **Internal sequencer BPM**

The chosen clock is published to the MIDI output as a `ClockSnapshot`
(`bpm`, `beat`, `running`, `timestamp`, `generation`). When the source is MIDI
in, the snapshot is marked invalid so incoming clock is never echoed back.

## Output

- Dedicated `std::thread` (best-effort `SCHED_FIFO` priority 10), woken at most
  every 4 ms; pulses are sent with `MidiOutput::sendMessageNow`.
- Phase model: pulses are derived from the published beat position, not from
  free-running timers. Small errors are corrected smoothly (gain 0.08); errors
  above 0.25 beat or a new `generation` (seek, loop jump, source switch) cause a
  hard resync. Forward jumps never produce pulse bursts.
- Optional transport: START is quantized to the next bar (96 pulses), STOP is
  sent when playback stops, transport is disabled, the clock becomes invalid or
  the app exits.

## Input

- Pulses are timestamped in the MIDI callback; tempo is a least-squares fit over
  the last 96 pulses (min. 12), so jitter is averaged out.
- START resets the position, Song Position Pointer sets it (16ths × 6), both bump
  the generation so the sequencer resyncs. No pulses for 0.5 s ⇒ inactive.

## Settings keys (`UI/` prefix)

`midiClock/outputEnabled`, `midiClock/output` (device name), `midiClock/transport`,
`midiClock/inputEnabled`, `midiClock/input`.

## Known limitations

- MIDI clock in drives the sampler/sequencer clock only, not deck beat sync.
- External START does not auto-enable the sequencer.
- Deck loops trigger a hard resync on the output (no SPP is sent).

## Verification

- `ctest -R midi_clock` (scheduler/follower unit tests).
- Manual: enable output to `Midi Through Port-0`, run `aseqdump -p 14:0`;
  at 120 BPM ≈ 48 `Clock` events per second are expected.
