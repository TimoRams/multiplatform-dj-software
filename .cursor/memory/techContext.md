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
- Context properties: `deckA`–`deckD`, `parameterStore`, `mixerControl`, `cursorControl`, `fxManager`
- **Working pattern:** CrossfaderBar calls `engineA.volume = …` directly — mixer must use **`mixerControl` Q_INVOKABLE** from C++, not nested inline-component `engine` bindings.
- `parameterStore` still used for MIDI ↔ UI sync; `MixerParameterBridge` applies MIDI-normalized params.

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
