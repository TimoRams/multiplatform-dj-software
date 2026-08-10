# Source Cleanup Report

Date: 2026-08-10
Baseline: `c9a67c3`  
Scope: safe incremental cleanup; no public QML API, audio routing, submodule,
or controller protocol change.

## Completed in this pass

| Change | Why | Risk control |
| --- | --- | --- |
| Removed the broad facade umbrella include from `Fx.cpp` | FX commands only need the `DjEngine`, deck pipeline/channel processor, and `QHash`. | No behavior or API change; direct dependencies are explicit. |
| Removed the broad facade umbrella include from `Settings.cpp` | Device settings only need the `DjEngine`, audio engine, and device service. | No behavior or API change; static audio commands remain unchanged. |
| Removed the broad facade umbrella include from `Diagnostics.cpp` | Latency/diagnostic code now declares its actual audio, transport, FX, Qt logging, and standard-library dependencies. | No behavior or API change; the public latency rows are unchanged except wording. |
| Renamed the latency row from a Rubber Band-specific local name to generic `Keylock / Time-stretch` | Signalsmith is the default keylock backend and the UI must not imply one fixed implementation. | Display text only. |
| Removed the broad facade umbrella include from `Sync.cpp` | Beat/bar calculations and synchronization now declare their transport, track-data, coordinator, Qt, and standard-library dependencies directly. | No behavior or public API change; the translation unit and full application build pass. |
| Removed the broad facade umbrella include from `Scratch.cpp` | Scratch routing now declares its pipeline, processor, transport, render-router, jog-policy, JUCE, and standard-library dependencies directly. | No behavior or public API change; the translation unit, full application build, and complete test suite pass. |

The transitional `FacadeIncludes.h` is now used by three complex facade units
instead of eight. It is intentionally retained until each remaining file has a
reviewed direct include set; deleting it earlier would only trade cleanup for
fragile transitive includes.

## Audio and performance invariants checked

- The only audio flow remains `AudioPageCache -> DeckAudioPipeline ->
  MasterMixer -> HeadphoneBus / MasterTap -> AudioOutputRouter -> hardware`.
- Deck `preFaderVuLevel*` reads the channel meter before the fader and
  crossfader. `MixerSection.qml` and `MidiFeedbackController` use these
  values, so the per-deck mixer/HID meters remain fader-independent.
- `vuLevel*` remains explicitly post-fader for the separate header/display
  visualization; it is not used by the controller deck meters.
- The `RenderModeRouter` `legacyPending` condition is an active compatibility
  branch for an in-flight scratch release handoff, not obsolete routing code.

## Validation

| Gate | Result |
| --- | --- |
| `git diff --check` | Passed after the cleanup edits. |
| `./build-fast` | Passed; the complete `BrockDJ` application compiled and linked. |
| `./test-fast` | Passed; all 39 tests succeeded. |

## Deliberately deferred

- Direct include extraction for `Core.cpp`, `Transport.cpp`, and `CueLoop.cpp`.
  They are the remaining large multi-domain facades and need one compile-verified
  file at a time.
- Settings QML consolidation. `SettingsWindow.qml` and `SettingsPanel.qml`
  are likely duplicated but need a behavior/visual inventory first.
- Canonical waveform-data migration. Multiple waveform representations are
  still intentional because HID, compact and progressive render paths have
  different snapshot contracts.
- Test-executable bundling. It must be justified with actual incremental-build
  measurements rather than reducing target count cosmetically.

## Next safe batch

1. Extract direct includes from `Transport.cpp`, the smallest remaining facade
   unit, then repeat the two repository gates.
2. Continue with `Core.cpp` and `CueLoop.cpp` one compile-verified file at a time.
3. Update this report with the actual full gate result before moving or
   deleting any source file.
