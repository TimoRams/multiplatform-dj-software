# Current Audio Routing

## Signal Flow

| Stage | Current code owner | Thread | Notes |
| --- | --- | --- | --- |
| File decode/read-ahead | `AudioPageCache` + `AudioCacheWorker` | worker | immutable pages after publication |
| Playback/loop/reverse | `CachedPlaybackAudioSource` | audio callback | cache misses fade; no decoder/disk entry |
| Transport intent/position | `DeckTransport` | Qt/control plus graph command | authoritative product snapshot |
| Scratch and varispeed | `ScratchDeckBridge`, `ScratchResampler`, `HermiteResamplingAudioSource` | audio callback | scratch cache and virtual turntable |
| Keylock/time stretch | `TimeStretchAudioSource` | worker prepares, callback activates | RubberBand switches at block boundary |
| Channel trim/polarity/color | `MixerDspSource` | audio callback | control values arrive through atomics/snapshots |
| EQ and filter | `MixerDspSource` | audio callback | prebuilt coefficient banks crossfade |
| Stop effects | `MixerDspSource` | audio callback | brake/backspin use preallocated circular buffers |
| PFL | `MixerDspSource` -> `DeckAudioGraph::preFaderBuffer` | audio callback | taken before fader processing |
| Channel fader | `MixerDspSource` | audio callback | smoothed channel gain |
| Deck FX and pad FX | `MixerDspSource`/`FxProcessor` | audio callback | currently after fader |
| Deck sum and cue/headphones | `DjMasterBus` | sole audio device callback | lifetime-guarded endpoints |
| Master/limiter/output routing | `DjMasterBus`, `AudioDeviceService` | callback/control configuration | limiter and output routing are global |

## Compared With the Desired Model

| Desired stage | Status | Evidence / implication |
| --- | --- | --- |
| Track/cache -> playback -> reverse/scratch | Correct | cache, playback source, and scratch bridge own this sequence |
| Time-stretch/keylock | Correct | prepared worker avoids callback setup |
| Trim -> channel EQ -> color FX | Wrong position | color FX runs after trim/polarity but before EQ/filter |
| Channel meter and PFL pre-fader | Correct | graph retains a pre-fader buffer |
| Channel fader -> crossfader | Unclear | crossfader is folded into `MixerControl` output, not a named DSP stage |
| Post-fader tails | Partially correct | deck/pad FX run after fader; master-tail owner is not separate |
| Master sum -> master FX -> master gain -> limiter | Missing/unclear | master gain and limiter are explicit; no distinct master-FX stage found |
| Master meter -> recording/output | Unclear | output routing exists; recording is not an explicit final stage |

## Ownership and Simplification

`ApplicationRuntime` owns `AudioDeviceService`, `AudioPageCache`, `DjMasterBus`, then the four deck
facades. Each `DjEngine` owns its `DeckAudioGraph` and `DeckTransport`; the graph is registered with
the bus through `IDeckAudioEndpoint`. Endpoint retirement drains active readers before graph lifetime
ends. This real-time boundary should stay.

Keep `DeckAudioGraph` as the only concrete graph owner, but later reduce direct facade access to its
implementation accessors (`mixer()`, `scratch()`, `timeStretch()`, `playback()`). Decide Color-FX
ordering in a dedicated audio-routing task because it changes sound.
