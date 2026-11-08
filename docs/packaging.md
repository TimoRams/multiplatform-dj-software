# Packaging and CI artifacts

Each native platform job builds, tests, deploys and smoke-tests its final
package in the same environment. Linux and macOS retain architecture matrices;
Windows uses the same build-and-package job. This avoids intermediate
executable artifacts and repeated dependency provisioning. There is no
`continue-on-error`; a failed platform is a failed workflow.

The workflow is configured to run workflow lint plus all five complete
build/package checks on pull requests. Final package artifacts are uploaded
only for pushes to `main`, `v*` tags and manual `workflow_dispatch` runs;
pull requests exercise deployment and launch the package without publishing
an artifact. This describes the configured checks, not successful remote
workflow results.

Each fresh-checkout build generates its required icon assets before CMake
configuration. Linux uses `librsvg` and ImageMagick, macOS uses Homebrew
`librsvg` and `iconutil`, and Windows provisions Inkscape and ImageMagick.

## Final artifacts

| Platform | GitHub artifact | File |
| --- | --- | --- |
| Linux x86_64 | `BrockDJ-linux-x86_64-AppImage` | `BrockDJ-Linux-x64.AppImage` |
| Linux ARM64 | `BrockDJ-linux-arm64-AppImage` | `BrockDJ-Linux-ARM64.AppImage` |
| macOS Apple Silicon | `BrockDJ-macos-arm64` | `BrockDJ-macOS-Apple-Silicon.zip` |
| macOS Intel | `BrockDJ-macos-x86_64` | `BrockDJ-macOS-Intel.zip` |
| Windows x64 | `BrockDJ-windows-x86_64` | `BrockDJ-Windows-x64.zip` |

Every upload uses `if-no-files-found: error`, and each packaging script rejects
a missing or empty output before upload.

## Stems in CI and packages

All native builds, including the Linux sanitizer build, install the
SHA-256-verified ONNX Runtime SDK and configure with
`-DBROCKDJ_REQUIRE_STEMS=ON`. A missing runtime is an error, not a successful
build with stems silently disabled. Linux x86_64 jobs install Qt Shader Tools
alongside the same Qt version used to build the application.

The five native platform jobs also download the application's HT-Demucs model
with `scripts/ci/fetch-stem-model.sh`, checking the same pinned SHA-256 used by
the in-app downloader. Their `stem_core` test runs real CPU inference on
synthetic audio and checks the resulting stems, not just the fake-model tests.
The sanitizer job covers the stem cache, mixing and waveform tests without
running the large real model under instrumentation.

Normal CTest runs use Qt's Offscreen platform. Linux jobs run the GPU-only
stem-waveform test separately under Xvfb with Qt's XCB plugin and Mesa software
OpenGL. This exercises its shaders without a physical GPU or desktop;
`libxcb-cursor0` is explicitly installed for the Qt 6.8 XCB plugin. macOS and
Windows report a skip for this GPU-only test in Offscreen mode, while its
shaders and C++ code are still compiled. Running it manually without a display
on Linux also reports a skip before Qt initializes a platform plugin.

Scratch quality tests use a controlled callback clock while retaining pointer
sampling/delivery jitter and the existing quality limits. The GPU animation
test similarly controls animation time, so a slow runner cannot skip past
the intermediate frame being checked. UI progress tests wait for the bound
geometry to reach its expected value rather than assuming animations finish
within a fixed wall-clock sleep.

Component fixtures load the same bundled Inter font as the application instead
of measuring platform-dependent fallback fonts. Large audio-test objects are
heap-owned so they also run with Windows' default 1 MiB stack. For plain
streaming sources without lookahead, the keylock worker consumes preallocated
bridge-audio slots continuously; catch-up does not wait for another callback
between batches. The existing 192 kHz recovery limit and realtime-safety
checks remain unchanged. Windows analysis-cache fingerprints include native
file identity and high-resolution change times, including rewrites that restore
the original modification time. The dedicated `streaming_seed_catchup` test
also covers full bridge slots and stale transport generations under ASan/UBSan.
It synchronizes with the worker outside the audio callback; the native
`time_stretch` suite separately retains its full timing and audio-quality checks.

Packages contain ONNX Runtime and its license/third-party notices. The model
is only a CI test input, not part of the uploaded package: users install it
once through **Settings -> DJ / Sync -> Stems -> Download**. The experimental
OpenVINO GPU plugin remains optional and is not bundled.

## Linux AppImage

`scripts/ci/package-appimage.sh` stages a fresh AppDir with `linuxdeploy`, the
Qt plugin and the AppImage output plugin. Those tools use immutable release
tags, and the Type-2 runtime is pinned to release `20251108`; the workflow never
downloads a mutable `continuous` asset while packaging.

Qt deployment scans `src/qml`, so Qt Quick Controls and other imported plugins
are included. System-Qt ARM64 builds explicitly install the Qt Quick Dialogs
QML module used by the sampler editor. A temporary plugin view excludes unrelated
system Qt plugins
whose own optional dependencies are unresolved. Cleanup is confined to that
staging area and the AppDir, and removes only unused SQL drivers; QSQLITE
remains. Nothing is deleted from the runner's global Qt installation. Optional
stripping is disabled because linuxdeploy's embedded binutils may predate
modern ELF `DT_RELR` sections. AppStream metadata is checked with the runner's
`appstreamcli`; the older validator embedded in the pinned appimagetool is
disabled. The final AppImage is extracted with its Type-2 runtime and its
`AppRun` launches with developer library/plugin search paths removed, avoiding
any FUSE requirement in CI. Packaging fails unless QSQLITE, SQLCipher and
every deployed ELF shared object have a complete runtime closure. Dependencies
may resolve from the AppDir or from the explicit Linux ABI/desktop-driver
allowlist; accidental reliance on other runner-installed libraries fails.
The allowlist includes the exact `libgmp.so.10` base-system ABI, which
linuxdeploy deliberately excludes to avoid GnuTLS/GMP symbol conflicts.
GnuTLS and its other non-allowlisted dependencies still have to be bundled.

SQLCipher is deliberately loaded privately with `dlopen`, not linked into
BrockDJ: linking its SQLite symbols alongside Qt's system SQLite causes symbol
collisions. CMake writes the selected shared-library path and SONAME to
`BrockDJ.sqlcipher-runtime` beside the executable. Packaging requires this
build metadata, verifies the ABI, and explicitly deploys SQLCipher and its
dependencies with `linuxdeploy --library`. The exact configured SONAME must
exist in `AppDir/usr/lib`; its dependency closure is checked like every other
shared object. When linuxdeploy retains the unversioned input filename, the
script creates a relative SONAME symlink inside AppDir. Reconfigure older
builds before packaging them.

## macOS bundles

CMake creates `BrockDJ.app` directly for both architectures. The package job:

1. copies and rewrites non-Qt Homebrew dependencies with `dylibbundler`,
   overwriting individual libraries rather than deleting `Frameworks`;
2. runs `macdeployqt` with the repository QML source directory and an isolated
   plugin view containing only QSQLITE in the SQL-driver category; other
   plugin categories and the installed Qt tree remain unchanged;
3. resolves every bundled Mach-O dependency through its loader/executable
   paths and runpaths, rejecting missing or external non-system libraries
   (including remaining Homebrew/runner references);
4. verifies the requested architecture on the app and every bundled Mach-O
   file using `lipo`;
5. applies an ad-hoc recursive signature and verifies it with `codesign`;
6. creates the ZIP with `ditto`, extracts that exact ZIP into a clean temporary
   directory, verifies its signature again, and runs its executable smoke test
   with Qt/developer library search paths removed.

Ad-hoc signing makes the bundle structurally verifiable but is not Apple
notarization. A future public release can add Developer ID signing and notary
credentials without changing the build/package separation.

The isolated SQL-driver view avoids deploying unused PostgreSQL, ODBC and
Mimer plugins linked to vendor SDKs absent from the runner. It does not remove
SQLite or SQLCipher, and does not install those unrelated database servers.
`macdeployqt` normally deploys only the Cocoa platform plugin. The script also
copies Qt's Offscreen plugin and supplies it as an additional executable to
`macdeployqt` so its framework references are rewritten. Both plugins are
required before the common architecture, dependency and signature checks.
The extracted ZIP smoke uses that bundled Offscreen plugin, not the runner's.
The native bundler receives explicit ONNX SDK, Qt and bundle search directories
to resolve dependencies such as `@rpath/libonnxruntime.1.dylib`. Any remaining
interactive lookup receives `quit`, so a missing dependency fails immediately
instead of looping on CI's closed standard input. Architecture, dependency
closure and final signature checks remain mandatory.

## Windows ZIP

Windows uses MSVC 2022, Qt's `win64_msvc2022_64` package and the pinned vcpkg
manifest. `scripts/ci/package-windows.ps1` runs `windeployqt` with `src/qml`,
excluding the SQL-driver category and copying only the required QSQLITE driver
from the same Qt installation. Unused Mimer, ODBC and PostgreSQL drivers and
their vendor SDK dependencies are not packaged. Offscreen is explicitly
included alongside the Windows platform plugin for the extracted ZIP smoke;
all three required plugins are checked before dependency inspection.
The script then follows `dumpbin /dependents` for every staged PE image, including Qt
platform and QML plugin DLLs. It copies only referenced DLLs from the vcpkg or
Qt runtime directories. Referenced MSVC runtime DLLs are also resolved from
the active toolchain's `VCToolsRedistDir/x64/Microsoft.VC*.CRT` directories,
since `windeployqt` may copy only the redistributable installer. Those DLLs
are bundled app-locally and undergo the same dependency and x64 checks;
an installed runner runtime is not accepted as a substitute. Unresolved
non-system dependencies and non-x64 PE images fail packaging. The script
does not copy the entire vcpkg `bin` directory.
The unused `vc_redist.x64.exe` installer is removed from staging before PE
inspection; its bootstrap architecture is not the runtime DLL architecture.
No application executable or DLL is excluded from the x64 checks.

The completed ZIP is extracted into a clean temporary directory and its
`BrockDJ.exe --ci-smoke-test` must pass with Qt/developer paths removed from
`PATH` and the Qt plugin search variables cleared. This checks the exact
uploaded archive, including Qt platform/QML/SQLite deployment, without
requiring an audio device or touching user data.

## Local package checks

The scripts are CI-oriented and expect native dependencies already installed.
Workflow lint also runs the isolated script regressions in `tests/packaging`.
Tool fixtures check SQLCipher deployment and ABI/error handling, and the
macOS bundler's explicit search paths and fail-fast input.
These script regressions do not replace native package smoke tests.
Typical invocations are:

```bash
mkdir -p dist
```

```bash
./scripts/ci/package-appimage.sh build-release/bin/BrockDJ dist/BrockDJ-Linux-x64.AppImage
```

```bash
./scripts/ci/package-macos.sh build-ci-macos-arm64/bin/BrockDJ.app arm64 dist/BrockDJ-macOS-Apple-Silicon.zip
```

```powershell
./scripts/ci/package-windows.ps1 `
  -Executable build-ci-windows-x64/bin/BrockDJ.exe `
  -VcpkgBin build-ci-windows-x64/vcpkg_installed/x64-windows/bin `
  -QmlDir src/qml `
  -Output dist/BrockDJ-Windows-x64.zip
```
