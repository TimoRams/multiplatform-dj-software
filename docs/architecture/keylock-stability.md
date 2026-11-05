# Keylock and sampler tempo stability

The deck tempo-sweep regression previously reported four cases with envelope
holes: +/-16% at 64/256-frame buffers, at both 44.1 and 48 kHz. Debugger inspection
of a failing 44.1 kHz/64-frame block measured 0.1315 input RMS and 0.0384 output
RMS with keylock fully active, one seed and no cache misses. The attenuation was
inside the vocoder rather than a cache underrun or a reseed.

Signalsmith splits analysis, frequency mapping and synthesis across one hop.
Changing its frequency multiplier between these steps can mix pitch maps within
one spectrum. TimeStretchProcessor now advances parameter ramps continuously but
applies a new map only at hop boundaries. Worker pre-roll/catch-up batches stop
at the same boundaries; reset also resets the wrapper's hop position.
Live processing also submits runs up to the next hop boundary, retaining
sample-accurate ramp advancement without a library call for every audio sample.

The 120 ms analysis window remains for bass resolution. Overlap increases from
eight to twelve, giving approximately 10 ms hops and 130 ms total spectral
latency at 44.1/48/96 kHz. This costs more FFT work and still requires physical
device profiling. The existing deck test bounds tempo response using the actual
read-ahead horizon rather than assuming zero spectral latency.

Sampler read-ahead formerly extrapolated every future position from the newest
tempo. A tempo update could therefore move the vocoder's input discontinuously;
a new regression measured a 0.3042 sample step while the loop and keylock remained
active. Each voice now uses the same preallocated TempoTimeline as the decks,
with a minimum 25 ms target ramp. Already scheduled positions and their tempo
values remain consistent. A clock-generation change still never seeks or
restarts a playing loop; pending-start rescheduling retains its existing policy.
The timeline adds approximately 1 MiB of preallocated memory per sampler voice.

No pipeline preparation, allocation, reset, blocking lock or worker seeding was
added to the audio callback. Existing realtime counters remain asserted by the
tests. Test limits were not relaxed.

## Validation

Use an optimized build for live scheduling and CPU acceptance:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target BrockDJ_time_stretch_tests BrockDJ_sampler_audio_tests BrockDJ_deck_audio_graph_tests -j4
ctest --test-dir build-release -R '^(time_stretch|sampler_audio|deck_audio_graph)$' --output-on-failure
```

`BROCKDJ_KEYLOCK_SWEEP_ONLY=1` runs just the twelve deck sweeps. They retain the
0.035 maximum-step and 0.04 minimum block-RMS limits, active-keylock, unchanged
seed/generation and bounded-response checks. The sampler test adds a 440 Hz loop
at a different source sample rate, +/-16% master-tempo sweeps and a clock/phase
handover, with the same step and envelope limits. Its measured maximum step fell
to 0.0147 with zero quiet blocks on the macOS development host.
All twelve targeted deck sweeps also pass in the unoptimized development build.
The full Release deck graph, including four decks, eight sampler voices, preview
and recording, passes; recorder, sync, clock, routing, master bus, loader and
cue/loop regression tests also pass.

The standalone time-stretch suite passes in Release, including spectral/stereo
quality, pitch, play/pause, stale worker results and scratch re-entry. Unoptimized
development builds reproduce worker engagement deadline failures also present in
the unchanged source; denser overlap increases that scheduling pressure. A green
optimized software run does not establish CoreAudio/ALSA/ASIO jitter, low-power
device capacity, or long-duration recording performance on target hardware.

The optional `BROCKDJ_TIME_STRETCH_BENCHMARK=1` run passed its functional checks.
Four active Signalsmith processors at 48 kHz measured 0.35–0.39 ms mean and at
most 1.24 ms per 64-frame callback (1.33 ms budget); the 128-frame worst was
1.97 ms (2.67 ms budget). The 64-frame margin is small and this synthetic run
does not include eight sampler voices, recording, device scheduling or effects.
Further profiling and the realtime-scheduled hardware soak remain necessary;
the functional test pass must not be presented as meeting every callback
deadline on target hardware.
