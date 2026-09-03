# BrockDJ

> A modern DJ application built with JUCE and Qt 6/QML.

![License](https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square)
![C++](https://img.shields.io/badge/C%2B%2B-23-informational?style=flat-square)
![Qt](https://img.shields.io/badge/Qt-6-41CD52?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey?style=flat-square)

## Project status

BrockDJ is currently developed mostly by a single developer (me :D), with only limited help from others. A significant part of the project has been developed with the assistance of AI tools for implementation, debugging, refactoring, and experimentation.

The current focus is primarily on functionality, performance, architecture, and long-term stability rather than having a perfectly polished product. As the project becomes more stable, more attention will also go into UI polish, documentation, branding, packaging, and the overall presentation of the software.

The project is still under active development, so bugs, unfinished features, experimental implementations, and larger changes should be expected.

> **The name is not final.** The project currently appears under names such as **BrockDJ** and **multiplatform-dj-software**. A final name has not been decided yet. If you have a good name idea, feel free to open an issue or discussion. Suggestions are very welcome. :D

## Features

* Dual-deck playback with FLAC, WAV, OGG, and MP3 support
* GPU-rendered RGB waveforms, beat grids, loops, hot cues, and sync
* Three-band EQ, filter, crossfader, and built-in effects
* Library browsing, metadata extraction, cover art, and background analysis
* MIDI mapping, controller integration, and Ableton Link support

## Platform status

| Platform | Status                       |
| -------- | ---------------------------- |
| Linux    | Primary development platform |
| macOS    | Actively supported           |
| Windows  | ~~Not actively maintained~~  |

> **Windows is currently de-prioritized.** I do not use Windows in daily development and have very limited access to Windows hardware. Development therefore focuses on Linux and macOS for the time being.

## Build

Clone the repository with its submodules:

```bash
git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
cd multiplatform-dj-software
```

### Linux

Install dependencies on Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-declarative-dev \
  libasound2-dev libtag1-dev libkeyfinder-dev librubberband-dev libsqlcipher-dev
```

Build and run:

```bash
./build-fast
./build/bin/BrockDJ
```

### macOS

```bash
brew install cmake ninja pkg-config qt@6 taglib rubberband libkeyfinder sqlcipher
export CMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
cmake --preset macos-dev-arm64
cmake --build --preset macos-dev-arm64
```

For Intel Macs, replace `macos-dev-arm64` with `macos-dev-x86_64`.

If Ninja is not installed, use `macos-dev-arm64-make` or `macos-dev-x86_64-make` instead.

## Documentation

* [Complete build guide](docs/building.md)
* [Architecture](docs/architecture/)
* [Packaging](docs/packaging.md)
* [Dependency pins](docs/dependencies.md)

## License

Licensed under the [GNU Affero General Public License v3.0 or later](LICENSE).
