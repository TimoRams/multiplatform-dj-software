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

* Four-deck playback with FLAC, WAV, OGG, and MP3 support
* GPU-rendered RGB waveforms, beat grids, loops, hot cues, and sync
* Read-only Rekordbox USB browsing (legacy and OneLibrary), imported dynamic
  beatgrids and cached analysis previews; built-in beat analysis remains static
* Three-band EQ, filter, crossfader, and built-in effects
* Library browsing, metadata extraction, cover art, and background analysis
* MIDI mapping, controller integration, and Ableton Link support
* Headphone-only library preview, split cue and mono master options
* Load lock, track-end warning, Auto Cue, editable memory cues and armed saved loops
* Stereo 24-bit WAV set recording with track marks and bounded background writing
* Independent sampler: four banks of eight slots, one-shots, quantized sync loops,
  deck-loop/beat capture, local sample storage and pre-fader headphone cue
* Sample sequencer: 16 beat-synced steps across eight sample tracks per bank, with
  adjustable swing and persistent patterns

The interface uses angular controls and subdued, translucent slate panels.
In **Settings > DJ / Sync > Playhead position**, the white scrolling-waveform
playhead can be moved from the center (50%) toward the left (20%) to show more
upcoming audio. The preference applies to all decks, including slip previews,
and is restored on restart.

Crossfader position, curve/sharpness and per-deck A/THRU/B assignments are
restored on restart, including changes made from a controller while the
development controls are closed.

## Live controls and recording

Open **Settings > Live / Recording** for load protection, split cue, mono master,
Auto Cue, track-end warning and the set recorder. Recording uses the final
software master; headphone preview is excluded. Empty recording-folder input
uses `Music/BrockDJ/Recordings`. Track Mark starts a new numbered WAV part.
External sources mixed exclusively inside a controller need a recording return
input; they cannot be captured from the software master alone.

Pull down the top menu to start/stop recording and see its HH:MM:SS timer in the
same button. The saved recording directory applies to this shortcut and MIDI too.
Stopping finalizes the file in the background; wait for “SAVING” to finish before
starting another recording.

The **Sampler** top-bar tab opens its own full page without stopping the decks.
Banks A–D each contain eight slots; deck SAMPLER pads and FLX10 sampler pads use
the same selected bank. Use EDIT to load/trim a file or capture a deck loop/beat
range. The Library context menu and drag-and-drop also open the sample editor.
Samples are copied into local storage (up to 30 seconds each, 256 MiB total PCM).
One-shots retrigger; loops toggle. At most eight voices play simultaneously;
occupied loop voices are never stolen. CUE is independent of the sampler level.
Pads display waveform/progress and queued-start countdowns; bank buttons show
activity in other banks. The editor adds frame-accurate trim, zoom and private
headphone preview, with fixed Save/Cancel actions and errors. FLX10 Sampler CUE
and 14-bit Sampler Volume use the same state as the UI, with level pickup.
The **FX** tab provides Touch FX and FX Settings for both existing FX units:
effect choice, A–D routing, wet/dry, BPM sync, quantization, beat division, Reverb
room size and Sound Color character. Touch FX uses a momentary XY surface; release,
page changes and window deactivation restore the unit's previous settings.

Sampler program reaches the master/booth and recorder; headphone-only audio does
not. Sync uses the active master/Link or a playing deck, then an internal clock.

Memory cues are also reachable from the performance deck side panel. Name/delete
cues and arm saved loops in Live / Recording. Imported USB performance edits are
stored in BrockDJ's local library, leaving the original device database untouched.

See [live milestone behavior and tests](docs/architecture/live-features-milestone.md).
See [sampler implementation and remaining roadmap](docs/architecture/sampler-milestone.md).
See [live sampler editing and FLX10 recovery](docs/architecture/sampler-live-editing.md).

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
  qt6-base-dev qt6-declarative-dev qt6-shadertools-dev qt6-shader-baker \
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

DDJ-FLX10 jog displays use macOS's shared IOHID interface alongside the installed
vendor audio/MIDI driver, without libusb, USB seizure or driver detachment.
This is not a proprietary driver API integration or hardware-verified support:
the driver must expose the screen HID service and activate the device.
See [FLX10 setup and limitations](docs/building.md#ddj-flx10-jog-displays-on-macos).

## Documentation

* [Complete build guide](docs/building.md)
* [Architecture](docs/architecture/)
* [Packaging](docs/packaging.md)
* [Dependency pins](docs/dependencies.md)

## License

Licensed under the [GNU Affero General Public License v3.0 or later](LICENSE).
