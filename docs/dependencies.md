# Dependency policy

## Language and toolchain

- CMake 3.24 or newer (required by the upstream Signalsmith Stretch CMake target).
- C++23 is the portable application and test baseline.
- Supported CI compilers are GCC on Ubuntu, AppleClang/Xcode on macOS and MSVC
  2022 on Windows.
- `BROCKDJ_ENABLE_NATIVE_ARCH` is off in all CI and release presets. Local
  `-march=native` remains an explicit opt-in.
- Native threads are linked explicitly through `Threads::Threads` for every
  BrockDJ executable and test target.

No C++26-only library or language feature is currently required. Raising the
baseline must be a deliberate cross-platform change with all five native CI
jobs green; it must not be introduced as a global experiment.

## Qt

The required Qt 6 components are:

- Core
- Gui
- Qml
- Quick
- QuickControls2
- Sql, including the QSQLITE driver
- Concurrent

Qt WebEngine, Qt PDF, Qt Quick3D, Qt Virtual Keyboard and other large optional
modules are not part of BrockDJ's dependency surface or deployment.

CI uses Qt 6.8.3 for Linux x86_64, both macOS architectures and Windows x64.
Linux ARM64 uses the native Ubuntu 24.04 Qt packages because the Qt online
installer does not provide an equivalent Linux desktop ARM64 archive.

## Audio and metadata libraries

| Dependency | Resolution | Purpose |
| --- | --- | --- |
| JUCE 8.0.13 | submodule commit `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2` | audio devices, codecs and DSP |
| Ableton Link 4.0 | submodule commit `e9a2e414d63f55f1aad158370b007a6fbdc1eeb9` | network tempo/phase sync |
| Signalsmith DSP v1.7.1 | submodule commit `2d20161915e733f117545c6be8cd3275a739a1e3` | header-only delay/filter DSP; MIT |
| Signalsmith Linear 0.5.0 | submodule commit `0dd6b823783f1fe8768e2700e0937903f4270698` | Stretch's local FFT/linear dependency; MIT |
| Signalsmith Stretch | submodule commit `57b93f4e9206a089a45387eaa39bdc9f310d3308` | default key-lock time stretching; MIT |
| TagLib | distro/Homebrew/vcpkg | metadata and cover extraction |
| libkeyfinder 2.2.8 | distro/Homebrew/vcpkg | musical-key analysis |
| RubberBand | distro/Homebrew/vcpkg | selectable compatibility key-lock backend |
| SQLCipher | distro/Homebrew | read-only Rekordbox USB nickname and colour access |
| ALSA | Linux only, required | native audio/MIDI backend |
| JACK | Linux only, optional when discovered | JACK audio backend |
| libusb | Linux only, optional when discovered | direct FLX10 HID support |

The repository records the JUCE, Link and Signalsmith commits in Git. CI always
checks out submodules recursively and never tracks their branches. A regular
`git pull` does not update submodule working trees; after a Gitlink changes,
run `git submodule sync --recursive` and `git submodule update --init --recursive`.
`git submodule update --remote` is never part of the normal build process.

libkeyfinder 2.2.8 corresponds to upstream commit
`b33b5a88e04a5182dd19c38c57762925631118fd`. Windows resolves that release via
the pinned vcpkg registry baseline; the official vcpkg port is available on
all triplets and depends on FFTW3. Therefore the workflow does not maintain a
second ad-hoc source build. If that port disappears in a future registry,
fallback builds must use that exact commit, `BUILD_TESTING=OFF`, and an
isolated install prefix included through `CMAKE_PREFIX_PATH`.

## Windows manifest

`vcpkg.json` is the only Windows third-party manifest. Its immutable registry
baseline is:

```text
d015e31e90838a4c9dfa3eed45979bc70d9357fc  (vcpkg 2026.05.25)
```

It resolves only `libkeyfinder`, `rubberband` and `taglib`; their transitive
runtime DLLs are handled by vcpkg. Qt is deliberately supplied by the matching
official MSVC Qt package, not by vcpkg, so one Qt ABI is used throughout.

## Updating dependencies

When updating Qt, a submodule or the vcpkg baseline:

1. Update the pin and this document together.
2. Configure, build and run CTest on every native CI platform.
3. Run the deployed package smoke test, not only the build-tree executable.
4. Inspect packaged runtime dependencies (`ldd`/AppDir, `otool -L`, or
   `dumpbin /dependents`) and reject host-specific absolute paths.
