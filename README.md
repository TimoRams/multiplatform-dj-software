<div align="center">

# BrockDJ

**Open-source DJ software for Linux and macOS, built with C++23, JUCE and Qt 6.**

Four decks, CDJ-style performance tools, stem separation, a sampler with a step
sequencer and Pioneer DDJ-FLX10 support.

[![Build](https://github.com/TimoRams/multiplatform-dj-software/actions/workflows/cross-platform-build.yml/badge.svg?branch=main)](https://github.com/TimoRams/multiplatform-dj-software/actions/workflows/cross-platform-build.yml)
![License](https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square)
![C++](https://img.shields.io/badge/C%2B%2B-23-informational?style=flat-square)
![Qt](https://img.shields.io/badge/Qt-6-41CD52?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey?style=flat-square)

[Features](#features) · [Stems](#stems) · [Build on Linux](#linux) · [Build on macOS](#macos) · [Docs](#documentation)

</div>

---

## Project status

BrockDJ is currently developed mostly by a single developer (me :D), with only limited help from others. A significant part of the project has been developed with the assistance of AI tools for implementation, debugging, refactoring, and experimentation.

The current focus is primarily on functionality, performance, architecture, and long-term stability rather than having a perfectly polished product. As the project becomes more stable, more attention will also go into UI polish, documentation, branding, packaging, and the overall presentation of the software.

The project is still under active development, so bugs, unfinished features, experimental implementations, and larger changes should be expected.

> **The name is not final.** The project currently appears under names such as **BrockDJ** and **multiplatform-dj-software**. A final name has not been decided yet. If you have a good name idea, feel free to open an issue or discussion. Suggestions are very welcome. :D

## Features

### Decks and mixing

- Up to four decks with FLAC, WAV, OGG and MP3 playback
- Three-band EQ, filter, crossfader curve and A / THRU / B assignment
- Two FX units plus Touch FX
- Key lock via Signalsmith Stretch or Rubber Band
- Sync, Ableton Link, MIDI clock in/out and a LUFS meter on the master

### CDJ-style performance

- Hot cues, memory cues and saved loops
- Beat loops from 1/4 to 32 beats, slip loops (rolls) while a pad is held
- Slip mode, quantize and Key Sync
- Vinyl-style scratching on the jog wheel and directly on the waveform

### Waveforms and analysis

- GPU-rendered scrolling and overview waveforms (RGB, EQ colour or 3-band)
- BPM, key and beat grid analysis in the background
- Phrase detection with song parts (Intro, Verse, Build, Drop, Breakdown,
  Outro) shown below the overview and in a phrase counter

### Library

- Collection, playlists, folder view and a play queue
- Search syntax such as `bpm:120-128`, `artist:x` or `~key:8A` for
  harmonically compatible tracks
- Read-only Rekordbox USB browsing (legacy export and OneLibrary)
- Automix, a tracklist of what was actually played and headphone preview

### Sampler and sequencer

- Four banks with eight slots each: one-shots, synced loops and loops
  captured from a deck, with a sample editor
- Step sequencer inspired by grooveboxes such as the Korg Electribe: up to 64
  steps per pattern, per-step velocity, swing, track mute/solo and pattern
  changes that wait for the running pattern to finish

### Live and hardware

- Set recording to 24-bit WAV with track markers
- MIDI learn and controller mappings
- Pioneer DDJ-FLX10 integration, including the jog wheel displays
- Desktop layout and a compact all-in-one (AIO) layout for small screens

## Stems

BrockDJ splits a track into **vocals, melody, bass and drums** with the
HT-Demucs model running locally through ONNX Runtime. On the deck's **STEMS**
pad page, pads 1–4 mute a stem and pads 5–8 solo it. Muted parts fade to grey
in the waveform, so you can see what is still audible.

Separation runs in the background and starts at the playhead when a track is
loaded. Sections that are not separated yet play the normal mix. The pad tab
shows `STEMS WAIT` until the playhead is covered, then the progress in percent. On a modern laptop CPU the
first separated audio is usually ready a few seconds after loading, while a
full track can take a few minutes, depending on the hardware.

For gigs, prepare tracks in advance: **Stems Preanalysis** in the library
separates the current view in the background, shows its progress in the bar
at the top and marks finished tracks with a green **S**. Results are cached on disk (up to 8 GB, oldest entries removed
first), so prepared tracks are ready immediately.

**Setting up stems:**

1. Before configuring CMake, download ONNX Runtime once:
   `./scripts/fetch-onnxruntime.sh`. In an existing build directory, run
   `cmake -S . -B build` again afterwards.
2. Start BrockDJ and open **Settings → DJ / Sync → Stems → Download**. This
   installs the 166 MB model once; it is not part of the repository.
3. Load a track and open the **STEMS** pad page.

Stems need several GB of RAM while separating. Without ONNX Runtime BrockDJ
still builds and runs normally, just without stems.

GPU acceleration is experimental and off by default (**Settings → DJ / Sync →
Stems → Experimental GPU acceleration**, takes effect after a restart). If it
fails, BrockDJ falls back to the CPU.

- **Intel iGPU on Linux:** install the OpenVINO plugin once with
  `python3 scripts/fetch-stems-openvino.py`.
- **macOS:** uses CoreML if the installed ONNX Runtime provides it.

Details are in [docs/architecture/stems.md](docs/architecture/stems.md).

## Platform status

| Platform | Status                       |
| -------- | ---------------------------- |
| Linux    | Primary development platform |
| macOS    | Actively supported           |
| Windows  | Not actively maintained      |

> **Windows is currently de-prioritized.** I do not use Windows in daily development and have very limited access to Windows hardware. Development therefore focuses on Linux and macOS for the time being.

## Building

There are no prebuilt releases yet, so BrockDJ is built from source with plain
CMake. You need:

- a C++23 compiler (GCC 13+, or Clang from a recent Xcode)
- CMake 3.25+ and, optionally, Ninja
- Qt 6.4+ including Qt Shader Tools

Clone the repository with its submodules:

```bash
git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
cd multiplatform-dj-software
```

### Linux

**1. Install the dependencies.**

Fedora:

```bash
sudo dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config curl \
  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtshadertools-devel \
  alsa-lib-devel jack-audio-connection-kit-devel libusb1-devel \
  taglib-devel rubberband-devel sqlcipher-devel fftw-devel
```

Ubuntu 24.04:

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config curl \
  qt6-base-dev qt6-declarative-dev qt6-shadertools-dev qt6-shader-baker libqt6sql6-sqlite \
  qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window \
  libasound2-dev libjack-jackd2-dev libusb-1.0-0-dev \
  libtag1-dev librubberband-dev libsqlcipher-dev libfftw3-dev \
  libfontconfig1-dev libfreetype6-dev libgl1-mesa-dev libxkbcommon-x11-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev
```

**2. Install libKeyFinder.** Fedora and Ubuntu do not package it, so build the
pinned version once (installs to `/usr/local`, asks for `sudo`):

```bash
./scripts/ci/install-libkeyfinder.sh
```

**3. Build and run.**

```bash
./scripts/fetch-onnxruntime.sh   # optional: enables stems
cmake -S . -B build -G Ninja
cmake --build build
./build/bin/BrockDJ
```

### macOS

**1. Install the dependencies** with the Xcode command-line tools
(`xcode-select --install`) and [Homebrew](https://brew.sh):

```bash
brew install cmake ninja pkg-config qt taglib rubberband libkeyfinder sqlcipher librsvg imagemagick
export CMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

**2. Build and run.**

```bash
./scripts/fetch-onnxruntime.sh   # optional: enables stems
cmake -S . -B build -G Ninja
cmake --build build
open build/bin/BrockDJ.app
```

The app is built for your Mac's architecture (Apple Silicon or Intel) and needs
macOS 13 or newer.

### Updating

```bash
git pull
git submodule update --init --recursive
cmake --build build
```

### Build options

| Option | Effect |
| --- | --- |
| *(default)* | `RelWithDebInfo`: optimized, with debug symbols |
| `-DCMAKE_BUILD_TYPE=Release` | Release build without debug symbols |
| `-DBUILD_TESTING=ON` | Also build the test suite |
| `-DBROCKDJ_REQUIRE_STEMS=ON` | Fail configuration if ONNX Runtime is missing |
| `-DBROCKDJ_ENABLE_STEMS=OFF` | Build without stems |

Run the tests in a separate build directory:

```bash
cmake -S . -B build-tests -G Ninja -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

More details, including the FLX10 setup and the CI presets, are in the
[build guide](docs/building.md).

## Documentation

| Topic | |
| --- | --- |
| Building | [docs/building.md](docs/building.md) |
| Stems | [docs/architecture/stems.md](docs/architecture/stems.md) |
| CDJ-style performance features | [docs/architecture/cdj-performance-features.md](docs/architecture/cdj-performance-features.md) |
| Song parts | [docs/architecture/song-parts.md](docs/architecture/song-parts.md) |
| Sampler and sequencer | [docs/architecture/sampler-milestone.md](docs/architecture/sampler-milestone.md) |
| Live features and recording | [docs/architecture/live-features-milestone.md](docs/architecture/live-features-milestone.md) |
| Library search syntax | [docs/architecture/library-search-and-playlists.md](docs/architecture/library-search-and-playlists.md) |
| Packaging | [docs/packaging.md](docs/packaging.md) |
| Dependency pins | [docs/dependencies.md](docs/dependencies.md) |
| All architecture notes | [docs/architecture/](docs/architecture/) |

## License

BrockDJ is licensed under the [GNU Affero General Public License v3.0 or later](LICENSE).
Third-party components and the stem model are listed in [NOTICE](NOTICE).
