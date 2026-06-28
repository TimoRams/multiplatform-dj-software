# Tech Context

## Stack
- **C++26**, CMake 3.22+, Qt 6 (Quick, QML module `DJSoftware`)
- **JUCE** audio graph per deck → `MixerDspSource` (trim/EQ/filter/fader) → `DjMasterBus`
- **SQLite** library via `LibraryDatabase`

## Entry / lifecycle
- `src/main.cpp` → `ApplicationBootstrap::run()`
- Decks `deckA`–`deckD` created async (`QTimer::singleShot`) and exposed as QML context properties
- `DjMasterBus` registers shared `AudioDeviceManager` callback

## Mixer audio path
| Control | C++ target |
|---------|------------|
| Gain | `DjEngine::setTrim` → `MixerDspSource::setTrim` |
| EQ HI/MID/LOW | `setEqHigh/Mid/Low` → `MixerDspSource::setEq` |
| Sound Color / filter | `setFilter` + `FxManager::setSoundColorDeck` |
| Channel fader + CF | `DjEngine::setVolume` (fader × crossfader multiplier) |

## QML integration
- Context properties: `deckA`–`deckD`, `parameterStore`, `mixerControl`, `cursorControl`, `fxManager`, `libraryPreview`
- **Working pattern:** CrossfaderBar calls `engineA.volume = …` directly — mixer must use **`mixerControl` Q_INVOKABLE** from C++, not nested inline-component `engine` bindings.
- `parameterStore` still used for MIDI ↔ UI sync; `MixerParameterBridge` applies MIDI-normalized params.

## AIO vs desktop layout

Single codebase; mode is **not** a fork.

| Layer | Files | Role |
|-------|-------|------|
| Shell / tabs | `src/qml/main.qml` | `allInOneMode`, `activeMainTab`, `compactLayout`, panel visibility |
| Header | `src/qml/TopHeader.qml` | DESK/AIO toggle, LIB/⚙ tabs |
| Library touch | `src/qml/Library.qml` | `touchMode` (= `allInOneMode`), swipe, `AioLoadBar`, row height |
| Shared deck/mixer | `DeckControl.qml`, `MixerSection.qml`, engine C++ | Same in both modes |

**Reference test sizes (AIO DoD):** 1280×800 (primary), 1024×600 (compact). `compactLayout` = `height < 720 || width < 1100`.

## Shared-first feature rule (new UI work)

1. **C++ / data first** — engine, DB, context properties; mode-agnostic.
2. **One QML component** — core UI works in DESK and AIO; use `touchMode` / `window.sp()` only for size and input (gestures vs drag).
3. **AIO extras optional** — e.g. `AioLoadBar`, swipe actions behind `Library.qml` `touchMode`.

Avoid new `allInOneMode` branches outside `main.qml`, `TopHeader.qml`, and `Library.qml` without strong reason.

**Examples:** `PreviewControlBar` (both modes); `AioLoadBar` (AIO only).

## Post-AIO desktop regression

After AIO UI changes, run:

```bash
./scripts/desktop-regression-checklist.sh
```

Manual checklist also in `.cursor/memory/progress.md`.

## Build
Single build dir `build/` via the fast build script (preset `linux-dev-fast`).
```bash
./build-fast                      # configure (first run) + incremental -> build/bin/BrockDJ
ctest --test-dir build --output-on-failure
```

## Key paths
- QML mixer: `src/qml/MixerSection.qml`, `src/qml/CrossfaderBar.qml`
- Mixer C++: `src/app/MixerControl.cpp`
- Engine core: `src/engine/DjEngine_*.cpp`, `src/engine/audio/MixerDspSource.cpp`
