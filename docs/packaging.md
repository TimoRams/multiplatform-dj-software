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

## Linux AppImage

`scripts/ci/package-appimage.sh` stages a fresh AppDir with `linuxdeploy`, the
Qt plugin and the AppImage output plugin. Those tools use immutable release
tags, and the Type-2 runtime is pinned to release `20251108`; the workflow never
downloads a mutable `continuous` asset while packaging.

Qt deployment scans `src/qml`, so Qt Quick Controls and other imported plugins
are included. A temporary plugin view excludes unrelated system Qt plugins
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

## Windows ZIP

Windows uses MSVC 2022, Qt's `win64_msvc2022_64` package and the pinned vcpkg
manifest. `scripts/ci/package-windows.ps1` runs `windeployqt` with `src/qml`,
then follows `dumpbin /dependents` for every staged PE image, including Qt
platform and QML plugin DLLs. It copies only referenced DLLs from the vcpkg or
Qt runtime directories. Referenced MSVC runtime DLLs are also resolved from
the active toolchain's `VCToolsRedistDir/x64/Microsoft.VC*.CRT` directories,
since `windeployqt` may copy only the redistributable installer. Those DLLs
are bundled app-locally and undergo the same dependency and x64 checks;
an installed runner runtime is not accepted as a substitute. Unresolved
non-system dependencies and non-x64 PE images fail packaging. The script
does not copy the entire vcpkg `bin` directory.

The completed ZIP is extracted into a clean temporary directory and its
`BrockDJ.exe --ci-smoke-test` must pass with Qt/developer paths removed from
`PATH` and the Qt plugin search variables cleared. This checks the exact
uploaded archive, including Qt platform/QML/SQLite deployment, without
requiring an audio device or touching user data.

## Local package checks

The scripts are CI-oriented and expect native dependencies already installed.
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
