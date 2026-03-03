# 🎧 [Name TBD] – Multiplatform DJ Software

> *I don't have a name for this yet — suggestions welcome!*

A modern DJ application built on **JUCE** (Audio/DSP) and **Qt 6 / QML** (UI). Waveform rendering is fully hardware-accelerated via **Qt RHI** directly on the GPU using Vulkan – no CPU software rendering.

---

## ✅ Current Features

| Feature | Details |
|---|---|
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

**Dependencies:** Qt 6, Vulkan, TagLib, CMake ≥ 3.22, C++20 Compiler

    git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
    cd multiplatform-dj-software
    mkdir build && cd build
    cmake ..
    make -j$(nproc)
    ./bin/MultiPlatformDJ

> JUCE is included as a submodule under `libs/JUCE/` – no separate installation required.

---

## 🗺 Roadmap

- [ ] Jogwheel / Scratch mechanics
- [ ] Mixer Section (Crossfader, EQ, Gain)
- [ ] Save & recall Hot Cues (Performance Pads)
- [ ] MIDI Controller Mapping
- [ ] Audio Streaming Integration

---

*Built with ❤️ on Linux | JUCE + Qt 6 + Vulkan RHI*