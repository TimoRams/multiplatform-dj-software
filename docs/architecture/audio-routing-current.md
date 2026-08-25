# Current Audio Routing

## Signal Flow

| Stage | Owner | Thread | Notes |
| --- | --- | --- | --- |
| File decode/read-ahead | `AudioPageCache` + `AudioCacheWorker` | worker | immutable pages after publication |
| Playback/loop/reverse | `CachedPlaybackAudioSource` + `DeckTransport` | audio/control command | cache-miss recovery crossfades |
| Direct/Keylock/Scratch | `RenderModeRouter` | audio callback | one mode per deck, block-boundary handoff |
| Time stretch | `TimeStretchProcessor` | worker/audio callback | prepared Signalsmith (default) or Rubber Band pipeline |
| Trim/EQ/filter/color/insert FX | `DeckChannelProcessor` | audio callback | one coherent parameter snapshot per block |
| Channel meter and PFL | `DeckChannelProcessor` | audio callback | pre-fader taps |
| Channel fader | `DeckChannelProcessor` | audio callback | smoothed independently from crossfader |
| Crossfader/deck sum/tails | `MasterMixer` | audio callback | typed post-fader wet returns |
| Master FX/gain/limiter/meter | `MasterMixer` | audio callback | each stage applied exactly once |
| Cue/master headphones | `HeadphoneBus` | audio callback | independent gain and safety limiter |
| Master/Booth/headphone outputs | `AudioOutputRouter` | audio callback | missing assignments remain silent |
| Device lifecycle | `AudioDeviceService` | control | no automatic or failure fallback |
| Library preview | `AudioPageCache` -> pre-master sum | worker/audio callback | lock-free cached reader, no callback decode |

`AudioEngine` owns four `DeckAudioPipeline` instances and is the sole hardware callback.
It snapshots global parameters once per callback, renders each deck, creates the canonical
MasterTap, mixes headphones, and hands logical buses to `AudioOutputRouter`.

The Scratch branch consumes an exact cumulative hand position plus a separately
filtered velocity. `ScratchResampler` joins controller snapshots with a C2
quintic trajectory, follows it with a bounded position/velocity servo, and reads
immutable cache pages through a 64-tap variable-cutoff sinc kernel. Release
inherits the rate actually rendered by the tracker and interpolates each coast
target as a bounded full-callback trajectory. The complete FLX10-to-handoff contract and
quality gates live in [scratch-engine-quality.md](scratch-engine-quality.md).

Master and Booth are separate physical assignments of the same post-limiter MasterTap and
therefore share software Master Gain. Recording reads that same tap. A separate Booth level,
where present, is hardware-owned.

## Realtime Boundary

The callback uses preallocated buffers, coherent snapshots, bounded lock-free commands,
cache-page reads, and `juce::ScopedNoDenormals`. It performs no file I/O, decoder work,
allocation, blocking lock, Qt signal, logging, or device reconfiguration. Track and preview
source replacement is protected by the pipeline callback gate. Decoder readers
are detached atomically and normally retired by the cache worker; explicit
eject/shutdown waits for active decoder leases and file closure.
