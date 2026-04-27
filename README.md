# 🎧 [Name TBD] – Multiplatform DJ Software

> *I don't have a name for this yet — suggestions welcome!*

A modern DJ application built on **JUCE** (Audio/DSP) and **Qt 6 / QML** (UI). Waveform rendering is fully hardware-accelerated via **Qt RHI** directly on the GPU using Vulkan – no CPU software rendering.

---

## ✅ Current Features

| Feature | Details |
| --- | --- |
| **Dual-Deck Playback** | Two independent decks supporting FLAC / WAV / OGG / MP3 |
| **3-Band Waveforms** | GPU-rendered waveforms with beatgrid overlay |
| **BPM & Key Detection** | Autocorrelation + Krumhansl-Schmuckler, Harmonic Mixing Format |
| **Metadata Extraction** | ID3v2 / Vorbis / M4A + filename fallback |
| **Cover Art** | Extracted via TagLib (MP3, FLAC, MP4, OGG, WAV) |
| **Library Management** | 3-column panel, folder tree navigation, drag & drop to decks |
| **Performance Pads UI** | 4×2 grid, 4 mode tabs (Hot Cue, Pad FX, Beatjump, Stems) |
| **AV-Sync** | Hardware latency compensation + sub-frame visual interpolation |

---

## 🛠 Architecture (Brief)

    JUCE Audio Thread  →  WaveformAnalyzer Thread  →  Qt Main Thread / QML
    (Real-time audio)     (BPM, Waveform Bins)        (UI, Signals, Rendering)
                                                             ↓
                                                       Qt RHI → Vulkan (GPU)

These three threads communicate exclusively via Qt Queued Connections – zero direct cross-thread access ensures audio stability.

---

## 🐧 Linux-First

- Native Wayland support (Vulkan, no XWayland required)
- PipeWire / ALSA out-of-the-box without extra configuration
- All dependencies available via standard package managers

---

## 💻 Build Instructions

### Required dependencies (all platforms)

- Qt 6 (Core, Gui, Qml, Quick, Quick3D, Sql)
- TagLib
- libkeyfinder
- RubberBand
- CMake >= 3.22
- C++23 compiler

### Linux (Vulkan + ALSA)

Example dependency install (Debian/Ubuntu):

    sudo apt update
    sudo apt install -y \
      build-essential cmake pkg-config \
      qt6-base-dev qt6-declarative-dev qt6-quick3d-dev \
      libtag1-dev libkeyfinder-dev librubberband-dev libasound2-dev

Build:

    git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
    cd multiplatform-dj-software
    cmake -S . -B build
    cmake --build build -j$(nproc)
    ./build/bin/RamsbrockDJ

Target-native SIMD code generation is enabled automatically on Linux, Windows x64, and Intel-based macOS builds. You can still force it on or off explicitly with:

    cmake -S . -B build -DRDBJ_ENABLE_NATIVE_SIMD=ON
    cmake -S . -B build -DRDBJ_ENABLE_NATIVE_SIMD=OFF

That lets the compiler emit ISA-specific code such as AVX2 on capable x86_64 systems, while Apple Intel builds use the matching x86 SIMD path.

### macOS (Metal backend)

Example dependency install (Homebrew):

    brew install cmake pkg-config qt@6 taglib rubberband libkeyfinder

Build:

    git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
    cd multiplatform-dj-software
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
    cmake --build build -j
    ./build/bin/RamsbrockDJ

On Apple Silicon, build the arm64 target natively to get the platform's SIMD path automatically; keep universal builds off if you want the compiler to specialise for one CPU family.

### Windows (Vulkan backend)

Recommended: install Qt 6 (with Quick/QML/Quick3D) via Qt Online Installer and use vcpkg for C/C++ dependencies.

Example vcpkg dependency set:

    vcpkg install taglib rubberband libkeyfinder

Then configure using the vcpkg toolchain and your Qt 6 path:

    cmake -S . -B build ^
      -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/msvcxxxx_64

    cmake --build build --config Release

> JUCE is included as a submodule under `libs/JUCE/` – no separate installation required.

---

## License

This project is licensed under the GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later).

- Full license text: LICENSE
- Copyright and project notices: NOTICE

License and third-party notice details are documented in NOTICE.

---

## 🗺 Roadmap

- [ ] Create roadmap

---

Built with love on Linux | JUCE + Qt 6 + Vulkan RHI
