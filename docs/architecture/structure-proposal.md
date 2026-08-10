# Repository Structure and QML Extraction Proposal

This is a review proposal. The CMake source-list modularization described below is implemented, but
no production source, QML component, resource or packaging file is moved by this cleanup.

## Implemented Build-File Boundary

Production target membership now lives beside each source domain:

```text
src/
├── analysis/CMakeLists.txt
├── app/CMakeLists.txt
├── audio/CMakeLists.txt
├── controllers/CMakeLists.txt
├── database/CMakeLists.txt
├── domain/CMakeLists.txt
├── engine/CMakeLists.txt
├── fx/CMakeLists.txt
├── io/CMakeLists.txt
├── library/CMakeLists.txt
├── link/CMakeLists.txt
├── midi/CMakeLists.txt
├── platform/CMakeLists.txt
├── qml/CMakeLists.txt
├── rendering/CMakeLists.txt
└── waveform/CMakeLists.txt
```

The root target is still created and configured centrally. The module files only declare source or
resource membership. QML paths intentionally remain root-relative so the existing
`qrc:/DJSoftware/src/qml/...` URLs do not change.

The remaining bulk in the root `CMakeLists.txt` is predominantly explicit test-target setup. A later
build-only change could move that block verbatim to `cmake/BrockDJTests.cmake`; it should not be mixed
with test-executable consolidation because those are separate risk levels.

## QML Extraction Candidates

These changes need a dedicated UI review. They are not mechanical file moves: binding context,
`Connections` lifetime, focus scopes, popup parents and creation/destruction timing must be tested in
both DESK and AIO modes.

| Current file | Evidence | Suggested boundaries |
| --- | --- | --- |
| `Library.qml` | 6,213 lines, about 260 visual primitives, browse state, navigation, tables, preview, notes and menus in one root | `LibraryBrowseCoordinator`, `LibrarySidebar`, `LibraryTrackTable`, `LibraryPreviewBar`, `LibraryNotesPanel`, `LibraryContextMenus` |
| `SettingsPanel.qml` + `SettingsWindow.qml` | near-parallel audio/MIDI state and the same 25 named controls; most matching blocks differ only by shell geometry | one shared `SettingsContent` with `AudioSettingsPage`, `MidiSettingsPage` and display/behavior pages; retain panel/window shells |
| `TopHeader.qml` | 1,564 lines and about 113 visual primitives; view menu, monitoring, master/cue controls and quick tray are independent groups | `HeaderViewMenu`, `HeaderMasterControls`, `HeaderMonitor`, `HeaderQuickAccessTray` |
| `DeckControl.qml` | 1,314 lines; metadata, transport, pads, tempo and two popups; it already proves local components work | `DeckMetadataPanel`, `DeckTransportControls`, `DeckTempoPanel`; keep engine-facing state at the deck root |

Recommended extraction order:

1. Extract the settings content first because the duplicated surfaces provide direct parity checks.
2. Extract leaf-only header groups with explicit properties and signals.
3. Extract deck visual groups while keeping transport state and `Connections` in `DeckControl`.
4. Split `Library.qml` last; first write focus, keyboard-navigation, swipe and preview regressions.

## Proposed Root Layout

The current `src/`, `tests/`, `scripts/` and `docs/` roots already fit the project. A restrained target
layout is preferable to moving every header into a public `include/` tree: BrockDJ builds an
application, not a public C++ SDK, so module-private headers should remain beside implementations.

```diff
 /
 ├── CMakeLists.txt
 ├── CMakePresets.json
 ├── cmake/
+│   └── BrockDJTests.cmake          # optional follow-up; declarations only
 ├── src/                            # current domain layout retained
 ├── tests/                          # current domain layout retained
 ├── docs/
 │   ├── architecture/
 │   └── testing/
 ├── resources/
 │   └── icons/
 ├── packaging/
 │   ├── linux/
+│   │   ├── net.ramsbrock.BrockDJ.desktop
+│   │   ├── net.ramsbrock.BrockDJ.metainfo.xml
 │   │   └── udev/
+│   ├── macos/                      # only when platform-owned inputs exist
+│   └── windows/                    # only when platform-owned inputs exist
-├── resources/packaging/
 ├── scripts/
 │   └── ci/                         # cross-platform orchestration retained
 └── libs/                           # submodules, unchanged
```

Moving the two Linux metadata files would require synchronized updates in
`.github/workflows/cross-platform-build.yml`, `scripts/ci/package-appimage.sh`, documentation and any
release tooling. That move is deliberately deferred until approved.

## CMake Impact of Future Moves

| Proposed move | Build references to update |
| --- | --- |
| QML component extraction | `src/qml/CMakeLists.txt`; imports/instantiations in parent QML files |
| Linux metadata into `packaging/linux` | workflow lint path and `scripts/ci/package-appimage.sh` |
| test declarations into `cmake/BrockDJTests.cmake` | replace the root test block with one `include()`; source paths remain root-relative |
| production file move between domains | remove from one domain `CMakeLists.txt`, add to the destination module, then update includes |

## Build-Acceleration Assessment

Unity builds are not safe for the current target. A clean temporary evaluation with
`CMAKE_UNITY_BUILD=ON` failed before linking because separately valid anonymous namespaces collide
when `Core.cpp` and `CueLoop.cpp` share a translation unit. Another unity batch exposed a `B0` macro
collision between platform headers and Signalsmith. Fixing those issues would change protected
monolith code and third-party interaction, so unity remains disabled.

A target-wide precompiled header is also not enabled. `BrockDJ` combines application Qt/JUCE sources,
generated Qt sources and JUCE module implementation translation units in one target. Injecting one
macro-bearing Qt/JUCE PCH into all of them is cross-platform risk, while ccache already covers the
incremental-build case. A useful PCH experiment first requires separating vendored JUCE module
implementations from first-party objects and collecting clean-build timings on Linux, macOS and
MSVC. Until then, the measurable low-risk improvement is accurate ccache invalidation for every
module `CMakeLists.txt`, which the CI workflow now includes.
