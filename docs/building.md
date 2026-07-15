# Building BrockDJ

BrockDJ uses CMake 3.22 or newer, Ninja and a portable C++23 baseline. JUCE and
Ableton Link are Git submodules, so clone with `--recurse-submodules` or run:

```bash
git submodule update --init --recursive
```

The supported native configurations are Linux x86_64, Linux ARM64, macOS
Apple Silicon, macOS Intel and Windows x64. CI builds and tests all five on
every pull request, main-branch push, version tag and manual dispatch.

## Linux

Ubuntu 24.04 dependencies:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build ccache pkg-config \
  qt6-base-dev qt6-declarative-dev \
  qml6-module-qtqml-workerscript qml6-module-qtquick \
  qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-window \
  libasound2-dev libjack-jackd2-dev libusb-1.0-0-dev \
  libtag1-dev libkeyfinder-dev librubberband-dev \
  libfontconfig1-dev libfreetype6-dev libgl1-mesa-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libxkbcommon-x11-dev
```

For normal local development, use the canonical fast build:

```bash
./build-fast
ctest --preset linux-dev-fast
./build/bin/BrockDJ
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

The ARM64 preset is a native preset. It is not a cross-compilation toolchain;
run it on an ARM64 host, as CI does with `ubuntu-24.04-arm`.

## macOS

Install Xcode command-line tools, CMake/Ninja, Qt and the native audio-analysis
dependencies:

```bash
brew install cmake ninja ccache pkg-config qt@6 taglib rubberband libkeyfinder
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
