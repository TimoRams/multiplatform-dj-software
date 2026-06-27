# Active Context

*Last updated: 2026-06-06*

## Current task
Mixer EQ / SC / volume faders fix — **completed**.

## What changed
1. **`MixerControl`** created **before** `engine.load()` and exposed as context property **`mixerControl`** (not `MixerApi` singleton — `qmlRegisterSingletonInstance` on URI `DJSoftware` breaks `QML_ELEMENT` types like `ScrollingWaveformItem`).
2. **`applyAllVolumes()`** — CrossfaderBar syncs CF state then applies stored fader levels; no longer overwrites faders from stale `volA`–`volD` defaults.
3. **`MixerSection.qml`** / **`CrossfaderBar.qml`** call **`mixerControl`** directly for trim, EQ, filter, fader, cue.
4. **Smoke test** (`tests/smoke/mixer_dsp_smoke.cpp`) verifies `MixerDspSource` trim attenuation and high-EQ boost on HF sine.

## Manual verification (user)
1. Play track on deck A — VU moves
2. Turn **GAIN** down — VU level drops
3. Turn **HI** full cut — treble dulls
4. Move **channel VOL** to zero — deck silent with CF centered
5. Crossfader still blends A/B

## Architecture note
- Volume path: `mixerControl.setChannelFader` → `MixerControl::applyChannelVolume` → `DjEngine::setVolume` × CF multiplier
- EQ/trim/filter: `mixerControl.setTrim/setEq*/setFilter` → `DjEngine` → `MixerDspSource`
