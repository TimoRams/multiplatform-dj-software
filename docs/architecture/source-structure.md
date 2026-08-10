# BrockDJ Source Structure

This document describes the currently implemented structure. It is a guide for
safe cleanup work, not a proposal for a parallel runtime.

## Scope snapshot

Measured on 2026-08-09 against `c9a67c3` before the current uncommitted
cleanup pass:

| Area | Files | Lines |
| --- | ---: | ---: |
| `src/` total | 239 | 65,914 C++/QML lines |
| Production C++ sources | 92 | — |
| Production headers | 107 | — |
| QML components | 36 | — |
| Test files | 40 | 5,570 C++ lines |

## Runtime ownership

The audio runtime has one authoritative data path:

```text
AudioPageCache
  -> DeckAudioPipeline (one per deck)
  -> MasterMixer
  -> HeadphoneBus / MasterTap
  -> AudioOutputRouter
  -> audio hardware
```

`AudioPageCache` is the decoder/worker boundary. It publishes immutable pages;
the audio callback may read them but must never decode, open files, wait for a
lock, or allocate on a miss. `DeckAudioPipeline` owns per-deck source and
render-mode processing. `MasterMixer` is the only component that sums deck
programs, applies the crossfader/master path and final limiter. `HeadphoneBus`
mixes pre-fader cue with the post-limiter master tap. `AudioOutputRouter` only
maps prepared buffers to physical channel pairs.

The channel meter and PFL source are after Trim/EQ/Color-FX/pre-fader insert
FX and before the channel and crossfader gains. This is exposed as
`preFaderVuLevel*`; it is therefore independent of a closed deck fader. The
post-fader `vuLevel*` remains a separate display signal. MIDI/HID deck meters
and `MixerSection.qml` consume the pre-fader values.

## Source domains

| Area | Responsibility | Main boundary |
| --- | --- | --- |
| `app/` | bootstrap, settings, control clock, shared mixer controls | Qt/control thread |
| `audio/cache/` | page cache, decoder worker and callback-safe reader | worker -> immutable pages |
| `audio/` | device callback, deck channels, master, cue and hardware routing | audio callback |
| `engine/deck/` | track loader, transport, cue/loop | control -> audio commands |
| `engine/scratch/`, `audio/RenderModeRouter*` | scratch state and Direct/Keylock/Scratch handoff | control/audio boundary |
| `engine/sync/`, `sync/` | beat and tempo synchronization | control clock |
| `engine/facade/` | `DjEngine` Qt/QML API grouped by responsibility | public UI facade |
| `analysis/`, `waveform/`, `rendering/` | analysis values, snapshots and Qt Quick renderers | worker -> immutable snapshot |
| `library/`, `database/`, `io/` | library, persistence and media I/O | DB/media workers |
| `controllers/`, `midi/` | controller input and feedback/HID output | device/control thread |
| `qml/` | presentation and interaction only | Qt Quick scene |

`DjEngine`, QML, MIDI and HID are command/display facades. They do not own a
second audio graph or decode path.

## UI integration boundaries

Desktop and all-in-one layouts share one C++ and QML implementation. Product
behavior belongs in mode-independent C++ owners and shared QML components;
`touchMode` or UI metrics may adapt sizing and input. New `allInOneMode`
composition branches are limited to `main.qml`, `TopHeader.qml`, and
`Library.qml` unless a reviewed product requirement needs another boundary.

Mixer QML uses the application-owned `mixerControl` context/singleton bridge.
Channel-fader changes go through `MixerControl::setChannelFader`, while
crossfader state is published through `syncCrossfaderState` followed by
`applyAllVolumes`. Inline QML components must not create a second mixer state
path through nested deck objects. `ParameterStore` and `MixerParameterBridge`
remain the MIDI-to-control synchronization path.

Reference AIO layouts are 1280x800 and 1024x600. UI changes that affect these
profiles must also run `scripts/desktop-regression-checklist.sh` and the manual
checks in `docs/testing/regression-checklist.md`.

## Facade include policy

`engine/facade/FacadeIncludes.h` is a transitional header for the remaining
large facade implementation units. Small isolated units must include only the
types they use. `Fx.cpp`, `Settings.cpp`, `Diagnostics.cpp`, `Scratch.cpp`, and
`Sync.cpp` now follow this policy. The larger Core, Transport, and CueLoop units
remain on the transitional header until their direct dependency sets are
extracted and compiled as separate low-risk batches.

## Local build workflow

Use the repository entry points:

```bash
./build-fast
./test-fast
```

They use separate Ninja build trees for application and tests, cap parallel
jobs at eight by default, use `ccache` when installed, and disable QML cachegen
for fast local development. Do not replace them with ad-hoc CMake build trees.
