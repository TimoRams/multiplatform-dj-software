# Project Brief — BrockDJ

## What this is
Cross-platform DJ application (Qt 6 / QML + JUCE audio). Target name in repo: **BrockDJ** (`multiplatform-dj-software`).

## Core goals
- Low-latency 2–4 deck DJ performance UI
- Pioneer-style mixer (gain, EQ, filter/sound color, channel faders, crossfader)
- Library with analysis (BPM, key, beatgrid, waveforms)
- MIDI / DDJ-FLX10 controller support

## Platform priority
1. Linux (primary)
2. Apple Silicon macOS
3. Intel macOS
4. Windows

## Current focus areas
- **Mixer UI → audio routing** must use direct C++ `MixerControl` facade (context property `mixerControl`), not nested QML `engine.*` or fragile `parameterStore`-only paths for UI knobs.
- Stability after large restructure (`ApplicationBootstrap`, `ApplicationLifecycle`, split engine/MIDI/library units).

## Non-goals (for now)
- Rewriting entire UI architecture
- Committing `.cursor/` except `memory/` bank files
