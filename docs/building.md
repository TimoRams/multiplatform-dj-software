# Building BrockDJ

BrockDJ uses CMake 3.25 or newer, Ninja and a portable C++23 baseline. JUCE,
Ableton Link, Signalsmith DSP, Signalsmith Linear and Signalsmith Stretch are
Git submodules. Clone a new checkout with:

```bash
git clone --recurse-submodules \
  https://github.com/TimoRams/multiplatform-dj-software.git
```

For an existing checkout, synchronize and initialize the recorded pins:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

Submodules are pinned to exact commits. A normal `git pull` does not update
their working trees; after a pull that changes a Gitlink, run the two commands
above. `git submodule update --remote` is not part of the normal build process.

Signalsmith Stretch is the default production key-lock/time-stretch backend;
Rubber Band remains a selectable compatibility backend and is therefore still
a required build dependency. Signalsmith Linear is Stretch's technical
dependency. All three Signalsmith projects are MIT-licensed; their canonical
license files remain in the pinned submodules.

The configured CI matrix covers Linux x86_64, Linux ARM64, macOS Apple
Silicon, macOS Intel and Windows x64. Each platform job is configured to
build, test, deploy and smoke-test its package on pull requests, main-branch
pushes, version tags and manual dispatches. This matrix is not a claim that
all platform runs have passed. Package artifacts are uploaded only for
main-branch pushes, version tags and manual dispatches.

Qt 6.4 remains supported. Explicit Vulkan pipeline-cache load/save files are
enabled only with Qt 6.5 or newer; Qt 6.4 retains the same Vulkan rendering
configuration without those optional APIs. The embedded `DJSoftware` module
requires an explicit `qrc:/` import root on Qt 6.4. Numeric font
typographic metrics are applied only when Qt exposes the optional
`preferTypoLineMetrics` property (Qt 6.8 or newer). Linux USB mount/eject operations
set interactive authorization on the D-Bus message itself, an API available
in Qt 6.4, rather than requiring a newer interface convenience method.
The waveform raster worker uses
`std::thread` with explicit stop/wake/join so it does not require Apple's
libc++ to provide `std::jthread` or `std::stop_token`.
Frosted sidebars use Qt ShaderTools' `qsb` at build time; the compiled shader
resources support Qt 6.4's ShaderEffect (no QtQuick.Effects dependency). The
shipping application does not need the shader compiler. Qt's online installer
needs the **Qt Shader Tools** (`qtshadertools`) module; Homebrew's `qt@6` includes
it, and Arch Linux provides `qt6-shadertools`.

## Linux

Ubuntu 24.04 dependencies:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake curl ninja-build ccache pkg-config \
  qt6-base-dev qt6-declarative-dev qt6-shadertools-dev qt6-shader-baker libqt6sql6-sqlite \
  qml6-module-qtqml qml6-module-qtqml-models \
  qml6-module-qtqml-workerscript qml6-module-qtquick \
  qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window \
  libasound2-dev libjack-jackd2-dev libusb-1.0-0-dev \
  libtag1-dev librubberband-dev libsqlcipher-dev libfftw3-dev \
  libfontconfig1-dev libfreetype6-dev libgl1-mesa-dev \
  imagemagick libx11-dev libxcomposite-dev libxcursor-dev libxext-dev librsvg2-bin \
  libxinerama-dev libxrandr-dev libxrender-dev libxkbcommon-x11-dev
```

Qt's development packages do not install every runtime plugin when using
`--no-install-recommends`, as ARM64 CI does. Install the explicit QML modules
above, including QtQuick.Templates (required by Controls) and QtQml.Models,
along with `libqt6sql6-sqlite` for the QSQLITE driver used by the database-worker
tests. Linking Qt SQL or SQLCipher alone does not supply that driver. These
packages retain Ubuntu 24.04's system Qt 6.4.2 baseline.
The application and CI smoke engine share initialization of the embedded
`qrc:/` module import root. App-enabled builds register `ci_runtime_smoke`
with CTest to exercise the complete `--ci-smoke-test` entrypoint, including
its QML/database preflight and both isolated window startup/close paths.
The optional Qt 6.8 typographic font metrics are applied only when available,
so older Qt versions can still load the deck display.

Ubuntu does not provide the required `libkeyfinder-dev` package on the CI
images. The pinned `libkeyfinder` 2.2.8 source is built against FFTW3 with:

```bash
./scripts/ci/install-libkeyfinder.sh
```

The script verifies the source archive SHA-256 before building and installing
it under `/usr/local`.

For normal local development, use the app-only build. It does not compile test
binaries, so incremental UI/audio work stays fast:

```bash
./build-fast
./build/bin/BrockDJ
```

Tests use their own `build-tests/` directory and never invalidate the app
build. The first test invocation has its own one-time dependency configure;
later runs are incremental. `BUILD_TESTING=ON` also requires Qt Test (included
in the Qt base development packages above). The QML component test uses it
for native mouse/touch event delivery and defaults to offscreen software rendering.
Run the complete local suite with:

```bash
./test-fast
```

CI-equivalent native release builds use one of these presets, according to the
host architecture:

```bash
cmake --preset ci-linux-x64
cmake --build --preset ci-linux-x64 --parallel 2
ctest --preset ci-linux-x64
```

```bash
cmake --preset ci-linux-arm64
cmake --build --preset ci-linux-arm64 --parallel 2
ctest --preset ci-linux-arm64
```

The CI-only `ci-linux-sanitizers` preset uses a separate
`build-ci-linux-sanitizers/` directory and enables the project's
`BROCKDJ_ENABLE_SANITIZERS` ASan/UBSan option. Its test preset runs the
focused startup-close, deck-audio-graph, audio-cache, waveform-cache, motion,
control-clock, neutral waveform analysis, database-worker and media-I/O
scheduler correctness tests. The sanitizer lane also builds BrockDJ and runs
`--ci-smoke-test`, which checks QML/database setup and launches the real
startup-close and early-close subprocesses. Performance- and pacing-sensitive
tests are excluded. It is not a shipping build.

The ARM64 preset is a native preset. It is not a cross-compilation toolchain;
run it on an ARM64 host. The configured CI target is Ubuntu 24.04 ARM64
(`ubuntu-24.04-arm`). It is not a validation of Raspberry Pi 4/5 hardware or
of Raspberry Pi OS.

Raspberry Pi 4 and 5 require a 64-bit OS for an ARM64 build, but neither
generation is currently claimed as hardware-validated. Raspberry Pi OS
Bookworm's GCC 12 and system Qt 6.4 are not an established build/runtime
combination: the project requires a C++23 standard library with
`std::expected`, and the minimum compiler/library support is checked during
CMake configuration. Use GCC 13 or newer and Qt 6.4 or newer only where the
configure checks pass, and validate the resulting package on the exact Pi OS
image before treating it as supported. Ubuntu 24.04 ARM64 CI does not establish
Bookworm ABI or package compatibility.

Linux ARM64 leaves graphics-backend selection to Qt by default; desktop Linux
retains its Vulkan default. `BROCKDJ_RHI_BACKEND=auto`, `opengl` or `vulkan`
selects the desired policy explicitly. An existing `QSG_RHI_BACKEND` selection
is respected when no BrockDJ override is set. Verify the actual backend on the
target display/driver rather than assuming software-rendered CI proves it.

The normal ARM64 configuration uses portable code generation and does not
apply CPU-specific tuning. For builds that stay on a machine where they were
compiled, native CPU tuning can be enabled explicitly:

```bash
cmake --preset linux-release -DBROCKDJ_ENABLE_NATIVE_ARCH=ON
cmake --build --preset linux-release --parallel 2
```

ARM64 builds use a 16 MiB scrolling-waveform CPU cache per active deck instead
of the 48 MiB desktop default, limiting the four-deck worst case to 64 MiB.
`BROCKDJ_WAVEFORM_CACHE_MB` can override the per-deck budget from 4 to 256 MiB
when testing a specific display resolution and memory size. This affects only
reusable rendered tiles; source analysis data and audio cache correctness are
unchanged.

## Read-only Rekordbox USB libraries

Device browsing supports both legacy `PIONEER/rekordbox/export.pdb` and encrypted
`PIONEER/rekordbox/exportLibrary.db` (OneLibrary, formerly Device Library Plus).
SQLCipher is required for OneLibrary browsing, not just device names and colours.
When both exist, OneLibrary is preferred; an unreadable or unsupported modern
index falls back to `export.pdb` with an explicit diagnostic in the device status.
A modern-only unsupported schema reports an error rather than an empty library.

The supported modern schema uses `content`, `artist`, `album`, `genre`, `key`,
`color`, `image`, `playlist` and `playlist_content`, as documented by
[pyrekordbox's OneLibrary format reference](https://github.com/dylanljones/pyrekordbox/blob/master/docs/source/formats/onelibrary.md).
Track metadata, millisecond durations, BPM × 100, device-relative audio/ANLZ/artwork
paths, nested folders and exported playlist membership/order are read directly.
Smart playlists use their exported membership; rules are not evaluated locally.
Unknown playlist attributes, missing required columns, invalid IDs, dangling
playlist membership and invalid/cyclic folder hierarchies report errors.
Missing audio remains visible but unavailable; paths cannot escape the USB root.

Access is strictly read-only (`mode=ro&immutable=1`, `query_only`, in-memory SQL
temporary storage); BrockDJ never updates the index or creates database sidecars
on the USB. Mount only completed, quiescent exports: immutable reading assumes
the exporter is not concurrently editing the library. This does not add USB
export/write support. Nonempty WAL/recovery sidecars are rejected rather than
silently ignored; finish exporting and safely eject the medium first.
The `rekordbox_read_only` CTest includes encrypted synthetic
OneLibrary fixtures and verifies metadata, ordering, discovery, failure handling,
legacy fallback, path containment and unchanged USB file bytes/sidecar absence.
Hardware/export-version coverage beyond the documented schema is not claimed.

## macOS

Install Xcode command-line tools, CMake/Ninja, Qt and the native audio-analysis
dependencies:

```bash
brew install cmake ninja ccache pkg-config qt@6 taglib rubberband libkeyfinder sqlcipher librsvg imagemagick
```

Expose Qt to CMake if Homebrew did not do so already:

```bash
export CMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
```

Then select the architecture-native preset:

```bash
cmake --preset ci-macos-arm64
cmake --build --preset ci-macos-arm64 --parallel 2
ctest --preset ci-macos-arm64
```

```bash
cmake --preset ci-macos-x86_64
cmake --build --preset ci-macos-x86_64 --parallel 2
ctest --preset ci-macos-x86_64
```

Both presets create a real `BrockDJ.app` bundle under the preset's `bin/`
directory. The architecture is explicit through `CMAKE_OSX_ARCHITECTURES`, and
the supported deployment baseline is macOS 13.0.

For local macOS builds without Ninja, use the matching Makefiles fallback
preset:

```bash
cmake --preset macos-dev-x86_64-make
cmake --build --preset macos-dev-x86_64-make --parallel 4
```

### DDJ-FLX10 jog displays on macOS

Keep the vendor's macOS audio/MIDI driver installed and enabled. BrockDJ uses
Apple's system IOKit/CoreFoundation frameworks for the **shared** IOHID screen
service and its existing JUCE/CoreMIDI backend for the session SysEx and
keepalive. No additional dependency, root access, libusb installation, kernel
extension or driver replacement is required. It never opens/seizes the whole
USB device, changes its configuration, detaches a driver, or claims the
audio/MIDI USB interfaces. This is **not** an implementation of a proprietary
vendor-driver API.

1. Connect one FLX10, allow its vendor driver to initialize, and close other DJ
   applications which may own the screen service.
2. Select the built-in DDJ-FLX10 MIDI mapping and enable DDJ-FLX10 controller
   support in Settings. Its dedicated display session uses the primary
   FLX10 CoreMIDI output, not MIDI/port 2.
3. Use Refresh after changing the connection or permissions. While enabled,
   disconnected/failed sessions are also retried every five seconds.
4. If shared HID access is denied, check the logged IOKit error and macOS
   Privacy & Security permissions where applicable. Do not uninstall the
   audio driver or run BrockDJ as root to work around this.

The built-in MIDI mapping also handles all four channel CROSSFADER ASSIGN
switches. A routes to the left side, B to the right, and THRU bypasses the
crossfader only (not the channel fader). Active Note On messages select the
position; the two inactive notes sent with it do not change the assignment.
Position, curve/sharpness and assignments persist even with the development
controls closed; opening those controls mirrors the current mixer state.
Custom mappings can learn each A/THRU/B action in the Mapping Editor.

The switch notes (`16` = A, `1D` = THRU, `18` = B; active value `7F`) follow
the [official FLX10 MIDI chart](https://downloads.support.alphatheta.com/software_info/dj-controllers/DDJ-FLX10/DDJ-FLX10_MIDI_Message_List_E1.pdf).
Statuses `90` through `93` correspond to decks 1 through 4, as also used by the
[community FLX10 mapping](https://github.com/Loui1979/MIXXX-FLX10/blob/main/Controllers/Pioneer-DDJ-FLX10.midi.xml#L1724-L1847).
The official chart's M11 channel column prints only `1`; the per-channel
expansion is corroborated by that community mapping, not by hardware captures.
Real-device switch behavior still requires hardware acceptance testing.

The TEMPO RESET lamp (the physical button also used for shifted TEMPO RANGE)
lights within +/-0.05 percentage points of zero tempo, rather than requiring
an exact floating-point zero. SHIFT + SLIP REVERSE uses dedicated note `38`
on deck channels 1-4 to toggle persistent reverse with Slip disabled; ordinary
SLIP REVERSE remains momentary. A custom mapping sending SHIFT plus ordinary
SLIP REVERSE on decks A/B is also supported.

### FLX10 dual-computer channel input ownership

The official MIDI message list defines INPUT SELECT (M17) per channel on
`9n 55/56/57` (n = 0-3): `55`=7F is PC-A, `56`=7F is LINE/PHONO, `57`=7F is
PC-B. It defines **no** message that tells a computer which USB port (A or B)
it is plugged into, and neither the official list nor the public
Loui1979/MIXXX-FLX10 mapping contains one. BrockDJ therefore never guesses:

- Settings > Controllers > "This computer is on" (Unknown / USB A / USB B,
  default Unknown) states the port.
- LINE/PHONO always means another source. PC-A/PC-B is resolved only when the
  port is configured; with Unknown the channel is left completely untouched.
- A channel on another source is paused, refuses play, is removed from master
  and cue (5 ms ramp, FX tails included) and its deck is dimmed with an
  EXTERNAL INPUT overlay. Returning to our computer restores audio but does
  not resume playback.
- Selector positions are learned only from reports (no documented query), so
  each channel is unknown until its switch is moved once after connecting;
  they reset to unknown when the MIDI input closes.

Sound Color starts with no effect selected. Turning a Color knob alone cannot
activate a filter or another Color effect. Pressing a mode selects only that
mode; pressing it again deselects it. Mode changes retain each deck's bipolar
knob position, clear the previous native filter, and update all four engines.
The hardware mode notes (`96 00` through `96 05`) carry latched selection
state: velocity `7F` selects, velocity `00` deselects that mode. OFF for a
previous mode never clears a newer selection; repeated ON reports are idempotent.
LEVEL/DEPTH and Beat FX remain independent of this selection.
All four FLX10 Color knobs use MIDI status `B6`, with MSB/LSB pairs
`17/37`, `18/38`, `19/39`, and `1A/3A` for channels 1-4 respectively.

Beat FX ON uses the unit's enabled state independently of LEVEL/DEPTH.
Engaging preserves the mix amount exactly, including zero and near-zero values;
only the mix control changes the depth, never the ON button.
Applying a hardware command suppresses intermediate mixer-state feedback until
routing, amount and enable have been applied, so the first ON does not get
cancelled by the previous disabled state. Beat timing follows current track
BPM (including imported tempo changes), not just tempo-fader movement.
For Master, timing uses the first playing deck, then the first loaded deck.
Reverb is not a beat-duration effect: BEAT minus/plus and the software FX
selectors use room-character steps of 1, 10, 25, 50, 75, 90 and 100 percent.
The percentage controls BrockDJ's reverb room size/damping/width, not the
wet/dry amount or a calibrated duration in seconds. LEVEL/DEPTH remains
independent. The performance FX panel shows percent instead of milliseconds
and restores its open/closed state from the UI settings across launches.

The official MIDI chart does not document host output for the separate Beat
FX LCD's numeric BPM, beat division, effect name or parameter/time fields.
Input-only effect/channel selector notes are not echoed as supposed LCD
commands. Jog-display BPM messages are a separate protocol and must not be
used for that LCD. This does not establish that LCD feedback is impossible;
it remains unsupported until a verified protocol is available.
The built-in mapping retains the previously hardware-observed ON/OFF note
`94 46` and its latched 127/0 reports; the official chart instead lists
`94 47`, so that discrepancy still needs device/firmware-level verification.

Only VID:PID `2B73:0041`, USB interface **5**, with the expected unnumbered
128-byte output descriptor is accepted. The interface number is also checked
on registry parents for macOS versions which omit it from IOHID properties.
Multiple matching controllers, missing/incompatible descriptors, hidden HID,
ambiguous MIDI ports and exclusive opens are rejected with actionable status
messages rather than guessing an interface. The deck byte (`10`, `20`, etc.)
is payload, **not** a report ID: IOHID receives report ID 0 plus exactly the
original 128 bytes, with no Linux-hidraw zero prefix.

The existing bounded FIFO/latest-state coalescing writer is shared with Linux.
At most two consecutive status packets precede an ordered upload packet when
both queues have work. This keeps track initialization, cover segments and
waveform uploads moving even when macOS HID completions are slower than the
200 Hz status producer. Ordered initialization/cover failures end the session
instead of silently dropping a required packet and leaving an unloaded screen.
macOS submits one asynchronous IOHID output at a time with an 8 ms driver
timeout and a bounded completion wait. Missing completions terminate the
session; the report buffer remains alive until the device is closed. Shutdown
unschedules on the writer, joins it, then closes the HID service. Repeated
timeouts and disconnects release the session so reconnect can retry.
`BROCKDJ_FLX10_DIAGNOSTICS=1` enables its existing transfer counters.

Track-load initialization no longer waits for waveform analysis. A temporary
analysis reset keeps the loaded display alive; track/session epochs reject
late worker output after replacement, eject or reconnect. Initialization waits
for enough queue space for the entire header/cover sequence and retries on
the state timer. The log distinguishes an initialization being **queued**
from a track-load report or complete cover actually **delivered** by HID;
neither is a firmware acknowledgement.

**Limitations:** native macOS builds and packet/descriptor tests do not verify
real screen rendering. A successful output completion is not a firmware-level
acknowledgement. The macOS path deliberately does **not** issue the Linux
device-wide vendor-unlock sequence; activation is left to the installed
driver. A driver that hides interface 5 or firmware that still needs further
activation cannot be supported by this shared-HID path. Audio/MIDI can keep
working even when displays are unavailable. There is no fallback that seizes
USB or interferes with the vendor driver.

Hardware acceptance still requires checking cover art, waveforms, scratching,
idle keepalive, unplug/replug, Refresh and shutdown while vendor-driver audio
and MIDI remain operational. The focused native regression tests can be run
with `ctest --test-dir <build-dir> -R 'flx10_(display_protocol|jog_routing)' --output-on-failure`.

Protocol/interface evidence:
[public FLX10 integration notes](https://github.com/Veezuhz/Mixxx_FLX10_Controller_Mapping/blob/main/findings/FLX10-INTEGRATION-NOTES.md)
document interface 5 and unnumbered 128-byte output reports;
[Apple IOHIDDevice API](https://developer.apple.com/documentation/iokit/iohiddevice_h)
defines shared opens and separate report IDs. These sources establish the
transport contract, not vendor-driver compatibility on a particular device.

## Windows x64

Use a Visual Studio 2022 x64 developer shell. Install Qt 6.8.x built for
`win64_msvc2022_64`, CMake and Ninja. Clone vcpkg at the baseline recorded in
`vcpkg.json`, bootstrap it, and set `VCPKG_ROOT`:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
git -C C:\vcpkg checkout d015e31e90838a4c9dfa3eed45979bc70d9357fc
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = 'C:\vcpkg'
```

The CMake toolchain automatically installs the manifest dependencies for the
`x64-windows` triplet:

```powershell
cmake --preset ci-windows-x64
cmake --build --preset ci-windows-x64 --parallel 2
ctest --preset ci-windows-x64
```

Do not combine MinGW Qt with the MSVC build. The CI and supported Windows
configuration consistently use MSVC 2022 and `win64_msvc2022_64` Qt.
On a fresh checkout, install Inkscape and ImageMagick and generate the icon
resources before configuring:

```powershell
choco install inkscape imagemagick --yes
bash ./scripts/generate_icons.sh
```

## Headless validation

Every native binary supports two side-effect-free checks:

```bash
BrockDJ --version
QT_QPA_PLATFORM=offscreen BrockDJ --ci-smoke-test
```

The package smoke test does not open an audio device or write to user data. It
checks the embedded main QML and its imports, then opens and initializes a
SQLite database inside a temporary directory. Packaging jobs run the same test
from the final AppImage or from a freshly extracted macOS/Windows ZIP.
