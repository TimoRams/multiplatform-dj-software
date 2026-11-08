# Installation and quick build

[← Overview](../README.md) · [Detailed build guide](building.md)


There are no prebuilt releases yet, so BrockDJ is built from source with plain
CMake. You need:

- a C++23 compiler (GCC 13+, or Clang from a recent Xcode)
- CMake 3.28+ and, optionally, Ninja
- `patch` (used to prepare ONNX Runtime dependencies; a fallback executable is included with ONNX Runtime on Windows)
- Python 3.10+ (used by the ONNX Runtime source build)
- Qt 6.4+ including Qt Shader Tools

Clone the repository with its submodules:

```bash
git clone --recurse-submodules https://github.com/TimoRams/multiplatform-dj-software.git
cd multiplatform-dj-software
```

ONNX Runtime is built automatically from the pinned `libs/onnxruntime-src`
submodule for the selected platform. The first build needs network access for
its dependencies and takes longer; no runtime SDK download is needed.
After starting BrockDJ, open **Settings → Stems → Install model** to install
the separate 166 MB model. See [Stems](stems.md).

### Linux

**1. Install the dependencies.**

Fedora:

```bash
sudo dnf install -y gcc-c++ python3 patch cmake ninja-build pkgconf-pkg-config curl \
  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtshadertools-devel \
  alsa-lib-devel jack-audio-connection-kit-devel libusb1-devel \
  taglib-devel rubberband-devel sqlcipher-devel fftw-devel
```

Ubuntu 24.04:

```bash
sudo apt install -y build-essential python3 patch cmake ninja-build pkg-config curl \
  qt6-base-dev qt6-declarative-dev qt6-shadertools-dev qt6-shader-baker libqt6sql6-sqlite \
  qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs qml6-module-qtquick-layouts \
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
cmake -S . -B build -G Ninja
cmake --build build
./build/bin/BrockDJ
```

### macOS

**1. Install the dependencies** with the Xcode command-line tools
(`xcode-select --install`) and [Homebrew](https://brew.sh):

```bash
brew install python cmake ninja pkg-config qt taglib rubberband libkeyfinder sqlcipher librsvg imagemagick
export CMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

**2. Build and run.**

```bash
cmake -S . -B build -G Ninja
cmake --build build
open build/bin/BrockDJ.app
```

The app is built for your Mac's architecture (Apple Silicon or Intel) and needs
macOS 13 or newer.

### Updating

```bash
git pull
git submodule sync --recursive
git submodule update --init --recursive
cmake -S . -B build
cmake --build build
```

### Build options

| Option | Effect |
| --- | --- |
| *(default)* | `RelWithDebInfo`: optimized, with debug symbols |
| `-DCMAKE_BUILD_TYPE=Release` | Release build without debug symbols |
| `-DBUILD_TESTING=ON` | Also build the test suite |
| `-DBROCKDJ_REQUIRE_STEMS=ON` | Require stems support |
| `-DBROCKDJ_ENABLE_STEMS=OFF` | Build without stems |

Run the tests in a separate build directory:

```bash
cmake -S . -B build-tests -G Ninja -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

More details, including the FLX10 setup and the CI presets, are in the
[build guide](building.md).
