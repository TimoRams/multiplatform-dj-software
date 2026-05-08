# 🎧 [Name TBD] – Multiplatform DJ Software

> *I don't have a name for this yet — suggestions welcome!*

A modern DJ application built on **JUCE** (Audio/DSP) and **Qt 6 / QML** (UI).
Waveform rendering is fully hardware-accelerated via **Qt RHI** on the GPU using Vulkan — no CPU software rendering.

---

## ✅ Features

### Playback & Transport

| Feature | Details |
| --- | --- |
| **Dual-Deck Playback** | Two independent decks — FLAC / WAV / OGG / MP3 |
| **4-Band RGB Waveforms** | GPU-rendered (low / low-mid / mid / high) with beat grid overlay |
| **BPM & Key Detection** | Autocorrelation + Krumhansl-Schmuckler; manual override and beat-grid correction |
| **Scratch & Jog** | Velocity-based scratch with spring-damped physics; up to 12× playback rate |
| **Loop Controls** | Set in/out points, 4-beat toggle, halve / double length, beat-quantized |
| **Hot Cues** | 8 cue points per deck with color, label, and database persistence |
| **Slip Mode** | Loops and reverses run silently while the playhead continues; restores on exit |
| **Sync & Quantize** | Master/follower deck sync with beat-phase nudge; quantize-aware cue triggers |
| **Key Lock** | Pitch-preserved time-stretching via RubberBand during tempo changes |
| **Reverse Playback** | Full reverse audio streaming, scratch-compatible |
| **Turntable FX** | Vinyl Brake, Backspin, Echo Out, Roll Out |

### Mixer & Effects

| Feature | Details |
| --- | --- |
| **3-Band EQ & Filter** | Per-deck High / Mid / Low EQ, resonant filter, trim / gain staging |
| **Crossfader** | Adjustable curve (smooth to sharp) with per-deck A / B assignment |
| **VU Metering** | Pre-fader metering per deck with peak hold and clip indicator |
| **24 Effect Types** | 2 FX units + Sound Color knob: Reverb, Echo, Flanger, Phaser, Bitcrusher, PitchShifter, Stretch, Spiral, Roll, Mobius and more |
| **Performance Pads** | 4×2 grid — Hot Cue, Pad FX, Beatjump, Sampler *(coming soon)* |

### Library

| Feature | Details |
| --- | --- |
| **Library Management** | 3-column panel, folder tree navigation, drag & drop to decks |
| **Persistent Analysis** | SQLite-backed BPM / key analysis cache; background analysis worker |
| **Metadata Extraction** | ID3v2 / Vorbis / M4A + filename fallback |
| **Cover Art** | Extracted via TagLib (MP3, FLAC, MP4, OGG, WAV) |

### Integration & Sync

| Feature | Details |
| --- | --- |
| **Ableton Link** | Network tempo and beat-phase sync with other Link-enabled apps |
| **MIDI Control** | Full MIDI input/output binding, MIDI learn mode, persistent controller mappings |
| **AV-Sync** | Hardware latency compensation + sub-frame visual interpolation |

---

## 🛠 Architecture

    JUCE Audio Thread  →  WaveformAnalyzer Thread  →  Qt Main Thread / QML
    (Real-time audio)     (BPM, Waveform Bins)        (UI, Signals, Rendering)
                                                             ↓
                                                       Qt RHI → Vulkan (GPU)

All cross-thread communication goes through Qt Queued Connections — zero direct cross-thread access.
The playhead position is shared via `std::atomic<double>` for wait-free VSync-frame reads by the render thread.

---

## 🐧 Linux-First

- Native Wayland support (Vulkan, no XWayland required)
- PipeWire / ALSA out-of-the-box without extra configuration
- All dependencies available via standard package managers

---

## 💻 Build Instructions

### Required dependencies

- Qt 6 (Core, Gui, Qml, Quick, Quick3D, Sql)
- TagLib
- libkeyfinder
- RubberBand
- CMake >= 3.22
- C++23 compiler

> JUCE and Ableton Link are included as submodules under `libs/` — no separate installation required.

### Linux (Vulkan + ALSA)

    sudo apt update
    sudo apt install -y \
      build-essential cmake pkg-config \
      qt6-base-dev qt6-declarative-dev qt6-quick3d-dev \
      libtag1-dev libkeyfinder-dev librubberband-dev libasound2-dev

    git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
    cd multiplatform-dj-software
    cmake -S . -B build
    cmake --build build -j$(nproc)
    ./build/bin/RamsbrockDJ

Target-native SIMD is enabled automatically on Linux, Windows x64, and Intel macOS. Override explicitly with:

    cmake -S . -B build -DRDBJ_ENABLE_NATIVE_SIMD=ON   # force on
    cmake -S . -B build -DRDBJ_ENABLE_NATIVE_SIMD=OFF  # force off

### macOS (Metal backend)

    brew install cmake pkg-config qt@6 taglib rubberband libkeyfinder

    git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
    cd multiplatform-dj-software
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
    cmake --build build -j
    ./build/bin/RamsbrockDJ

On Apple Silicon, build the arm64 target natively to get the platform's SIMD path automatically.

### Windows (Vulkan backend)

    vcpkg install taglib rubberband libkeyfinder

    cmake -S . -B build ^
      -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/msvcxxxx_64

    cmake --build build --config Release

---

## License

Licensed under the **GNU Affero General Public License v3.0 or later** (AGPL-3.0-or-later).

- Full license text: `LICENSE`
- Copyright and third-party notices: `NOTICE`

---

## 🗺 Roadmap

- [ ] Create roadmap

---

Built with love on Linux | JUCE + Qt 6 + Vulkan RHI
