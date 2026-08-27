# Scratch Engine Quality and FLX10 Path

Date: 2026-08-24

This is the living reference for physical platter input, scratch motion,
band-limited reverse playback, release inertia, and the cache window used by
that path. It records the current design and measurable quality gates. It does
not claim subjective parity with a commercial product: that last step requires
repeatable listening and latency tests on a physical FLX10 and audio device.

## End-to-end path

```text
FLX10 MIDI packet timestamp + relative platter ticks
    -> Flx10RealtimeScratchIngress
       -> JogSpeedEstimator (signed velocity)
       -> cumulative tick travel (absolute hand position)
    -> RealtimeScratchInput coherent atomic snapshot
    -> RenderModeRouter in the audio callback
       -> event-age compensation and short stale-velocity decay
       -> ScratchResampler position tracker
          -> C2 quintic hand trajectory
          -> critically damped position/velocity servo
          -> acceleration, jerk, speed and runaway safety bounds
       -> 64-tap variable-cutoff sinc reader over AudioPageCache
    -> ScratchController release coast
       -> bounded full-block rate trajectory
    -> cursor handoff and short Direct/Keylock crossfade
```

Position and velocity deliberately have different jobs. Every FLX10 tick is
integrated one-to-one into `cumulativeDeltaSeconds`, so sticker position cannot
drift with estimator tuning. Velocity predicts where the hand moves between
events and shapes the sound; it is allowed to be filtered without changing
total platter travel.

## Input and estimator

`JogSpeedEstimator` keeps a fixed-capacity history of original packet receive
timestamps and cumulative ticks.

- The semantic FLX10 calibration is 12,750 ticks per revolution at 33 1/3 RPM.
- Sub-0.35 ms packets are treated as one drained USB batch; their ticks are
  retained but their scheduler gap is not treated as physical acceleration.
- A same-direction rate window carries at least eight ticks and 3.5 ms once
  available. The tick rule keeps slow drags precise; the time floor prevents a
  fast 6x-8x throw from becoming a quotient of one jittered USB frame.
- A real sign change resets the velocity history immediately. The first reverse
  frame therefore changes sign instead of waiting for the averaging window.
- A 60 ms stale/time bound prevents old movement from surviving a stopped hand.

The native ingress publishes before the later Qt/MIDI facade path, so display or
UI work cannot batch the audio trajectory. `RealtimeScratchInput::tryRead()`
makes one non-spinning coherent read attempt; the callback retains its previous
valid snapshot if it catches a writer in progress.

## Audio trajectory

The callback reconstructs the absolute hand target from the grab anchor plus
cumulative tick travel. It supplies the measured event age and velocity to
`ScratchResampler::processScratchTracking()`.

Each audio block joins the persistent prior hand state to the newest predicted
position and velocity with a quintic Hermite curve whose endpoint acceleration
is zero. Adjacent blocks are therefore continuous in position, velocity, and
acceleration (C2). The previous block-constant velocity and linear position
correction were only C0/C1 at callback boundaries; the servo converted those
hidden edges into the metallic block-rate chirp heard during fast scratching.

A 56 Hz critically damped servo follows the moving reference while absolute
position remains authoritative. Safety shaping is continuous rather than a
hard clip:

| Bound | Current value | Purpose |
| --- | ---: | --- |
| Physical command/release range | +/-8x | one range across FLX10 ingress, controller and release |
| Private tracker catch-up range | +/-10x normalized | recovers finite event/startup lag while the hand is already at 8x |
| Tracker acceleration | 1,500x/s | rounds an impossible position/timestamp impulse |
| Tracker jerk | 1,000,000x/s^2 | removes a single-sample acceleration edge |
| Speed-bound approach | 1.5 ms | reaches the safety limit without a hard clamp corner |
| Tracker bandwidth | 56 Hz, critically damped | low lag without position overshoot from an underdamped loop |
| Input-age compensation | 0-30 ms | bridges native jog and lower-rate screen input causally |
| Runaway envelope decay | 250 ms | keeps the emergency bound open through a zero crossing |

The 10x value is not a second controller sensitivity. It is internal headroom:
without it, any finite acceleration from rest leaves a permanent position gap
when an 8x hand command is already equal to the renderer's hard maximum.

## Band-limited reader and cache window

The scratch reader supports true forward and reverse variable-rate playback.
It uses a 64-tap, 256-phase, four-term Blackman-Harris sinc kernel. Thirty-two
cutoff bands are spaced uniformly in cutoff frequency and interpolated per
sample. The source-domain cutoff is `0.90 / abs(sourceSamplesPerOutputSample)`;
filtering after resampling would be too late to remove folded content.

The prepared table covers an absolute source/output rate of 64. This distinction
matters: normal 1x playback of a 192 kHz file on a 48 kHz device already advances
four source samples per output sample, before scratch speed is applied.
The fixed coefficients are built once at the device/control boundary and shared
immutably by every deck; track loading never rebuilds them.

The local PCM window is allocated during `prepareToPlay()`, not in the callback.
It reserves half a second at up to 192 kHz plus a device-block/rate floor. Window
margins are measured in track samples, predict the actual direction of the next
block, and load both sides for a reversal. This avoids the former failure where
a reverse 8x block biased most of its window ahead of the head and faded out
behind it. Ordinary <=192 kHz track installation uses the existing reservation;
only a larger source can grow it, behind `DeckAudioPipeline`'s callback gate.

The callback only copies immutable resident `AudioPageCache` pages. A miss
requests bounded worker work and uses the fixed 128-sample starvation fade; it
never decodes, reads a file, allocates, or waits.

## Release and handoff

Touch-up is decided after one final position-tracking block. The release starts
from the velocity that block actually rendered; it is never snapped to the raw
MIDI quotient. That "actually rendered" velocity is itself seeded from
`ScratchController::submitHandDelta`'s per-event `deltaTrackSec/dtSec`
estimate for any input (mouse drag included) that supplies no measured rate of
its own. `dtSec` there is floored to
`ScratchControllerConfig::minHandRateEstimateDtSec` (2 ms) before the division:
an on-screen drag reports through the Qt event queue, and two events can land
a fraction of a millisecond apart — most commonly the final move immediately
before button-up, or any move right after the render thread catches up from a
stall. Dividing a real (if tiny) position delta by a near-zero interval used to
inflate the quotient straight to the +/-8x clamp regardless of its true sign,
which then dominated the reversal-damping blend and, when it was the last
sample before release, became the release direction — heard as the platter
throwing briefly backward at the moment of release, or as a smaller "hin und
her" jitter mid-drag whenever such a sample landed inside a scratch. Position
is unaffected by the floor: the full delta is always integrated into
`m_handPositionSec` regardless of how implausible its implied rate is.
`ScratchController` then publishes one exponential coast target
per callback with a 220 ms return constant. `ScratchResampler::processBlock()`
uses the prior rate and new target as a complete block trajectory, then applies
the same time-based acceleration and jerk limits as tracking. This prevents both
callback-frequency pitch stairs and a short-buffer impulse when the newest
physical release estimate is faster than the final rendered tracking rate. Once
the coast reaches its handoff threshold, the rendered cursor transfers to Direct
or Keylock and the existing short tail crossfade masks the reader change.

## Measured regression points

All figures below were recorded at 48 kHz on the audit host. They are regression
orientation points, not psychoacoustic proof.

| Measurement | Earlier path | Current path |
| --- | ---: | ---: |
| 10 kHz source at 4x, output RMS | 0.000509 (32 taps) | 0.000000151 |
| Decaying 10x reverse, 18 kHz source | not covered | 0.000000047 RMS at 128/512 |
| Passband amplitude, 4 kHz at 4x | 0.801 (32 taps) | 0.946 |
| Passband amplitude, 1.8 kHz at 8x | not covered | 0.848 |
| 192 kHz/48 kHz device, 60 kHz source RMS | wrong absolute-rate cutoff was possible | 0.0000127 versus 0.1767 legal-passband RMS |
| Baby scratch acceleration edge, 512 | 0.008787x/sample^2 (linear reference) | 0.000103x/sample^2 |
| Alternating +/-1x edge, 512 | 0.021141x/sample^2 | 0.000207x/sample^2 |
| Decaying backspin edge, 512 | 0.003656x/sample^2 | 0.000039x/sample^2 |
| 8x steady hardware trajectory | previously clamped/could retain startup gap | 8.000x mean, 1.0002 travel ratio |
| 6x USB-jitter trajectory, 128/256/512 | one-frame quotient oscillated | 5.9997-6.0000x mean, 1.0006 travel ratio |
| 512-sample stereo reader microbenchmark | 66 us (32 taps) | 115-128 us observed |

The isolated kernel costs roughly 49-62 us more for a 512-sample block, at most
about 0.6% of that block's 10.67 ms deadline on the audit host. The complete
active deck graph measured about 90 us in the final soak run; paused or normally
playing decks do not run the scratch kernel. The quality increase is intentional;
do not reduce taps or cutoff coverage without both spectral and physical
listening A/B.

Automated gates cover:

- forward/reverse slow drag, baby scratch, alternating turns, 4x and 8x throws;
- 8x release coast at 128, 256, and 512 samples;
- a deliberately mismatched 2x-rendered/8x-physical release at 128 and 512;
- full FLX10 estimator-to-audio motion with alternating 0.7/1.3 ms receive jitter;
- acceleration and jerk bounds for +/-6x turns down to an intentionally
  non-physical eight-millisecond stress case;
- passband and stopband response through 10x, in both directions;
- 192 kHz source alias rejection on a 48 kHz output device;
- cache starvation, generation changes, loop/track edges and callback-I/O zeros.

## Physical validation still required

For any scratch-quality sign-off, use a real FLX10 and record device/backend,
sample rate, buffer, track rate, XRun count, and an audio capture. At minimum:

1. Test 128, 256, and 512 samples at 44.1/48/96 kHz device rates.
2. Use 44.1, 48, 96, and 192 kHz tracks with bright transients and sustained
   high-frequency material.
3. Audition slow drags, baby scratches, scribbles, chirps, rapid reversals, a
   full-force forward/reverse throw, 8x backspin release, and immediate re-grab.
4. Repeat with two and four playing decks, keylock on/off, analysis active, and
   a track loading on another deck.
5. Compare level-matched captures against the chosen Serato/Rekordbox setup.
   Judge attack shape, turn-point click/chirp, pitch stairing, high-speed alias,
   position feel, release tail, and end-to-end latency separately.

Automated output can prove continuity, bandwidth, exact travel and callback
safety. It cannot prove FLX10 firmware timing, audio-driver jitter, hand feel, or
subjective parity. Those remain explicit open risks, not hidden assumptions.

## Change rules and commands

- Do not tune the velocity filter by changing cumulative tick travel.
- Do not change the 8x input, 10x catch-up, or 64x absolute filter limits in
  isolation.
- Do not replace full-block coast interpolation with a fixed per-sample slew.
- Do not rebuild sinc tables or grow the source window in the callback.
- Any trajectory change must report both maximum rate step and its delta;
  checking velocity continuity alone misses callback-rate acceleration buzz.

```bash
cmake --build build-tests --target \
  BrockDJ_scratch_motion_tests \
  BrockDJ_scratch_cache_tests \
  BrockDJ_flx10_jog_routing_tests \
  BrockDJ_deck_audio_graph_tests -j2

BROCKDJ_SCRATCH_VERBOSE=1 ./build-tests/BrockDJ_scratch_motion_tests
BROCKDJ_SCRATCH_BENCHMARK=1 ./build-tests/BrockDJ_scratch_cache_tests
BROCKDJ_CACHE_STRESS_SECONDS=2 BROCKDJ_CACHE_STRESS_TRACK_SECONDS=60 \
  ./build-tests/BrockDJ_audio_cache_stress
BROCKDJ_AUDIO_SOAK_SECONDS=5 BROCKDJ_AUDIO_SOAK_STRICT=1 \
  ./build-tests/BrockDJ_deck_audio_graph_tests
ctest --test-dir build-tests --output-on-failure -j2
```

## External implementation references

These references informed the design review; BrockDJ does not copy their
algorithms verbatim.

- Mixxx's official linear scaler interpolates playback rate over an audio
  buffer instead of applying one callback-sized step:
  <https://raw.githubusercontent.com/mixxxdj/mixxx/main/src/engine/bufferscalers/enginebufferscalelinear.cpp>
- Mixxx issue 6951 documents speed oscillation caused by wheel polling and
  audio-buffer timing jitter, including timestamp-aware mitigation:
  <https://github.com/mixxxdj/mixxx/issues/6951>
- Mixxx's MIDI scripting documentation describes its alpha-beta scratch input
  filter and exposed physical parameters:
  <https://github.com/mixxxdj/mixxx/wiki/midi-scripting>
- xwax keeps DVS target position authoritative while applying pitch correction
  to close the player error:
  <https://raw.githubusercontent.com/xwax/xwax/master/player.c>
