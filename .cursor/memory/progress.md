# Progress

## Done (recent)
- [x] Full codebase restructure (bootstrap, lifecycle, split MIDI/library/waveform/FLX10, smoke tests, CI macOS)
- [x] FLX10 jog display fix, `.gitignore` for `.cursor/` (with memory bank exception)
- [x] Multiple mixer fix attempts (parameterStore bridge, direct engine bindings)
- [x] **Mixer routing fix** — `MixerApi` singleton, `applyAllVolumes()`, QML wired to C++ facade
- [x] **Mixer DSP smoke test** — trim + high EQ verified in `tests/smoke/mixer_dsp_smoke.cpp`

## In progress
- [ ] User confirmation: mixer EQ / SC / volume faders affect audio in live app
- [ ] Cursor hide on knob/fader drag (guards added; verify on macOS)

## Known issues
- (none for mixer routing — pending user live test)

## Not started / backlog
- Commit/push mixer fix (only when user asks)
- Optional: deduplicate `MixerParameterBridge` vs `MidiFlx10Bridge` gain/EQ handlers
