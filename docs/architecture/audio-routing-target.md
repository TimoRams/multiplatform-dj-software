# Target Audio Routing Contract

Status: binding target architecture

This document is the authoritative routing contract for BrockDJ. The current
implementation is inventoried separately in `audio-routing-current.md`. Migration must
follow the phased plan and must not create a second audio architecture beside the current
one.

## 1. Non-negotiable invariant

BrockDJ has exactly one canonical live-audio path:

```text
Cache -> DeckAudioPipeline -> MasterMixer -> HeadphoneBus / MasterTap
      -> AudioOutputRouter -> Hardware
```

Scratch, reverse, keylock, preview, development tools, and specific controllers are modes
or clients of this path. They never own a separate DSP path. UI, MIDI, and HID code may
publish control state and commands, but may not process or route audio.

## 2. Code-level topology

`src/audio/AudioRouting.h` is the only code declaration of the fixed topology. It contains
types and constants only. The following statement is intentionally identical to the
statement in that header:

```text
Canonical signal flow:

DeckAudioPipeline:
  Cache -> PlaybackReader -> Transport -> RenderModeRouter
  -> Trim -> ChannelEQ -> ColorFX -> DeckFX(PreFaderInsert)
  -> ChannelMeter -> PFLTap -> ChannelFader -> CrossfaderGain -> DeckProgramOutput

MasterMixer:
  Sum(DeckProgramOutputs) + Sum(DeckFX PostFaderTail wet returns)
  -> MasterFX -> MasterGain -> Limiter -> MasterMeter -> MasterTap

HeadphoneBus:
  Sum(PFLTaps) -> CueSum; (CueSum, Master) -> ConstantPowerBlend
  -> HeadphoneGain -> HeadphoneLimiter -> HeadphoneOutput
```

The header may grow only after this document changes. `AudioEngine`,
`DeckAudioPipeline`, `MasterMixer`, `HeadphoneBus`, and `AudioOutputRouter` must use its
types and constants rather than declaring local copies.

## 3. AudioEngine

`AudioEngine` is the only entry point used by the hardware audio callback. It owns:

- four `DeckAudioPipeline` instances;
- one `MasterMixer`;
- one `HeadphoneBus`;
- one `AudioOutputRouter`;
- all callback buffers prepared to the maximum supported block size and channel count;
- the coherent parameter snapshot consumed for the current block;
- real-time-safe meter publication.

It does not own track metadata, library state, cover art, QML state, controller mappings,
waveform data, file dialogs, decoder setup, or device configuration.

One callback block is processed in this order:

```cpp
void AudioEngine::processBlock(AudioBuffer& deviceOutput)
{
    ScopedNoDenormals noDenormals;
    const AudioParameters parameters = parameterStore.snapshot();

    deckA.render(parameters, blockContext);
    deckB.render(parameters, blockContext);
    deckC.render(parameters, blockContext);
    deckD.render(parameters, blockContext);

    masterMixer.mix(deckOutputs, tailReturns, masterBuffer);
    headphoneBus.mix(deckPflBuffers, masterBuffer, headphoneBuffer);
    outputRouter.write(masterBuffer, headphoneBuffer, boothBuffer, deviceOutput);

    meters.publish();
}
```

The pseudocode specifies ownership and ordering, not concrete API spelling for a migration
phase.

## 4. DeckAudioPipeline

Every deck has one complete pipeline:

```text
AudioPageCache
  -> PlaybackReader
  -> Transport (Play/Pause/Seek/Loop/Slip/Reverse)
  -> RenderModeRouter (Direct | Keylock | Scratch)
  -> Trim
  -> ChannelEQ (Low/Mid/High)
  -> ColorFX / Filter
  -> DeckFX (PreFaderInsert; PostFaderTail input is also taken here)
  -> ChannelMeter
  -> PFLTap
  -> ChannelFader
  -> CrossfaderGain supplied by MasterMixer
  -> DeckProgramOutput
```

There are no controller-specific branches and no track-length-dependent branches. The
pipeline receives cache data and control snapshots; it never opens or reads a file.

### 4.1 PlaybackReader and cache boundary

The playback reader accesses audio only through a non-blocking cache read:

```cpp
cache.read(position, output, sampleCount);
```

A cache miss in the callback must not wait, allocate, lock, decode, or perform I/O. It
produces a short smoothed hold or silence, sets an underflow flag, and publishes a
highest-priority bounded worker request. When real audio becomes available, output returns
through a 32 to 128 sample crossfade. A hard switch from hold or silence to real audio is
forbidden.

### 4.2 Transport positions

Transport and rendering distinguish four related positions:

- `sourcePosition`: the authoritative source location requested from the cache;
- `audiblePosition`: the source location represented by the samples actually emitted;
- `slipPosition`: the uninterrupted background transport position while slip is active;
- `scratchPosition`: the cursor owned by the scratch processor while Scratch mode renders.

They are not four independent clocks. At mode boundaries the audio callback transfers the
authoritative rendered cursor in the same block in which the mode changes. UI playheads and
handoffs use `audiblePosition`, not a stale hand target or grab anchor.

### 4.3 Render modes

The `RenderModeRouter` owns exactly one active mode:

- `Direct`: cache playback at variable rate; pitch follows speed;
- `Keylock`: cache playback through `TimeStretchProcessor`; pitch is preserved;
- `Scratch`: cache playback through `ScratchResampler`.

Entering Scratch positions the scratch processor at the audible cursor and crossfades to
Scratch over 32 to 128 samples. Leaving Scratch transfers the final scratch cursor to
Transport, reseeds the stretcher when Keylock is the destination, and
crossfades to Direct or Keylock over 32 to 128 samples. Cursor transfer, processor reset,
mode switch, and crossfade arming happen at one audio-block boundary.

Within Scratch, cumulative controller travel owns absolute position and a
separately filtered velocity predicts inter-event movement. The callback joins
successive hand states with a C2 trajectory; it must not hold one velocity for a
whole block or snap the rendered rate to a raw release estimate. Variable-rate
forward and reverse reads are source-domain band-limited for the complete
track/device sample-rate ratio. Detailed current limits and regression evidence
are normative in `scratch-engine-quality.md`.

Reseeding hands the stretcher the audio the listener just heard so it does not start from
its own latency worth of silence. It costs several FFT frames, so it runs on the
`TimeStretchProcessor` worker while the deck keeps playing on the Direct path; the
callback only takes the seed itself when the block is long enough to absorb it with room
to spare. Neither a keylock toggle, a scratch release, nor a track load rebuilds a
pipeline — keylock is decided per block, so there is never a stretch where the deck plays
unlocked while waiting for a worker.

Reverse is a Transport direction, not a fourth render engine. Keylock activation belongs
to the router; its tempo value belongs to deck playback.

## 5. Deck and master effects

The only placement types are declared by `FxPlacement`:

- `PreFaderInsert`: Flanger, Phaser, Crush, Trans, Gate, and filter effects. Dry and wet
  processing remains in the channel before ChannelMeter, PFLTap, and ChannelFader.
- `PostFaderTail`: Echo, Reverb, Spiral, and Dub Echo. Input is captured before the fader;
  the wet return is summed after channel and crossfader gains.
- `Master`: effects that process the complete master mix.

An effect produces a dry output and a wet output. The mixer routes the wet output according
to its placement. Complete duplicate Dry/Wet/PostFader/BeatFx pipelines are forbidden.

Post-fader delay and reverb state has a lifecycle independent of the deck track. A track
change or `ResetDeck` command stops new input from that deck but does not clear or truncate
an active tail. The tail state continues to render into the MasterMixer until it decays or
is explicitly reset by a dedicated non-clicking FX command.

## 6. Tap and meter positions

Tap and meter placement is fixed:

- `ChannelMeter` measures the channel after Trim, EQ, ColorFX, and pre-fader DeckFX, but
  before ChannelFader and CrossfaderGain.
- `PFLTap` is immediately after ChannelMeter and before ChannelFader. It remains audible
  with the channel fader closed and is unaffected by crossfader, MasterGain, or MasterFX.
- Post-fader tail returns do not feed the deck PFL tap or ChannelMeter. They feed the master
  sum and are visible at the MasterMeter.
- `MasterMeter` is after the limiter. It publishes final post-limiter peak and also exposes
  diagnostic pre-limiter peak and limiter gain reduction.
- `MasterTap` is after limiter and MasterMeter and is the single source for recording,
  Master output, Booth output, and the master side of the cue/master blend.

Meters publish lock-free snapshots. Meter publication never emits Qt signals from the
audio callback.

## 7. MasterMixer

`MasterMixer` owns:

- summing of all four deck program outputs;
- summing of all independent post-fader tail wet returns;
- crossfader value, curve, and per-deck assignment;
- MasterFX, MasterGain, and the one final limiter;
- pre-limiter peak, limiter gain reduction, final peak, MasterMeter, and MasterTap.

Crossfader assignments use `CrossfaderAssignment::{A, Thru,B}`. A `Thru` deck is not
affected by the crossfader. Curves use
`CrossfaderCurve::{ConstantPower,Smooth,Scratch}`. ConstantPower and Smooth are smoothed
within the block. Scratch is sample-accurate or uses a separately bounded, substantially
shorter smoothing time so a cut is not softened by the normal fader ramp.

The crossfader, MasterGain, and limiter are each applied exactly once. Recording consumes
MasterTap and therefore matches the audible master signal bit-for-bit before physical
device conversion.

## 8. HeadphoneBus

The headphone path is:

```text
PFL(A) + PFL(B) + PFL(C) + PFL(D)
  -> CueSum
  -> ConstantPowerBlend(CueSum, MasterTap)
  -> HeadphoneGain
  -> HeadphoneLimiter
  -> HeadphoneOutput
```

PFL selection and cue/master blend belong to `HeadphoneBus`. HeadphoneGain affects neither
MasterTap, recording, Booth, Master output, nor ChannelMeter.

## 9. AudioOutputRouter

The engine exposes only the logical buses declared in `AudioRouting.h`:

```text
Master, Headphones, Booth, DeckA, DeckB, DeckC, DeckD
```

Only `AudioOutputRouter` maps these buses to physical channels. Controller mappings and DSP
objects never contain physical channel numbers.

The output policy is deterministic:

1. A logical bus is audible only when assigned to an available physical device/channel
   pair.
2. An unavailable or unassigned bus is silent and is shown as `No device` in device
   selection.
3. There is no automatic fold-down, no automatic routing to Master, and no automatic
   second-device fallback.
4. In particular, missing Headphones never leaks PFL into Master; missing Booth and deck
   stem outputs remain silent.
5. Master is the required primary bus. If Master has no valid assignment, device output is
   silent and the control side reports the invalid assignment.

Booth is a physically separate output of the same MasterTap and follows software
MasterGain. BrockDJ has no independent software Booth gain. A mixer may provide a separate
hardware or analog Booth level after the software output; that hardware state is outside
the audio engine.

## 10. Parameter and command transfer

The only control path into DSP is:

```text
QML / MIDI / HID -> ControlState -> AudioParameterSnapshot -> Audio callback
```

`parameterStore.snapshot()` returns one coherent struct per block through double buffering
and one atomic published index or pointer. A group of unrelated per-field atomic loads is
not a valid snapshot because it can mix generations.

Trim, ChannelFader, normal Crossfader curves, Filter, EQ, effect wet values, MasterGain,
HeadphoneGain, and other sensitive continuous values are smoothed inside the block. The
Scratch crossfader curve follows the faster rule in section 7.

Transport uses a fixed-capacity lock-free command queue with these command categories:

```cpp
enum class AudioCommandType {
    Play, Pause, Seek, SetLoop, ClearLoop,
    BeginScratch, EndScratch, ResetDeck
};
```

The queue never blocks. On overflow it drops the oldest pending command, accepts the newest
command, increments an atomic overflow counter, and sets an atomic diagnostic flag. The
control thread consumes the flag and may log it; the audio callback never logs. Command
payloads are complete generation-tagged snapshots so the callback cannot combine fields
from different commands.

## 11. Real-time rules

The callback may:

- read and write prepared audio buffers;
- run prepared DSP processors;
- consume coherent snapshots and bounded lock-free commands;
- perform non-blocking cache page reads;
- publish atomic meter and diagnostic values.

The callback must never:

- allocate or free memory;
- resize containers;
- wait for a mutex or condition variable;
- open, decode, read, or write files;
- access SQLite, QML objects, cover art, metadata, or waveform analysis;
- emit Qt signals or write logs;
- start threads or reconfigure devices.

Every callback begins with FTZ/DAZ denormal protection, using
`juce::ScopedNoDenormals` or an equivalent scope. All buffers and DSP state are allocated,
prepared, and touched before playback begins.

On Linux the device callback thread requests `SCHED_FIFO` with an installation-appropriate
priority and reports failure outside the callback. Required memory is prefaulted. Where the
deployment permits it, the audio process locks its working set with `mlockall`; failure is
reported but never handled by blocking or logging in the callback. Scheduling and memory
lock failures must be distinguished from DSP/cache underflows in diagnostics.

## 12. State ownership

Each state has one authoritative owner:

| State | Owner |
| --- | --- |
| Track and cache handle | Deck Playback |
| Source position | Transport |
| Loop and slip | Transport |
| Scratch position | Scratch Processor |
| Active Direct/Keylock/Scratch mode | RenderModeRouter |
| Keylock tempo value | Deck Playback |
| Trim, EQ, Filter, ColorFX | Deck Channel |
| DeckFX and independent tail lifecycle | Deck Channel / independent FX state |
| ChannelFader | Deck Channel |
| Crossfader value, curve, assignment | MasterMixer |
| MasterGain and limiter | MasterMixer |
| PFL selection | HeadphoneBus |
| Cue/master blend and HeadphoneGain | HeadphoneBus |
| Physical channel assignment | AudioOutputRouter |
| Audio device lifecycle | AudioDeviceService |
| Waveform data | WaveformStore |
| Track metadata | Track Model |
| UI display state | UI Facade |

Booth has no independent software gain state. It is a separate physical rendering of
MasterTap after MasterGain. Any separate hardware Booth gain remains outside BrockDJ.

`DjEngine` is a facade: it displays snapshots and publishes commands but owns none of the
audio states above. The same value may not independently exist as truth in QML,
`MixerControl`, `AudioParameterStore`, `DjEngine`, `DeckAudioPipeline`, and
`DeckChannelProcessor`.

## 13. Migration invariants

The phased migration must preserve these externally testable outcomes:

1. Every deck has one audio path and controllers have no DSP path.
2. Cache and DSP remain separated; cache misses resume through a crossfade.
3. Direct, Keylock, and Scratch are modes of one deck pipeline.
4. ChannelMeter and PFL are pre-fader; PFL works with a closed fader.
5. Crossfader, MasterGain, and limiter are each applied exactly once.
6. Post-fader tails survive ChannelFader close, track changes, and `ResetDeck`.
7. Recording, Master, and Booth originate at the same MasterTap.
8. Headphones remain independent of Master and recording.
9. Master and Booth use separate physical assignments while sharing software MasterGain.
10. No callback locks, allocations, file operations, decoder calls, Qt signals, or logs.
11. Every parameter has the single owner declared in section 12.
12. Obsolete wrappers and alternative direct paths are deleted as their function migrates.
