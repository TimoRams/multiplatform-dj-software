# 🎧 BrockDJ

> A modern, multiplatform DJ application — GPU-accelerated waveforms, professional mixing, and deep hardware integration.
>
> **by [Ramsbrock.net](https://ramsbrock.net)**

![License](https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square)
![C++](https://img.shields.io/badge/C%2B%2B-26-informational?style=flat-square)
![Qt](https://img.shields.io/badge/Qt-6-41CD52?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey?style=flat-square)

Built on **JUCE** for real-time audio and **Qt 6 / QML** for the UI.
Waveforms are rendered entirely on the GPU via **Qt RHI** (Vulkan on Linux/Windows, Metal on macOS) — no CPU software rendering.

---

## Features

### Playback & Transport

- Dual-deck playback — FLAC / WAV / OGG / MP3
- 4-band RGB waveforms (GPU-rendered) with beat grid overlay
- BPM & key detection — autocorrelation + Krumhansl-Schmuckler; manual override and beat-grid correction
- Scratch & jog with spring-damped physics, up to 12× playback rate
- Loop controls — set in/out, 4-beat toggle, halve / double length, beat-quantized
- 8 hot cues per deck with color, label, and database persistence
- Slip mode — loops and reverses run silently while the playhead continues
- Master / follower sync with beat-phase nudge correction + quantize-aware cue triggers
- Key lock — pitch-preserved time-stretching via RubberBand
- Reverse playback, scratch-compatible
- Turntable FX: Vinyl Brake, Backspin, Echo Out, Roll Out

### Mixer & Effects

- 3-band EQ + resonant filter per deck, trim / gain staging
- Crossfader with adjustable curve (smooth to sharp) and per-deck A / B assignment
- Pre-fader VU metering with peak hold and clip indicator
- 24 effect types across 2 FX units + Sound Color centre knob
- Performance pads — 4×2 grid, 4 modes: Hot Cue, Pad FX, Beatjump, Sampler *(coming soon)*

### Library

- Folder tree navigation with drag & drop to decks
- SQLite-backed BPM / key analysis cache + background analysis worker
- Metadata extraction: ID3v2 / Vorbis / M4A + filename fallback
- Cover art: MP3, FLAC, MP4, OGG, WAV

### Integration

- **Ableton Link** — network tempo and beat-phase sync with other Link-enabled apps
- MIDI — full input/output binding, learn mode, persistent controller mappings
- Hardware latency compensation + sub-frame visual interpolation

---

## Architecture

```text
JUCE Audio Thread  →  WaveformAnalyzer Thread  →  Qt Main Thread / QML
(Real-time audio)     (BPM, Waveform Bins)        (UI, Signals, Rendering)
                                                         ↓
                                                   Qt RHI → Vulkan / Metal (GPU)
```

All cross-thread communication goes through Qt Queued Connections — zero direct cross-thread access.
The playhead position is exposed to the render thread via `std::atomic<double>` for wait-free VSync-frame reads.

---

## Build

### Dependencies

| Library | Purpose |
| --- | --- |
| Qt 6 (Core, Gui, Qml, Quick, Quick3D, Sql) | UI & rendering |
| TagLib | Metadata extraction |
| libkeyfinder | Key detection |
| RubberBand | Key lock / time-stretch |
| CMake ≥ 3.22 | Build system |

> **JUCE** and **Ableton Link** are included as submodules under `libs/` — no separate installation required.

---

<details>
<summary>🐧 Linux — Vulkan + ALSA/PipeWire</summary>

Install dependencies (Debian/Ubuntu):

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  qt6-base-dev qt6-declarative-dev qt6-quick3d-dev \
  libtag1-dev libkeyfinder-dev librubberband-dev libasound2-dev
```

Build:

```bash
git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
cd multiplatform-dj-software
cmake -S . -B build
cmake --build build -j$(nproc)
./build/bin/BrockDJ
```

</details>

<details>
<summary>🍎 macOS — Metal</summary>

Install dependencies (Homebrew):

```bash
brew install cmake pkg-config qt@6 taglib rubberband libkeyfinder
```

Build:

```bash
git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
cd multiplatform-dj-software
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
cmake --build build -j
./build/bin/BrockDJ
```

> On Apple Silicon, build the arm64 target natively to get the platform's SIMD path automatically.

</details>

<details>
<summary>🪟 Windows — Vulkan</summary>

Install dependencies via vcpkg:

```bash
vcpkg install taglib rubberband libkeyfinder
```

Configure and build:

```bat
cmake -S . -B build ^
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/msvcxxxx_64

cmake --build build --config Release
```

</details>

---

### SIMD

Target-native SIMD is enabled automatically on Linux, Windows x64, and Intel macOS. Override if needed:

```bash
cmake -S . -B build -DRDBJ_ENABLE_NATIVE_SIMD=ON   # force on
cmake -S . -B build -DRDBJ_ENABLE_NATIVE_SIMD=OFF  # force off
```

---

## License

Licensed under the **GNU Affero General Public License v3.0 or later** (AGPL-3.0-or-later).
See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE) for details.

---

## 🗺 Roadmap

- [ ] Create roadmap

---

Built with love on Linux · JUCE + Qt 6 + Vulkan RHI
