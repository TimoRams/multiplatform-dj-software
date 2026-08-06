# Audio Routing Phase 2 Baseline

This baseline was captured before moving production audio code. Its former baseline
checks are now hard assertions in
`tests/audio_routing/audio_routing_contract_tests.cpp`.

## Green current behavior

- A closed channel program output keeps Master silent.
- PFL remains audible from the pre-fader tap with the channel fader closed.
- The pre-fader meter/tap remains active with a closed fader.
- Master Gain does not alter the PFL signal.
- Existing routed A/B cut and Thru-equivalent program behavior reaches Master as expected.
- An independently registered FX tail continues after deck endpoint retirement.
- Existing callback diagnostics report no allocation, buffer growth, or blocking lock.
- Existing `deck_audio_graph`, `scratch_cache`, and `cached_playback` tests cover scratch
  crossfade and cache-miss resume continuity at the deck/cache boundary.

## Closed architecture gaps

The refactor closed the seven gaps recorded here: Booth and recording now use MasterTap,
headphones own PFL/gain/safety limiting, MasterMixer owns all crossfader state, Scratch
uses its fast-cut curve, deck tails are typed post-fader returns, and callback parameters
arrive as one coherent per-block snapshot. These behaviors are required for the routing
contract test to pass.
