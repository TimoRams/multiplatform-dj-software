# Active Context

*Last updated: 2026-06-27*

## Current task
Mixer channel strip (EQ/GAIN/SC/channel fader) still dead + scrolling waveform
"hangs" at loops/hotcues — **both fixed**.

### Mixer channel strip — two compounding QML bugs (`MixerSection.qml`)
1. **Grouped-alias signal handler never fires.** Knob cells wired audio via
   `knob.onValueChanged: …` where `knob` is `property alias knob: innerKnob`. A
   grouped-alias *signal handler* at the use site is silently dropped, so the
   knobs never called `mixerControl`. Fix: `KnobCell` now exposes a real
   `signal moved(real value)` emitted by a **direct** `onValueChanged: kc.moved(value)`
   on the inner `Knob`; use sites use `onMoved: (v) => mixer.setXxxValue(channelId, v)`.
2. **Self-referential `channelId: channelId`.** In `ChannelSide`, bindings like
   `channelId: channelId` / `cueActive: cueActive` / `mirrored: mirrored` /
   `deckName: deckName` on child components (`KnobStackColumn`, `FaderColumn`,
   `ChannelHeader`) that *own a property of the same name* resolve the RHS to the
   child's own (empty) property → `channelId` arrived as `""`, so
   `deckForChannelId("")` returned null and nothing reached a deck (the channel
   fader was dead for the same reason). Fix: gave `ChannelSide` `id: side` and
   qualified every such binding as `side.<prop>`.
   Verified live: forcing a knob value logged `setEqHigh "deckA" 0.5 deck=0x…`
   (was `setEqHigh "" 0.5 deck=0x0`). The C++/`MixerControl`→deck path was always
   fine (startup `applyAllControls` reached valid decks).

### Scrolling waveform loop/hotcue "hang" (`ScrollingWaveformItem.cpp`)
The monotonic playback anchor only treated large **forward** deltas as a seek;
a large **backward** jump (loop wrap to loop-in, hotcue/cue jump) fell through to
"hold last center" → waveform froze at loop-out until playback caught up. Fix:
`largeSeek = std::abs(delta) > 2px` follows real jumps in **either** direction;
only sub-2px motion opposing the play direction is suppressed (jitter guard).

## Previous task
"Whole mixer does nothing" — **root cause was audio device, not mixer QML**. Fixed.

### Root cause
Saved output device `DDJ-FLX10 Analog Surround 4.0` (controller unplugged) failed to
open. The fallback chain in `applyAudioDeviceSettingsExpected` (`DjEngine_Settings.cpp`)
could return *no error* yet leave **no open device** ("Retrying with default output
device" → still not ready). The code then **restored the broken saved device** and
returned failure → no active audio → every mixer/deck control was inaudible, so the
mixer looked dead.

### Fix
Added a last-ditch recovery before the restore-previous block: when no device is ready
after all targeted fallbacks, call `initialiseWithDefaultDevices(0, 2)` and, on success,
reset to safe stereo routing (`OutputRoutingConfig{}`, master ch1, booth/HP disabled) +
show a fallback message instead of restoring the unusable device. Verified in logs:
`No usable output after fallbacks; forcing system default device` → `Backend output
latency ... sr 48000` (device open at startup; no more "restoring previous device" /
"Play requested without active audio device").

> Verified in the live binary after a clean `./build-fast`.

## Build layout (consolidated 2026-06-27)
Single build dir only: **`build/`**, produced by **`./build-fast`** (preset `linux-dev-fast`,
Ninja, RelWithDebInfo, QML cachegen off). The old `build/` (stale ASAN) and `build-dev/`
were deleted; `CMakePresets.json`, `scripts/build-fast-linux.sh`, `.gitignore`, README and
`.cursorrules` all point at `build/`. Always build with `./build-fast`; run `./build/bin/BrockDJ`.
Do not recreate `build-dev/`.

## Previous task
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
