# Progress

## Done (recent)
- [x] **Mixer channel strip fix (real root cause)** — `MixerSection.qml`: (1) replaced silent
  grouped-alias `knob.onValueChanged:` with a `KnobCell.moved` signal + `onMoved:`; (2) fixed
  self-referential `channelId/cueActive/mirrored/deckName: <same>` bindings via `id: side` +
  `side.<prop>`. Knobs/EQ/SC and channel faders now reach the deck (verified in logs).
- [x] **Scrolling waveform loop/hotcue hang fix** — `ScrollingWaveformItem.cpp` anchor now
  follows large jumps in *either* direction; only sub-2px reverse jitter is suppressed.
- [x] **Audio device fallback fix** — unplugged saved device (DDJ-FLX10) no longer leaves the app
  with no output; `DjEngine_Settings.cpp` now forces `initialiseWithDefaultDevices` + safe stereo
  routing instead of restoring the broken device. Was the real cause of "mixer does nothing".
- [x] Full codebase restructure (bootstrap, lifecycle, split MIDI/library/waveform/FLX10, smoke tests, CI macOS)
- [x] FLX10 jog display fix, `.gitignore` for `.cursor/` (with memory bank exception)
- [x] Multiple mixer fix attempts (parameterStore bridge, direct engine bindings)
- [x] **Mixer routing fix** — `MixerApi` singleton, `applyAllVolumes()`, QML wired to C++ facade
- [x] **Mixer DSP smoke test** — trim + high EQ verified in `tests/smoke/mixer_dsp_smoke.cpp`

## In progress
- [ ] User confirmation: mixer EQ / SC / volume faders + waveform loop scroll feel right in live app
- [ ] Cursor hide on knob/fader drag (guards added; verify on macOS)

## Known issues
- Watch for the same QML pitfalls elsewhere (DeckControl/FxBar/PerformancePads): grouped-alias
  signal handlers (`alias.onXxx:`) silently don't fire, and `prop: prop` bindings self-reference
  when the child owns a property of the same name — qualify with the parent's `id`.

## Not started / backlog
- Commit/push mixer fix (only when user asks)
- Optional: deduplicate `MixerParameterBridge` vs `MidiFlx10Bridge` gain/EQ handlers
