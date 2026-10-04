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

## Linux

Ubuntu 24.04 dependencies:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake curl ninja-build ccache pkg-config \
  qt6-base-dev qt6-declarative-dev libqt6sql6-sqlite \
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
The application explicitly includes the embedded `qrc:/` module import root.
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
